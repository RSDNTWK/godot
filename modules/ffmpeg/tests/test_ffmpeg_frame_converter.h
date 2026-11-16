#pragma once

#include "modules/ffmpeg/ffmpeg_frame_converter.h"
#include "core/templates/safe_refcount.h"
#include "tests/test_macros.h"
#include <atomic>

namespace TestFFmpegFrameConverter {

struct OwnedInput {
	std::atomic<int> released{ 0 };
	static void release(void *p_owner, uint8_t *p_data) {
		av_free(p_data);
		static_cast<OwnedInput *>(p_owner)->released.fetch_add(1);
	}
	AVFrame *make(int p_value) {
		AVFrame *frame = av_frame_alloc();
		REQUIRE(frame);
		uint8_t *pixels = static_cast<uint8_t *>(av_malloc(64 * 48 + 64));
		REQUIRE(pixels);
		frame->buf[0] = av_buffer_create(pixels, 64 * 48 + 64, release, this, 0);
		REQUIRE(frame->buf[0]);
		frame->format = AV_PIX_FMT_NV12;
		frame->width = 64;
		frame->height = 32;
		frame->data[0] = pixels;
		frame->data[1] = pixels + 64 * 32;
		frame->linesize[0] = frame->linesize[1] = 64;
		for (int y = 0; y < 48; ++y) {
			for (int x = 0; x < 64; ++x) {
				pixels[y * 64 + x] = uint8_t((x * 17 + y * 31 + p_value) & 255);
			}
		}
		return frame;
	}
};

struct WorkGate {
	Semaphore entered;
	SafeFlag release;
	static void block(void *p_self) {
		WorkGate *self = static_cast<WorkGate *>(p_self);
		self->entered.post();
		const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
		while (!self->release.is_set() && OS::get_singleton()->get_ticks_usec() < deadline) {
			OS::get_singleton()->delay_usec(1000);
		}
	}
	void wait_entered() {
		const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 1000000;
		while (!entered.try_wait()) {
			REQUIRE(OS::get_singleton()->get_ticks_usec() < deadline);
			OS::get_singleton()->delay_usec(1000);
		}
	}
};

static Vector<uint8_t> reference(AVFrame *p_frame) {
	SwsContext *scale = sws_getContext(64, 32, AV_PIX_FMT_NV12, 64, 32, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
	REQUIRE(scale);
	Vector<uint8_t> pixels;
	REQUIRE(pixels.resize(64 * 32 * 4 + 64) == OK);
	uint8_t *planes[4] = { pixels.ptrw(), nullptr, nullptr, nullptr };
	int strides[4] = { 64 * 4, 0, 0, 0 };
	REQUIRE(sws_scale(scale, p_frame->data, p_frame->linesize, 0, 32, planes, strides) == 32);
	sws_freeContext(scale);
	pixels.resize(64 * 32 * 4);
	return pixels;
}

static FFmpegFrameConverter::Result take(FFmpegFrameConverter &p_converter) {
	FFmpegFrameConverter::Result result;
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 2000000;
	while (!p_converter.take(result)) {
		REQUIRE(OS::get_singleton()->get_ticks_usec() < deadline);
		OS::get_singleton()->delay_usec(1000);
	}
	REQUIRE(result.error == OK);
	REQUIRE(result.image.is_valid());
	return result;
}

static void compare(const Ref<Image> &p_image, const Vector<uint8_t> &p_expected) {
	const Vector<uint8_t> actual = p_image->get_data();
	REQUIRE(actual.size() == p_expected.size());
	int maximum = 0;
	for (int i = 0; i < actual.size(); ++i) {
		const int delta = int(actual[i]) - int(p_expected[i]);
		maximum = MAX(maximum, delta < 0 ? -delta : delta);
	}
	CHECK(maximum <= 1);
}

TEST_CASE("[NativeStreaming][FrameConversion] Two owned jobs preserve FIFO pixels and restart without stale output") {
	OwnedInput first, second, rejected, restarted;
	WorkGate gate;
	FFmpegFrameConverter converter;
	converter.set_work_gate(WorkGate::block, &gate);
	AVFrame *source = first.make(13);
	const Vector<uint8_t> first_pixels = reference(source);
	REQUIRE(converter.submit(source, 1.0) == OK);
	av_frame_free(&source);
	gate.wait_entered();
	CHECK(first.released.load() == 0);
	source = second.make(97);
	const Vector<uint8_t> second_pixels = reference(source);
	REQUIRE(converter.submit(source, 2.0) == OK);
	av_frame_free(&source);
	CHECK(converter.outstanding() == 2);
	source = rejected.make(71);
	CHECK(converter.submit(source, 3.0) == ERR_BUSY);
	av_frame_free(&source);
	CHECK(rejected.released.load() == 1);
	CHECK(second.released.load() == 0);
	gate.release.set();
	const FFmpegFrameConverter::Result a = take(converter);
	const FFmpegFrameConverter::Result b = take(converter);
	CHECK(a.position == 1.0);
	CHECK(b.position == 2.0);
	compare(a.image, first_pixels);
	compare(b.image, second_pixels);
	CHECK(first.released.load() == 1);
	CHECK(second.released.load() == 1);
	CHECK(converter.outstanding() == 0);
	converter.stop();
	source = restarted.make(33);
	const Vector<uint8_t> third_pixels = reference(source);
	REQUIRE(converter.submit(source, 9.0) == OK);
	av_frame_free(&source);
	const FFmpegFrameConverter::Result c = take(converter);
	CHECK(c.position == 9.0);
	compare(c.image, third_pixels);
	compare(a.image, first_pixels);
	CHECK(restarted.released.load() == 1);
	CHECK(converter.outstanding() == 0);
}

TEST_CASE("[NativeStreaming][FrameConversion] Replacing active work drops old generations without accumulating wake credits") {
	OwnedInput active, queued, replacements, current;
	WorkGate gate;
	FFmpegFrameConverter converter;
	converter.set_work_gate(WorkGate::block, &gate);
	AVFrame *source = active.make(11);
	REQUIRE(converter.submit(source, 1.0) == OK);
	av_frame_free(&source);
	gate.wait_entered();
	source = queued.make(22);
	REQUIRE(converter.submit(source, 2.0) == OK);
	av_frame_free(&source);
	converter.clear();
	CHECK(active.released.load() == 0);
	CHECK(queued.released.load() == 1);
	const uint64_t wake_before = converter.get_wake_count();
	for (int i = 0; i < 1000; ++i) {
		source = replacements.make(i);
		REQUIRE(converter.submit(source, 3.0 + i) == OK);
		av_frame_free(&source);
		converter.clear();
		CHECK(converter.outstanding() == 1);
	}
	CHECK(replacements.released.load() == 1000);
	source = current.make(55);
	const Vector<uint8_t> expected = reference(source);
	REQUIRE(converter.submit(source, 77.0) == OK);
	av_frame_free(&source);
	gate.release.set();
	const FFmpegFrameConverter::Result result = take(converter);
	CHECK(result.position == 77.0);
	compare(result.image, expected);
	CHECK(active.released.load() == 1);
	CHECK(current.released.load() == 1);
	CHECK(converter.outstanding() == 0);
	converter.stop();
	CHECK(converter.get_wake_count() - wake_before <= 3);
	FFmpegFrameConverter::Result extra;
	CHECK_FALSE(converter.take(extra));
}

TEST_CASE("[NativeStreaming][FrameConversion] Stopping active and queued work joins before releasing their owners") {
	OwnedInput active, queued;
	WorkGate gate;
	FFmpegFrameConverter converter;
	converter.set_work_gate(WorkGate::block, &gate);
	AVFrame *source = active.make(1);
	REQUIRE(converter.submit(source, 1.0) == OK);
	av_frame_free(&source);
	gate.wait_entered();
	source = queued.make(2);
	REQUIRE(converter.submit(source, 2.0) == OK);
	av_frame_free(&source);
	CHECK(active.released.load() == 0);
	CHECK(queued.released.load() == 0);
	Thread release;
	REQUIRE(release.start([](void *p_gate) {
		OS::get_singleton()->delay_usec(5000);
		static_cast<WorkGate *>(p_gate)->release.set();
	}, &gate) != Thread::UNASSIGNED_ID);
	converter.stop();
	release.wait_to_finish();
	CHECK(active.released.load() == 1);
	CHECK(queued.released.load() == 1);
	CHECK(converter.outstanding() == 0);
	FFmpegFrameConverter::Result result;
	CHECK_FALSE(converter.take(result));
}

} // namespace TestFFmpegFrameConverter
