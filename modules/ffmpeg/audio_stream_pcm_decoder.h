/**************************************************************************/
/*  audio_stream_pcm_decoder.h                                              */
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

#if defined(MODULE_MEDIA_STREAM_ENABLED) || defined(WEB_ENABLED)
#include "core/io/file_access.h"
#include "core/object/ref_counted.h"
#include "core/object/worker_thread_pool.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"

// Opt-in finite-audio preparation. Realtime AudioStreamMedia playback is separate.
class AudioStreamPCMDecoder : public RefCounted {
	GDCLASS(AudioStreamPCMDecoder, RefCounted);

	WorkerThreadPool::TaskID task_id = WorkerThreadPool::INVALID_TASK_ID;
	ObjectID tree_id;
	SafeFlag interrupted;
	bool busy = false;
	bool user_cancelled = false;
	int64_t request_id = 0;
	String source;
	Dictionary headers;
	int mix_rate = 48000;
	int64_t max_pcm_bytes = 128 * 1024 * 1024;
	uint64_t deadline = 0;
	Error result_error = OK;
	Dictionary result;
#ifdef WEB_ENABLED
	int web_request = 0;
	Ref<FileAccess> web_input;
	Vector<uint8_t> web_pcm;
	int web_copied = 0;
#endif
	static void _worker(void *p_self);
	void _run_task(const Ref<AudioStreamPCMDecoder> &p_keepalive);
	void _collect(const Ref<AudioStreamPCMDecoder> &p_keepalive);

protected:
	static void _bind_methods();

public:
	Error decode(const String &p_source, int p_mix_rate = 0, int64_t p_max_pcm_bytes = 128 * 1024 * 1024, int p_timeout_msec = 60000, const Dictionary &p_http_headers = Dictionary());
	void cancel();
	bool is_decoding() const { return busy; }
	int64_t get_request_id() const { return request_id; }
	static Dictionary get_capabilities(const String &p_source = String());
	~AudioStreamPCMDecoder();
};

// Synchronous internal decoder: caller must run this on a worker. It returns
// only actual decoded samples and never publishes a truncated or padded WAV.
Error ffmpeg_decode_audio_pcm(const String &p_source, const Dictionary &p_headers, const SafeFlag &p_cancel, int p_mix_rate, int64_t p_max_pcm_bytes, uint64_t p_deadline, Dictionary &r_result);
#endif
