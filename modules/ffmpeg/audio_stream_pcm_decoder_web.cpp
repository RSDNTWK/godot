/**************************************************************************/
/*  audio_stream_pcm_decoder_web.cpp                                              */
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
#ifdef WEB_ENABLED
#include "ffmpeg_media_metadata.h"
#include "media_source_policy.h"
#include "web/pcm_bridge.h"

#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/audio_stream_wav.h"
#include "servers/audio/audio_server.h"

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
	const bool youtube = MediaSourcePolicy::youtube_url(p_source);
	capabilities["pcm_decode"] = !youtube;
	capabilities["precise_loop"] = !youtube;
	capabilities["seek_loop"] = false;
	capabilities["live_playback"] = false;
	capabilities["dvr"] = false;
	capabilities["requires_cors"] = MediaSourcePolicy::https_url(p_source);
	capabilities["native_destination_validation"] = false;
	capabilities["max_pcm_bytes"] = int64_t(128 * 1024 * 1024);
	capabilities["max_encoded_bytes"] = int64_t(64 * 1024 * 1024);
	capabilities["worker_backend"] = "dedicated_web_worker";
	capabilities["requires_finite_content_length"] = MediaSourcePolicy::https_url(p_source);
	capabilities["max_concurrent_requests"] = 4;
	capabilities["max_worker_heap_bytes"] = int64_t(512 * 1024 * 1024);
	capabilities["redirects"] = false;
	return capabilities;
}

Error AudioStreamPCMDecoder::decode(const String &p_source, int p_mix_rate, int64_t p_max_pcm_bytes, int p_timeout_msec, const Dictionary &p_http_headers, bool p_allow_http) {
	ERR_FAIL_COND_V(!Thread::is_main_thread(), ERR_INVALID_PARAMETER);
	if (busy) {
		return ERR_BUSY;
	}
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree) {
		return ERR_UNCONFIGURED;
	}
	const int rate = p_mix_rate == 0 && AudioServer::get_singleton() ? int(AudioServer::get_singleton()->get_mix_rate()) : p_mix_rate;
	if (p_source.is_empty() || rate < 8000 || rate > 192000 || p_max_pcm_bytes < 4 || p_max_pcm_bytes > 128 * 1024 * 1024 || p_timeout_msec < 1 || p_timeout_msec > 60000) {
		return ERR_INVALID_PARAMETER;
	}
	const bool remote = p_source.contains("://") && !p_source.begins_with("res://") && !p_source.begins_with("user://");
	if (remote && !MediaSourcePolicy::https_url(p_source)) {
		return ERR_INVALID_PARAMETER;
	}
	if (MediaSourcePolicy::youtube_url(p_source)) {
		return ERR_UNAVAILABLE;
	}
	Dictionary approved;
	for (const Variant &name : p_http_headers.keys()) {
		const String key = String(name).to_lower();
		const String value = p_http_headers[name];
		if ((key != "accept" && key != "accept-language") || value.contains("\n") || value.contains("\r") || value.length() > 4096) {
			return ERR_UNAUTHORIZED;
		}
		approved[key] = value;
	}
	web_input.unref();
	if (!remote) {
		web_input = FileAccess::open(p_source, FileAccess::READ);
		if (web_input.is_null()) {
			return ERR_FILE_CANT_OPEN;
		}
		if (!web_input->get_length() || web_input->get_length() > 64 * 1024 * 1024) {
			web_input.unref();
			return ERR_OUT_OF_MEMORY;
		}
	}
	const CharString url = (remote ? p_source : String()).utf8();
	const CharString json = JSON::stringify(approved).utf8();
	web_request = godot_web_pcm_start(url.get_data(), rate, int(p_max_pcm_bytes - p_max_pcm_bytes % 4), p_timeout_msec, json.get_data());
	if (!web_request) {
		web_input.unref();
		return ERR_CANT_CREATE;
	}
	source = p_source;
	mix_rate = rate;
	max_pcm_bytes = p_max_pcm_bytes - p_max_pcm_bytes % 4;
	deadline = OS::get_singleton()->get_ticks_usec() + uint64_t(p_timeout_msec) * 1000;
	request_id++;
	busy = true;
	user_cancelled = false;
	web_copied = 0;
	web_pcm.clear();
	result.clear();
	result_error = OK;
	tree_id = tree->get_instance_id();
	Error connected = tree->connect(SNAME("process_frame"), callable_mp(this, &AudioStreamPCMDecoder::_collect).bind(Ref<AudioStreamPCMDecoder>(this)));
	if (connected != OK) {
		godot_web_pcm_free(web_request);
		web_request = 0;
		busy = false;
		web_input.unref();
		return connected;
	}
	if (tree->get_root()) {
		tree->get_root()->connect(SNAME("tree_exiting"), callable_mp(this, &AudioStreamPCMDecoder::cancel), CONNECT_ONE_SHOT);
	}
	return OK;
}

