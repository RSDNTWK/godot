#pragma once

#include "ffmpeg_frame_scaler.h"
#include "ffmpeg_packed_frame.h"
#include "core/error/error_macros.h"
#include "core/io/image.h"
#include "core/math/math_funcs.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/string/print_string.h"

// One decode owner submits referenced NV12 frames. A separate CPU worker owns
// the scaler; only immutable Images return to the decode owner. Two outstanding
// jobs bound both latency and storage, and the worker never touches a demuxer,
// decoder, audio clock or rendering resource.
// All public calls and destruction have one serialized decode/lifecycle owner.
// stop() joins the current bounded conversion; it is not an instant interrupt.
class FFmpegFrameConverter {
public:
	struct Result {
		Ref<Image> image;
		double position = 0.0;
		Error error = OK;
		uint64_t scale_usec = 0, total_usec = 0;
	};

private:
	static constexpr int CAPACITY = 2;
	struct Job {
		AVFrame *frame = nullptr;
		double position = 0.0;
		uint64_t generation = 0;
	};
	mutable Mutex mutex;
	Semaphore available;
	Thread thread;
	Job inputs[CAPACITY];
	Result outputs[CAPACITY];
	int input_head = 0, input_count = 0;
	int output_head = 0, output_count = 0;
	bool processing = false;
	bool quitting = false;
	bool wake_pending = false;
	uint64_t wake_count = 0;
	uint64_t generation = 0;
	int workers = 2;
	void (*work_gate)(void *) = nullptr;
	void *work_gate_userdata = nullptr;

	static void run(void *p_self) { static_cast<FFmpegFrameConverter *>(p_self)->convert(); }
	void convert() {
		Thread::set_name("FFmpegConversion");
		FFmpegFrameScaler scaler(workers);
		AVFrame *destination = av_frame_alloc();
		for (;;) {
			available.wait();
			Job job;
			{
				MutexLock lock(mutex);
				wake_pending = false;
				wake_count++;
				if (quitting) {
					break;
				}
				if (!input_count) {
					continue;
				}
				job = inputs[input_head];
				inputs[input_head] = Job();
				input_head = (input_head + 1) % CAPACITY;
				input_count--;
				processing = true;
			}
			Result result;
			result.position = job.position;
			if (work_gate) {
				work_gate(work_gate_userdata);
			}
			const bool measure = is_print_verbose_enabled();
			const uint64_t began = measure ? OS::get_singleton()->get_ticks_usec() : 0;
			Vector<uint8_t> pixels;
			result.error = destination ? FFmpegPackedFrame::prepare(destination, job.frame->width, job.frame->height, pixels) : ERR_OUT_OF_MEMORY;
			if (result.error == OK) {
				const uint64_t scale_begin = measure ? OS::get_singleton()->get_ticks_usec() : 0;
				const int converted = scaler.convert(job.frame, destination);
				result.scale_usec = measure ? OS::get_singleton()->get_ticks_usec() - scale_begin : 0;
				result.error = converted < 0 ? (converted == AVERROR(ENOMEM) ? ERR_OUT_OF_MEMORY : ERR_INVALID_DATA) : FFmpegPackedFrame::finish(destination, pixels);
			}
			if (result.error == OK) {
				result.image = Image::create_from_data(job.frame->width, job.frame->height, false, Image::FORMAT_RGBA8, pixels);
				if (result.image.is_null() || result.image->is_empty()) {
					result.error = ERR_OUT_OF_MEMORY;
				}
			}
			if (destination) {
				av_frame_unref(destination);
			}
			av_frame_free(&job.frame);
			result.total_usec = measure ? OS::get_singleton()->get_ticks_usec() - began : 0;
			{
				MutexLock lock(mutex);
				processing = false;
				if (!quitting && job.generation == generation) {
					// Input admission counts queued, active and completed jobs.
					DEV_ASSERT(output_count < CAPACITY);
					outputs[(output_head + output_count) % CAPACITY] = result;
					output_count++;
				}
				if (!quitting && input_count > 0 && !wake_pending) {
					wake_pending = true;
					available.post();
				}
			}
		}
		av_frame_free(&destination);
	}

public:
	// Configure before the first submission; used to control worker scheduling
	// in lifetime tests. Production never installs a work gate.
	void set_work_gate(void (*p_callback)(void *), void *p_userdata) {
		MutexLock lock(mutex);
		ERR_FAIL_COND(thread.is_started());
		work_gate = p_callback;
		work_gate_userdata = p_userdata;
	}
	static bool supports(const AVFrame *p_frame) {
		return p_frame && (p_frame->format == AV_PIX_FMT_NV12 || (p_frame->format == AV_PIX_FMT_YUV420P && p_frame->data[2])) && p_frame->data[0] && p_frame->data[1] &&
				FFmpegPackedFrame::supports(p_frame->width, p_frame->height) && int64_t(p_frame->width) * p_frame->height <= 1920 * 1080;
	}
	Error submit(const AVFrame *p_frame, double p_position) {
		if (!supports(p_frame) || !Math::is_finite(p_position)) {
			return ERR_INVALID_PARAMETER;
		}
		MutexLock lock(mutex);
		if (quitting) {
			return ERR_UNAVAILABLE;
		}
		if (input_count + output_count + int(processing) >= CAPACITY) {
			return ERR_BUSY;
		}
		AVFrame *copy = av_frame_clone(p_frame);
		if (!copy) {
			return ERR_OUT_OF_MEMORY;
		}
		if (!thread.is_started() && thread.start(run, this) == Thread::UNASSIGNED_ID) {
			av_frame_free(&copy);
			return ERR_CANT_CREATE;
		}
		inputs[(input_head + input_count) % CAPACITY] = Job{ copy, p_position, generation };
		input_count++;
		// Coalesce notifications. Repeated clear/replace cannot accumulate
		// unbounded semaphore credits while the worker is converting.
		if (!processing && !wake_pending) {
			wake_pending = true;
			available.post();
		}
		return OK;
	}
	bool take(Result &r_result) {
		MutexLock lock(mutex);
		if (!output_count) {
			return false;
		}
		r_result = outputs[output_head];
		outputs[output_head] = Result();
		output_head = (output_head + 1) % CAPACITY;
		output_count--;
		return true;
	}
	int outstanding() const {
		MutexLock lock(mutex);
		return input_count + output_count + int(processing);
	}
	uint64_t get_wake_count() const {
		MutexLock lock(mutex);
		return wake_count;
	}
	void clear() {
		MutexLock lock(mutex);
		generation++;
		for (Job &job : inputs) {
			av_frame_free(&job.frame);
			job = Job();
		}
		for (Result &result : outputs) {
			result = Result();
		}
		input_head = input_count = output_head = output_count = 0;
	}
	void stop() {
		{
			MutexLock lock(mutex);
			quitting = true;
			if (!wake_pending) {
				wake_pending = true;
				available.post();
			}
		}
		if (thread.is_started()) {
			thread.wait_to_finish();
		}
		clear();
		while (available.try_wait()) {}
		MutexLock lock(mutex);
		quitting = false;
		wake_pending = false;
	}
	~FFmpegFrameConverter() { stop(); }
};
