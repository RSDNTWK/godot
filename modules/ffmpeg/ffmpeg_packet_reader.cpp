/**************************************************************************/
/*  ffmpeg_packet_reader.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "ffmpeg_packet_reader.h"

#ifdef MODULE_MEDIA_STREAM_ENABLED

#include "core/os/os.h"

#include <chrono>
#include <cmath>

bool FFmpegPacketReader::_interrupted() const {
	return stop_requested.is_set() || (external_cancel && external_cancel->is_set());
}

int64_t FFmpegPacketReader::_packet_bytes(const AVPacket *p_packet) {
	if (p_packet->size < 0 || p_packet->side_data_elems < 0) {
		return -1;
	}
	// Account backing buffers, not only slices of their payload, and side-data
	// allocations including their required padding and descriptors. Packet/queue
	// metadata is additionally bounded by MAX_PACKETS. av_read_frame may allocate
	// ONE in-flight packet (and demuxer-internal buffers) before we can inspect
	// its size; the queue limit is not a limit on those FFmpeg allocations.
	uint64_t bytes = uint64_t(p_packet->size);
	if (p_packet->buf && p_packet->buf->size > bytes) {
		bytes = p_packet->buf->size;
	}
	if (bytes > uint64_t(MAX_BYTES) || uint64_t(p_packet->side_data_elems) > uint64_t(MAX_BYTES) / sizeof(AVPacketSideData)) {
		return -1;
	}
	bytes += uint64_t(p_packet->side_data_elems) * sizeof(AVPacketSideData);
	for (int i = 0; i < p_packet->side_data_elems; ++i) {
		const size_t size = p_packet->side_data[i].size;
		if (bytes > uint64_t(MAX_BYTES) || size > uint64_t(MAX_BYTES) - bytes || AV_INPUT_BUFFER_PADDING_SIZE > uint64_t(MAX_BYTES) - bytes - size) {
			return -1;
		}
		bytes += size + AV_INPUT_BUFFER_PADDING_SIZE;
	}
	return bytes <= uint64_t(MAX_BYTES) ? int64_t(bytes) : -1;
}

void FFmpegPacketReader::_set_terminal(int p_error) {
	MutexLock lock(mutex);
	terminal_error = p_error < 0 ? p_error : AVERROR_EOF;
	capacity_pressure = false;
}

void FFmpegPacketReader::_run(void *p_self) {
	static_cast<FFmpegPacketReader *>(p_self)->_produce();
}

void FFmpegPacketReader::_produce() {
#ifdef THREADS_ENABLED
	AVPacket *packet = av_packet_alloc();
	if (!packet) {
		_set_terminal(AVERROR(ENOMEM));
		return;
	}
	int result = 0;
	while (!_interrupted()) {
#ifdef FFMPEG_TESTS_ENABLED
		result = test_read ? test_read(test_userdata, packet, &io_stop_requested, external_cancel) : av_read_frame(format, packet);
#else
		result = av_read_frame(format, packet);
#endif
		if (_interrupted()) {
			result = AVERROR_EXIT;
			break;
		}
		if (result == AVERROR(EAGAIN)) {
			av_packet_unref(packet);
			OS::get_singleton()->delay_usec(1000);
			continue;
		}
		if (result < 0) {
			break;
		}
		if (packet->stream_index != selected_stream) {
			av_packet_unref(packet);
			continue;
		}
		const int64_t bytes = _packet_bytes(packet);
		if (bytes < 0) {
			result = AVERROR(ENOBUFS);
			break;
		}
		{
			MutexLock lock(mutex);
			while (!_interrupted() && (count == MAX_PACKETS || bytes > MAX_BYTES - queued_bytes)) {
				capacity_pressure = true;
				// stop/read notify immediately. The bounded wait also observes an
				// external generation-cancel flag, which cannot notify this mutex.
				capacity_changed.wait_for(lock._get_lock(), std::chrono::milliseconds(10));
			}
			capacity_pressure = false;
			if (_interrupted()) {
				result = AVERROR_EXIT;
				break;
			}
			QueuedPacket &entry = queue[(head + count) % MAX_PACKETS];
			entry.packet = packet;
			entry.bytes = bytes;
			const int64_t timestamp = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
			entry.has_time = timestamp != AV_NOPTS_VALUE;
			if (entry.has_time) {
				entry.start = double(timestamp) * av_q2d(time_base);
				entry.end = entry.start + double(packet->duration > 0 ? packet->duration : 0) * av_q2d(time_base);
				entry.has_time = std::isfinite(entry.start) && std::isfinite(entry.end);
			}
			queued_bytes += bytes;
			++count;
		}
		packet = av_packet_alloc();
		if (!packet) {
			result = AVERROR(ENOMEM);
			break;
		}
	}
	av_packet_free(&packet);
	_set_terminal(_interrupted() ? AVERROR_EXIT : result);
#endif
}

Error FFmpegPacketReader::_start_worker() {
#ifdef THREADS_ENABLED
	if (stop_requested.is_set() || (external_cancel && external_cancel->is_set())) {
		_set_terminal(AVERROR_EXIT);
		return ERR_CANT_CREATE;
	}
	worker.start(_run, this);
	if (!worker.is_started()) {
		_set_terminal(AVERROR(ENOMEM));
		return ERR_CANT_CREATE;
	}
	return OK;
#else
	_set_terminal(AVERROR(ENOSYS));
	return ERR_UNAVAILABLE;
#endif
}

Error FFmpegPacketReader::start(AVFormatContext *p_format, int p_selected_stream, const SafeFlag *p_external_cancel) {
	if (worker.is_started()) {
		return ERR_ALREADY_IN_USE;
	}
	if (!p_format || p_selected_stream < 0 || unsigned(p_selected_stream) >= p_format->nb_streams || !p_format->streams[p_selected_stream] || p_format->streams[p_selected_stream]->time_base.num <= 0 || p_format->streams[p_selected_stream]->time_base.den <= 0) {
		return ERR_INVALID_PARAMETER;
	}
	format = p_format;
	selected_stream = p_selected_stream;
	time_base = p_format->streams[p_selected_stream]->time_base;
	external_cancel = p_external_cancel;
	return _start_worker();
}

#ifdef FFMPEG_TESTS_ENABLED
Error FFmpegPacketReader::start_for_test(TestReadCallback p_read, void *p_userdata, int p_selected_stream, AVRational p_time_base, const SafeFlag *p_external_cancel) {
	if (worker.is_started()) {
		return ERR_ALREADY_IN_USE;
	}
	if (!p_read || p_selected_stream < 0 || p_time_base.num <= 0 || p_time_base.den <= 0) {
		return ERR_INVALID_PARAMETER;
	}
	test_read = p_read;
	test_userdata = p_userdata;
	selected_stream = p_selected_stream;
	time_base = p_time_base;
	external_cancel = p_external_cancel;
	return _start_worker();
}
#endif

int FFmpegPacketReader::read(AVPacket *r_packet) {
	if (!r_packet) {
		return AVERROR(EINVAL);
	}
	MutexLock lock(mutex);
	if (count == 0) {
		return terminal_error < 0 ? terminal_error : AVERROR(EAGAIN);
	}
	QueuedPacket &entry = queue[head];
	av_packet_unref(r_packet);
	av_packet_move_ref(r_packet, entry.packet);
	av_packet_free(&entry.packet);
	queued_bytes -= entry.bytes;
	entry = QueuedPacket();
	head = (head + 1) % MAX_PACKETS;
	--count;
#ifdef THREADS_ENABLED
	capacity_changed.notify_one();
#endif
	return 0;
}

bool FFmpegPacketReader::has_prefill(double p_seconds) const {
	MutexLock lock(mutex);
	if (count == 0) {
		return false;
	}
	if (capacity_pressure || count == MAX_PACKETS || queued_bytes == MAX_BYTES || terminal_error < 0) {
		return true;
	}
	if (!std::isfinite(p_seconds) || p_seconds < 0.0) {
		return false;
	}
	bool found_time = false;
	double first = 0.0;
	double last = 0.0;
	for (int i = 0; i < count; ++i) {
		const QueuedPacket &entry = queue[(head + i) % MAX_PACKETS];
		if (!entry.has_time) {
			continue;
		}
		if (!found_time || entry.start < first) {
			first = entry.start;
		}
		if (!found_time || entry.end > last) {
			last = entry.end;
		}
		found_time = true;
	}
	return found_time && last - first >= p_seconds;
}

int FFmpegPacketReader::get_terminal_error() const {
	MutexLock lock(mutex);
	return terminal_error;
}

void FFmpegPacketReader::stop(bool p_interrupt_io) {
	stop_requested.set();
	if (p_interrupt_io) {
		io_stop_requested.set();
	}
#ifdef THREADS_ENABLED
	capacity_changed.notify_all();
#endif
	if (worker.is_started()) {
		worker.wait_to_finish();
	}
	MutexLock lock(mutex);
	for (int i = 0; i < count; ++i) {
		QueuedPacket &entry = queue[(head + i) % MAX_PACKETS];
		av_packet_free(&entry.packet);
		entry = QueuedPacket();
	}
	head = 0;
	count = 0;
	queued_bytes = 0;
	capacity_pressure = false;
	terminal_error = AVERROR_EXIT;
	format = nullptr;
	external_cancel = nullptr;
#ifdef FFMPEG_TESTS_ENABLED
	test_read = nullptr;
	test_userdata = nullptr;
#endif
}

void FFmpegPacketReader::reset() {
	stop();
	MutexLock lock(mutex);
	terminal_error = 0;
	selected_stream = -1;
	time_base = { 0, 1 };
	stop_requested.clear();
	io_stop_requested.clear();
}

#endif // MODULE_MEDIA_STREAM_ENABLED
