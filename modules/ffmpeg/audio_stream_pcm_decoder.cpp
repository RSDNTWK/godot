/**************************************************************************/
/*  audio_stream_pcm_decoder.cpp                                              */
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

#include "audio_stream_pcm_decoder.h"

#if defined(MODULE_MEDIA_STREAM_ENABLED) && !defined(WEB_ENABLED)
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "ffmpeg_media_metadata.h"
#include "modules/media_stream/media_http.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/audio/audio_server.h"
#ifdef MODULE_YOUTUBE_ENABLED
#include "modules/youtube/youtube_extractor.h"
#include "modules/youtube/youtube_parser.h"
#endif

void AudioStreamPCMDecoder::_bind_methods() {
	ClassDB::bind_method(D_METHOD("decode", "source", "mix_rate", "max_pcm_bytes", "timeout_msec", "http_headers", "allow_http"), &AudioStreamPCMDecoder::decode, DEFVAL(0), DEFVAL(128 * 1024 * 1024), DEFVAL(60000), DEFVAL(Dictionary()), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("cancel"), &AudioStreamPCMDecoder::cancel);
	ClassDB::bind_method(D_METHOD("is_decoding"), &AudioStreamPCMDecoder::is_decoding);
	ClassDB::bind_method(D_METHOD("get_request_id"), &AudioStreamPCMDecoder::get_request_id);
	ClassDB::bind_static_method("AudioStreamPCMDecoder", D_METHOD("get_capabilities", "source"), &AudioStreamPCMDecoder::get_capabilities, DEFVAL(String()));
	ADD_SIGNAL(MethodInfo("completed", PropertyInfo(Variant::INT, "error"), PropertyInfo(Variant::DICTIONARY, "result")));
}

Dictionary AudioStreamPCMDecoder::get_capabilities(const String &p_source) {
	Dictionary capabilities;
	capabilities["pcm_decode"] = true;
	capabilities["precise_loop"] = true;
	capabilities["seek_loop"] = false;
	capabilities["live_playback"] = false;
	capabilities["dvr"] = false;
	capabilities["native_destination_validation"] = true;
	capabilities["max_pcm_bytes"] = int64_t(128 * 1024 * 1024);
	return capabilities;
}

Error AudioStreamPCMDecoder::decode(const String &p_source, int p_mix_rate, int64_t p_max_pcm_bytes, int p_timeout_msec, const Dictionary &p_http_headers, bool p_allow_http) {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), ERR_INVALID_PARAMETER);
	if (busy) {
		return ERR_BUSY;
	}
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !WorkerThreadPool::get_singleton()) {
		return ERR_UNCONFIGURED;
	}
	const int rate = p_mix_rate == 0 && AudioServer::get_singleton() ? int(AudioServer::get_singleton()->get_mix_rate()) : p_mix_rate;
	if (p_source.is_empty() || rate < 8000 || rate > 192000 || p_max_pcm_bytes < 4 || p_max_pcm_bytes > 128 * 1024 * 1024 || p_timeout_msec < 1 || p_timeout_msec > 60000) {
		return ERR_INVALID_PARAMETER;
	}
	if (p_source.contains("://") && !p_source.begins_with("res://") && !p_source.begins_with("user://") && !MediaHTTP::valid_url(p_source, p_allow_http)) {
		return ERR_INVALID_PARAMETER;
	}
	source = p_source;
	allow_http = p_allow_http;
	headers = p_http_headers.duplicate(true);
	mix_rate = rate;
	max_pcm_bytes = p_max_pcm_bytes - p_max_pcm_bytes % 4;
	deadline = OS::get_singleton()->get_ticks_usec() + uint64_t(p_timeout_msec) * 1000;
	request_id++;
	user_cancelled = false;
	interrupted.clear();
	result = Dictionary();
	result_error = OK;
	busy = true;
	tree_id = tree->get_instance_id();
	// The tree keeps the request alive until collection, even if its owner drops
	// its Ref during a track change. Binding only here avoids an owned self-cycle.
	Error error = tree->connect(SNAME("process_frame"), callable_mp(this, &AudioStreamPCMDecoder::_collect).bind(Ref<AudioStreamPCMDecoder>(this)));
	if (error != OK) {
		busy = false;
		return error;
	}
	if (tree->get_root()) {
		tree->get_root()->connect(SNAME("tree_exiting"), callable_mp(this, &AudioStreamPCMDecoder::cancel), CONNECT_ONE_SHOT);
	}
	// The pool also owns a Ref, so scene-tree teardown never frees an active
	// worker or waits for it while an AudioServer/scene-tree lock may be held.
	task_id = WorkerThreadPool::get_singleton()->add_task(callable_mp(this, &AudioStreamPCMDecoder::_run_task).bind(Ref<AudioStreamPCMDecoder>(this)), false, "Prepare bounded playlist PCM");
	if (task_id == WorkerThreadPool::INVALID_TASK_ID) {
		tree->disconnect(SNAME("process_frame"), callable_mp(this, &AudioStreamPCMDecoder::_collect).bind(Ref<AudioStreamPCMDecoder>(this)));
		busy = false;
		return ERR_CANT_CREATE;
	}
	return OK;
}

