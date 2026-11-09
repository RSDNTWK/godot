/**************************************************************************/
/*  ffmpeg_packet_reader.h                                              */
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

#pragma once

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_MEDIA_STREAM_ENABLED

#include "core/error/error_list.h"
#include "core/os/condition_variable.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"

extern "C" {
#include <libavformat/avformat.h>
}

// One producer owns av_read_frame while the decoder consumes its selected stream.
// The format and its custom IO remain caller-owned: stop() MUST precede closing,
// seeking, or otherwise accessing that format on another thread. Decoder/clock
// state is never accessed by the producer.
class FFmpegPacketReader {
	static constexpr int MAX_PACKETS = 1024;
	static constexpr int64_t MAX_BYTES = 8 * 1024 * 1024;

	struct QueuedPacket {
		AVPacket *packet = nullptr;
		int64_t bytes = 0;
		double start = 0.0;
		double end = 0.0;
		bool has_time = false;
	};

	Thread worker;
	SafeFlag stop_requested;
	SafeFlag io_stop_requested;
	const SafeFlag *external_cancel = nullptr;
	AVFormatContext *format = nullptr;
	int selected_stream = -1;
	AVRational time_base = { 0, 1 };
	mutable BinaryMutex mutex;
#ifdef THREADS_ENABLED
	THREADING_NAMESPACE::condition_variable capacity_changed;
#endif
	QueuedPacket queue[MAX_PACKETS];
	int head = 0;
	int count = 0;
	int64_t queued_bytes = 0;
	bool capacity_pressure = false;
	int terminal_error = 0;

	static void _run(void *p_self);
	void _produce();
	bool _interrupted() const;
	Error _start_worker();
	void _set_terminal(int p_error);
	static int64_t _packet_bytes(const AVPacket *p_packet);

	// Keep the class layout identical in module and test_main translation units.
	// The callback path itself is compiled only in FFmpeg test builds.
	using ReadCallback = int (*)(void *, AVPacket *, const SafeFlag *, const SafeFlag *);
	ReadCallback test_read = nullptr;
	void *test_userdata = nullptr;

public:
	// The flag address is stable even before the HTTP input opens. reset() only
	// clears it AFTER joining the old producer; pass it to every child transport.
	void reset();
	const SafeFlag *get_stop_flag() const { return &io_stop_requested; }
	Error start(AVFormatContext *p_format, int p_selected_stream, const SafeFlag *p_external_cancel);
	bool is_started() const { return worker.is_started(); }
	// Nonblocking: packet, EAGAIN while empty/running, then EOF/error after drain.
	int read(AVPacket *r_packet);
	bool has_prefill(double p_seconds = 6.0) const;
	int get_terminal_error() const;
	// A finite seek can finish the current bounded read instead of aborting a
	// partially parsed container packet. Destruction/cancellation still abort IO.
	void stop(bool p_interrupt_io = true);

	using TestReadCallback = ReadCallback;
	// Synthetic inputs exercise the identical producer, bounds, and queue path.
	// Its definition is linked only when the FFmpeg module is built with tests.
	Error start_for_test(TestReadCallback p_read, void *p_userdata, int p_selected_stream, AVRational p_time_base, const SafeFlag *p_external_cancel = nullptr);

	~FFmpegPacketReader() { stop(); }
};

#endif // MODULE_MEDIA_STREAM_ENABLED