void AudioStreamPCMDecoder::_collect(const Ref<AudioStreamPCMDecoder> &p_keepalive) {
	if (!busy) {
		return;
	}
	char json[8192] = {};
	int state = web_request ? godot_web_pcm_status(web_request, json, sizeof(json)) : -2;
	const bool timeout = OS::get_singleton()->get_ticks_usec() >= deadline;
	Dictionary status;
	if (state != -2) {
		Variant parsed = JSON::parse_string(String::utf8(json));
		if (parsed.get_type() == Variant::DICTIONARY) {
			status = parsed;
		} else {
			state = -2;
		}
	}
	if (!user_cancelled && !timeout && state == 0) {
		if (web_input.is_valid() && bool(status.get("input_ready", false))) {
			Vector<uint8_t> chunk;
			if (chunk.resize(MIN(uint64_t(262144), web_input->get_length() - web_input->get_position())) != OK) {
				result_error = ERR_OUT_OF_MEMORY;
				status["message"] = "Could not allocate the encoded input chunk.";
			} else {
				const int read = web_input->get_buffer(chunk.ptrw(), chunk.size());
				if (read != chunk.size()) {
					result_error = ERR_FILE_CANT_READ;
				} else if (!godot_web_pcm_input(web_request, chunk.ptr(), read, web_input->get_position() == web_input->get_length())) {
					// A worker or transfer can fail after the initial status poll.
					// Preserve that asynchronous error instead of reporting a file read failure.
					const int submitted_state = godot_web_pcm_status(web_request, json, sizeof(json));
					Variant parsed = JSON::parse_string(String::utf8(json));
					if (submitted_state == -1 && parsed.get_type() == Variant::DICTIONARY) {
						status = parsed;
						result_error = Error(int(status.get("error", int(FAILED))));
						if (result_error == OK) {
							result_error = FAILED;
						}
					} else {
						result_error = ERR_FILE_CANT_READ;
					}
				} else {
					if (web_input->get_position() == web_input->get_length()) {
						web_input.unref();
					}
					return;
				}
			}
		} else {
			return;
		}
	} else if (!user_cancelled && !timeout && state == 1) {
		const int bytes = status.get("bytes", 0);
		if (bytes <= 0 || bytes > max_pcm_bytes || bytes % 4 || int(status.get("rate", 0)) != mix_rate || int(status.get("frames", 0)) != bytes / 4) {
			result_error = ERR_INVALID_DATA;
		} else {
			const int count = MIN(262144, bytes - web_copied);
			if (web_pcm.resize(web_copied + count) != OK) {
				result_error = ERR_OUT_OF_MEMORY;
			} else if (godot_web_pcm_read(web_request, web_pcm.ptrw() + web_copied, web_copied, count) != count) {
				result_error = ERR_INVALID_DATA;
			} else {
				web_copied += count;
				if (web_copied < bytes) {
					return;
				}
				Ref<AudioStreamWAV> wav;
				wav.instantiate();
				wav->set_format(AudioStreamWAV::FORMAT_16_BITS);
				wav->set_stereo(true);
				wav->set_mix_rate(mix_rate);
				wav->set_data(web_pcm);
				result["stream"] = wav;
				result["mix_rate"] = mix_rate;
				result["frame_count"] = bytes / 4;
				result["pcm_bytes"] = bytes;
				result["duration"] = double(bytes / 4) / mix_rate;
				result["metadata"] = FFmpegMediaMetadata::source(source);
				Dictionary metadata = result["metadata"];
				metadata["duration"] = result["duration"];
				metadata["opened"] = true;
				if (!String(status.get("title", "")).is_empty()) {
					metadata["title"] = FFmpegMediaMetadata::title(status["title"]);
					metadata["title_source"] = "container";
				}
			}
		}
	} else {
		result_error = timeout ? ERR_TIMEOUT : state == -1 ? Error(int(status.get("error", int(ERR_INVALID_DATA))))
														   : ERR_SKIP;
	}
	if (result_error != OK) {
		result.clear();
		result["message"] = timeout ? "PCM preparation timed out." : String(status.get("message", "Web PCM preparation failed."));
	}
	result["request_id"] = request_id;
	result["decoder_instance_id"] = int64_t(uint64_t(get_instance_id()));
	result["source"] = source;
	result["worker_thread"] = true;
	result["worker_backend"] = "dedicated_web_worker";
	result["capabilities"] = get_capabilities(source);
	if (web_request) {
		godot_web_pcm_free(web_request);
	}
	web_request = 0;
	web_input.unref();
	web_pcm.clear();
	busy = false;
	SceneTree *tree = Object::cast_to<SceneTree>(ObjectDB::get_instance(tree_id));
	if (tree) {
		tree->disconnect(SNAME("process_frame"), callable_mp(this, &AudioStreamPCMDecoder::_collect).bind(p_keepalive));
		if (tree->get_root() && tree->get_root()->is_connected(SNAME("tree_exiting"), callable_mp(this, &AudioStreamPCMDecoder::cancel))) {
			tree->get_root()->disconnect(SNAME("tree_exiting"), callable_mp(this, &AudioStreamPCMDecoder::cancel));
		}
	}
	const bool notify = !user_cancelled;
	const Error completed_error = result_error;
	const Dictionary completed_result = result;
	result = Dictionary();
	if (notify) {
		emit_signal(SNAME("completed"), int(completed_error), completed_result);
	}
}

void AudioStreamPCMDecoder::cancel() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	user_cancelled = true;
	if (web_request) {
		godot_web_pcm_free(web_request);
	}
	web_request = 0;
}
AudioStreamPCMDecoder::~AudioStreamPCMDecoder() {
	if (web_request) {
		godot_web_pcm_free(web_request);
	}
}
#endif