void AudioStreamPCMDecoder::_run_task(const Ref<AudioStreamPCMDecoder> &p_keepalive) {
	DEV_ASSERT(p_keepalive.ptr() == this);
	_worker(this);
}

void AudioStreamPCMDecoder::_worker(void *p_self) {
	AudioStreamPCMDecoder *self = static_cast<AudioStreamPCMDecoder *>(p_self);
	String resolved = self->source;
	Dictionary transport_headers = self->headers;
	Dictionary resolved_metadata;
#ifdef MODULE_YOUTUBE_ENABLED
	if (!YouTubeParser::video_id(self->source).is_empty()) {
		YouTubeExtractor extractor(self->interrupted, self->deadline);
		self->result_error = extractor.resolve(self->source, true, resolved_metadata);
		if (self->result_error != OK) {
			self->result = resolved_metadata;
		} else if (bool(resolved_metadata.get("is_live", false))) {
			self->result_error = ERR_UNAVAILABLE;
			self->result["message"] = "PCM preparation requires finite audio; use streaming/DVR for live inputs.";
		} else {
			resolved = resolved_metadata.get("url", String());
			transport_headers = resolved_metadata.get("http_headers", Dictionary());
		}
	}
#endif
	if (self->result_error == OK) {
		self->result_error = ffmpeg_decode_audio_pcm(resolved, transport_headers, self->interrupted, self->mix_rate, self->max_pcm_bytes, self->deadline, self->result, self->allow_http);
		if (self->result_error == OK && !resolved_metadata.is_empty()) {
			Dictionary metadata = self->result.get("metadata", Dictionary());
			metadata["title"] = FFmpegMediaMetadata::title(resolved_metadata.get("title", String()));
			metadata["title_source"] = "youtube";
			metadata["source_type"] = "youtube";
			metadata["source"] = self->source;
			self->result["metadata"] = metadata;
		}
	}
	if (OS::get_singleton()->get_ticks_usec() >= self->deadline) {
		self->result.clear();
		self->result["message"] = "PCM preparation timed out.";
		self->result["request_id"] = self->request_id;
		self->result_error = ERR_TIMEOUT;
	}
	self->result["source"] = self->source;
	self->result["request_id"] = self->request_id;
	self->result["decoder_instance_id"] = int64_t(uint64_t(self->get_instance_id()));
	self->result["worker_thread"] = !Thread::is_main_thread();
}

void AudioStreamPCMDecoder::_collect(const Ref<AudioStreamPCMDecoder> &p_keepalive) {
	if (!busy) {
		return;
	}
	if (OS::get_singleton()->get_ticks_usec() >= deadline) {
		interrupted.set();
	}
	if (!WorkerThreadPool::get_singleton()->is_task_completed(task_id)) {
		return;
	}
	// A completed task only: this never waits for active decoding on the frame.
	WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
	task_id = WorkerThreadPool::INVALID_TASK_ID;
	SceneTree *tree = Object::cast_to<SceneTree>(ObjectDB::get_instance(tree_id));
	if (tree) {
		tree->disconnect(SNAME("process_frame"), callable_mp(this, &AudioStreamPCMDecoder::_collect).bind(p_keepalive));
		if (tree->get_root() && tree->get_root()->is_connected(SNAME("tree_exiting"), callable_mp(this, &AudioStreamPCMDecoder::cancel))) {
			tree->get_root()->disconnect(SNAME("tree_exiting"), callable_mp(this, &AudioStreamPCMDecoder::cancel));
		}
	}
	busy = false;
	const bool notify = !user_cancelled;
	const Error completed_error = result_error;
	const Dictionary completed_result = result;
	result = Dictionary();
	if (notify) {
		emit_signal(SNAME("completed"), completed_error, completed_result);
	}
}

void AudioStreamPCMDecoder::cancel() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	user_cancelled = true;
	interrupted.set();
}

AudioStreamPCMDecoder::~AudioStreamPCMDecoder() {
	interrupted.set();
	// Active tasks retain the decoder themselves. There is no blocking join in
	// cancellation, owner release, or scene-tree teardown.
}
#endif
