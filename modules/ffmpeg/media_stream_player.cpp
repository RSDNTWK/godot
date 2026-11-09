/**************************************************************************/
/*  media_stream_player.cpp                                              */
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

#include "media_stream_player.h"
#if defined(WEB_ENABLED) || defined(MODULE_MEDIA_STREAM_ENABLED)
#include "ffmpeg_media_metadata.h"
#include "media_source_policy.h"
#include "video_stream_media.h"

#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/main/viewport.h"
#include "scene/resources/material.h"
#include "scene/resources/shader.h"
#include "servers/rendering/rendering_server.h"
namespace {
struct MediaPortalViewport {
	int users = 0;
	bool was_transparent = false;
};
HashMap<ObjectID, MediaPortalViewport> media_portal_viewports;
} //namespace
#ifdef WEB_ENABLED
#include "web/player_bridge.h"
#endif

void MediaStreamPlayer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "source", "options"), &MediaStreamPlayer::open, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("stop"), &MediaStreamPlayer::stop);
	ClassDB::bind_method(D_METHOD("play"), &MediaStreamPlayer::play);
	ClassDB::bind_method(D_METHOD("set_paused", "paused"), &MediaStreamPlayer::set_paused);
	ClassDB::bind_method(D_METHOD("is_paused"), &MediaStreamPlayer::is_paused);
	ClassDB::bind_method(D_METHOD("seek", "seconds"), &MediaStreamPlayer::seek);
	ClassDB::bind_method(D_METHOD("set_loop_range", "begin", "end"), &MediaStreamPlayer::set_loop_range);
	ClassDB::bind_method(D_METHOD("clear_loop"), &MediaStreamPlayer::clear_loop);
	ClassDB::bind_method(D_METHOD("get_snapshot"), &MediaStreamPlayer::get_snapshot);
	ClassDB::bind_method(D_METHOD("is_preparing"), &MediaStreamPlayer::is_preparing);
	ClassDB::bind_method(D_METHOD("get_preparation_state"), &MediaStreamPlayer::get_preparation_state);
	ClassDB::bind_method(D_METHOD("get_capabilities"), &MediaStreamPlayer::get_capabilities);
	ClassDB::bind_method(D_METHOD("get_seekable_ranges"), &MediaStreamPlayer::get_seekable_ranges);
	ClassDB::bind_method(D_METHOD("get_generation"), &MediaStreamPlayer::get_generation);
	ClassDB::bind_method(D_METHOD("set_volume_linear", "volume"), &MediaStreamPlayer::set_volume_linear);
	ClassDB::bind_method(D_METHOD("get_volume_linear"), &MediaStreamPlayer::get_volume_linear);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "volume_linear", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_volume_linear", "get_volume_linear");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "paused"), "set_paused", "is_paused");
	const char *signals[] = { "playback_event", "media_ready", "buffering_changed", "playback_error", "seek_completed", "metadata_changed", "capabilities_changed", "seekable_ranges_changed", "loop_invalidated", "loop_changed", "finished", "preparation_failed" };
	for (const char *name : signals) {
		ADD_SIGNAL(MethodInfo(name, PropertyInfo(Variant::DICTIONARY, "event")));
	}
}

void MediaStreamPlayer::_event(const StringName &p_kind, Dictionary p_payload) {
	p_payload = p_payload.duplicate(true);
	p_payload["type"] = p_kind;
	p_payload["playback_instance_id"] = int64_t(uint64_t(get_instance_id()));
	p_payload["generation"] = generation;
	p_payload["source"] = source;
	const int64_t current = generation;
	emit_signal(p_kind, p_payload);
	if (current == generation) {
		emit_signal(SNAME("playback_event"), p_payload);
	}
}

void MediaStreamPlayer::_capabilities(const Dictionary &p_capabilities) {
	if (capabilities == p_capabilities) {
		return;
	}
	capabilities = p_capabilities.duplicate(true);
	const int64_t current = generation;
	if (loop_active && !bool(capabilities.get("precise_loop", false)) && !bool(capabilities.get("seek_loop", false))) {
		_invalidate_loop("loop_capability_withdrawn");
	}
	if (current != generation) {
		return;
	}
	Dictionary event;
	event["capabilities"] = capabilities.duplicate(true);
	_event(SNAME("capabilities_changed"), event);
}

bool MediaStreamPlayer::_contains(double p_begin, double p_end) const {
	for (int i = 0; i < ranges.size(); i++) {
		Dictionary range = ranges[i];
		if (p_begin >= double(range.get("start", 0.0)) && p_end <= double(range.get("end", 0.0))) {
			return true;
		}
	}
	return false;
}

void MediaStreamPlayer::_invalidate_loop(const String &p_reason) {
	if (!loop_active) {
		return;
	}
	loop_active = false;
	if (pcm.is_valid()) {
		AudioServer::get_singleton()->lock();
		pcm->set_loop_mode(AudioStreamWAV::LOOP_DISABLED);
		AudioServer::get_singleton()->unlock();
	}
	Dictionary event;
	event["reason"] = p_reason;
	event["begin"] = loop_begin;
	event["end"] = loop_end;
	event["active"] = false;
	_event(SNAME("loop_invalidated"), event);
}

void MediaStreamPlayer::_ranges(const Array &p_ranges) {
	if (ranges == p_ranges) {
		return;
	}
	ranges = p_ranges.duplicate(true);
	const int64_t current = generation;
	if (loop_active && !_contains(loop_begin, loop_end)) {
		_invalidate_loop("requested_range_no_longer_seekable");
	}
	if (current != generation) {
		return;
	}
	Dictionary event;
	event["ranges"] = ranges.duplicate(true);
	_event(SNAME("seekable_ranges_changed"), event);
}

void MediaStreamPlayer::_collect_retired_preparations() {
	for (int i = retiring_preparations.size() - 1; i >= 0; i--) {
		if (!retiring_preparations[i].decoder->is_decoding()) {
			retiring_preparations.remove_at(i);
		}
	}
}

void MediaStreamPlayer::_reset() {
	_collect_retired_preparations();
#ifdef WEB_ENABLED
	if (web_handle) {
		godot_web_media_close(web_handle);
	}
#endif
	web_handle = 0;
	web_source_size = Size2();
	_portal(false);
	if (decoder.is_valid()) {
		decoder->cancel();
		if (decoder->is_decoding()) {
			RetiringPreparation retired;
			retired.decoder = decoder;
			retired.generation = decoder_generation;
			retired.source = source;
			retiring_preparations.push_back(retired);
		}
		decoder.unref();
	}
	decoder_generation = 0;
	_native_unbind();
	audio->stop();
	audio->set_stream(Ref<AudioStream>());
	video->stop();
	video->set_stream(Ref<VideoStream>());
	pcm.unref();
	web_texture.unref();
	web_pixels.clear();
	opened = ready_state = buffering = play_requested = seek_pending = loop_active = false;
	position = duration = 0;
	loop_begin = loop_end = 0;
	metadata.clear();
	capabilities.clear();
	ranges.clear();
	queue_redraw();
}

Error MediaStreamPlayer::open(const String &p_source, const Dictionary &p_options) {
	ERR_FAIL_COND_V(!is_inside_tree(), ERR_UNCONFIGURED);
	if (p_source.is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	const bool remote = p_source.contains("://") && !p_source.begins_with("res://") && !p_source.begins_with("user://");
#ifdef WEB_ENABLED
	if (remote && !MediaSourcePolicy::browser_url(p_source)) {
#else
	if (remote && !MediaSourcePolicy::https_url(p_source)) {
#endif
		return ERR_INVALID_PARAMETER;
	}
#ifdef WEB_ENABLED
	// HTML media/iframes do not accept arbitrary request headers.
	if (!bool(p_options.get("prepare_pcm", false)) && !Dictionary(p_options.get("http_headers", Dictionary())).is_empty()) {
		return ERR_UNAVAILABLE;
	}
#endif
	if (bool(p_options.get("prepare_pcm", false)) && bool(p_options.get("video", false))) {
		return ERR_INVALID_PARAMETER;
	}
	// A canceled request can still own a worker/output until its frame collector
	// retires it. Reject admission without replacing the accepted generation.
	if (bool(p_options.get("prepare_pcm", false)) && is_preparing()) {
		return ERR_BUSY;
	}
	++generation;
	_reset();
	source = p_source;
	options = p_options.duplicate(true);
	opened = true;
	play_requested = options.get("autoplay", true);
	paused = !play_requested;
	metadata = FFmpegMediaMetadata::source(source);
	buffering = true;
	set_process_internal(true);
	if (bool(options.get("prepare_pcm", false))) {
		decoder.instantiate();
		decoder_generation = generation;
		decoder->connect(SNAME("completed"), callable_mp(this, &MediaStreamPlayer::_pcm_completed).bind(generation));
		const Error err = decoder->decode(source, options.get("mix_rate", 0), options.get("max_pcm_bytes", 128 * 1024 * 1024), options.get("timeout_msec", 60000), options.get("http_headers", Dictionary()));
		if (err == OK) {
			buffering = true;
			return OK;
		}
		decoder.unref();
		decoder_generation = 0;
		if (!bool(options.get("allow_browser_fallback", true))) {
			opened = false;
			return err;
		}
#ifndef WEB_ENABLED
		opened = false;
		return err;
#endif
	}
	return _open_backend();
}

void MediaStreamPlayer::_pcm_completed(int p_error, const Dictionary &p_result, int64_t p_generation) {
	if (p_generation != generation || !opened) {
		return;
	}
	decoder.unref();
	decoder_generation = 0;
	if (p_error != OK) {
#ifdef WEB_ENABLED
		if (bool(options.get("allow_browser_fallback", true)) && MediaSourcePolicy::browser_url(source)) {
			if (!Dictionary(options.get("http_headers", Dictionary())).is_empty()) {
				_native_error(ERR_UNAVAILABLE, "Browser playback cannot forward the requested custom headers.", p_generation);
				return;
			}
			Dictionary event;
			event["error"] = p_error;
			event["message"] = p_result.get("message", "PCM preparation unavailable; trying browser playback.");
			_event(SNAME("preparation_failed"), event);
			if (p_generation == generation) {
				const Error error = _open_backend();
				if (error != OK) {
					_native_error(error, "Browser playback fallback could not open.", p_generation);
				}
			}
			return;
		}
#endif
		_native_error(p_error, p_result.get("message", "PCM preparation failed."), p_generation);
		return;
	}
	pcm = p_result.get("stream", Variant());
	if (pcm.is_null()) {
		_native_error(ERR_INVALID_DATA, "PCM preparation returned no WAV.", p_generation);
		return;
	}
	duration = pcm->get_length();
	metadata = p_result.get("metadata", Dictionary());
	// Sample backends snapshot loop settings at registration. Stream playback
	// reads the WAV's exclusive sample bounds, including later loop changes.
	audio->set_playback_type(AudioServer::PLAYBACK_TYPE_STREAM);
	audio->set_stream(pcm);
	audio->play();
	_apply_pause();
	Dictionary caps;
	caps["precise_loop"] = true;
	caps["seek_loop"] = true;
	caps["seek"] = true;
	caps["live_playback"] = false;
	caps["dvr"] = false;
	caps["pcm_decode"] = true;
	caps["backend"] = "pcm";
	caps["volume"] = true;
	caps["godot_audio_bus"] = true;
	_capabilities(caps);
	if (p_generation != generation) {
		return;
	}
	Array seekable;
	Dictionary range;
	range["start"] = 0.0;
	range["end"] = duration;
	seekable.push_back(range);
	_ranges(seekable);
	if (p_generation != generation) {
		return;
	}
	_native_metadata(metadata, p_generation);
	if (p_generation != generation) {
		return;
	}
	_native_ready(p_generation);
}

Error MediaStreamPlayer::_open_backend() {
#ifdef WEB_ENABLED
	if (!Dictionary(options.get("http_headers", Dictionary())).is_empty()) {
		opened = buffering = false;
		return ERR_UNAVAILABLE;
	}
	if (!MediaSourcePolicy::browser_url(source)) {
		opened = buffering = false;
		return ERR_UNAVAILABLE;
	}
	Dictionary browser_options = options.duplicate(true);
	browser_options["volume"] = volume;
	browser_options["autoplay"] = play_requested && !paused && can_process();
	browser_options["canvas_background"] = RenderingServer::get_singleton()->get_default_clear_color().to_html(false);
	const CharString url = source.utf8(), json = JSON::stringify(browser_options).utf8();
	web_handle = godot_web_media_open(url.get_data(), json.get_data());
	if (!web_handle) {
		opened = buffering = false;
		return ERR_CANT_CREATE;
	}
	buffering = true;
#else
	if (bool(options.get("video", false))) {
		Ref<VideoStreamMedia> stream;
		stream.instantiate();
		stream->set_file(source);
		stream->set_http_headers(options.get("http_headers", Dictionary()));
		stream->set_live_dvr_enabled(options.get("dvr", true));
		video->set_stream(stream);
		if (video->get_stream_playback().is_null()) {
			opened = buffering = false;
			return ERR_CANT_OPEN;
		}
		video->play();
		_apply_pause();
	} else {
		Ref<AudioStreamMedia> stream;
		stream.instantiate();
		stream->set_file(source);
		stream->set_http_headers(options.get("http_headers", Dictionary()));
		stream->set_live_dvr_enabled(options.get("dvr", true));
		audio->set_stream(stream);
		audio->play();
		_apply_pause();
		if (!audio->has_stream_playback()) {
			opened = buffering = false;
			return ERR_CANT_OPEN;
		}
	}
	_native_bind();
#endif
	return OK;
}

void MediaStreamPlayer::_native_bind() {
	Ref<RefCounted> playback;
	if (video->get_stream().is_valid()) {
		playback = video->get_stream_playback();
	} else if (audio->has_stream_playback()) {
		playback = audio->get_stream_playback();
	}
	if (playback == bound_playback || playback.is_null()) {
		return;
	}
	_native_unbind();
	bound_playback = playback;
	if (playback->has_signal(SNAME("ready"))) {
		playback->connect(SNAME("ready"), callable_mp(this, &MediaStreamPlayer::_native_ready).bind(generation));
		playback->connect(SNAME("buffering_changed"), callable_mp(this, &MediaStreamPlayer::_native_buffering).bind(generation));
		playback->connect(SNAME("error"), callable_mp(this, &MediaStreamPlayer::_native_error).bind(generation));
		playback->connect(SNAME("seek_completed"), callable_mp(this, &MediaStreamPlayer::_native_seek).bind(generation));
		playback->connect(SNAME("metadata_changed"), callable_mp(this, &MediaStreamPlayer::_native_metadata).bind(generation));
	} else {
		_native_ready(generation);
	}
}

void MediaStreamPlayer::_native_unbind() {
	if (bound_playback.is_null()) {
		return;
	}
	const StringName names[] = { SNAME("ready"), SNAME("buffering_changed"), SNAME("error"), SNAME("seek_completed"), SNAME("metadata_changed") };
	// Disconnect our callbacks even when another caller retains the old playback.
	for (const StringName &name : names) {
		if (!bound_playback->has_signal(name)) {
			continue;
		}
		List<Object::Connection> signal_connections;
		bound_playback->get_signal_connection_list(name, &signal_connections);
		for (const Object::Connection &connection : signal_connections) {
			if (connection.callable.get_object_id() == get_instance_id()) {
				bound_playback->disconnect(name, connection.callable);
			}
		}
	}
	bound_playback.unref();
}

void MediaStreamPlayer::_native_ready(int64_t p_generation) {
	if (p_generation != generation || !opened) {
		return;
	}
	const bool first = !ready_state;
	ready_state = true;
	_native_buffering(false, p_generation);
	if (p_generation != generation) {
		return;
	}
#ifndef WEB_ENABLED
	if (first && pcm.is_null()) {
		_poll();
	}
	if (p_generation != generation) {
		return;
	}
	if (first && !bool(metadata.get("opened", false))) {
		Dictionary next_metadata = metadata.duplicate(true);
		next_metadata["opened"] = true;
		next_metadata["duration"] = duration;
		_native_metadata(next_metadata, p_generation);
		if (p_generation != generation) {
			return;
		}
	}
#endif
	if (first) {
		_event(SNAME("media_ready"), get_snapshot());
	}
}
void MediaStreamPlayer::_native_buffering(bool p_buffering, int64_t p_generation) {
	if (p_generation != generation || !opened || buffering == p_buffering) {
		return;
	}
	buffering = p_buffering;
	Dictionary event;
	event["buffering"] = buffering;
	_event(SNAME("buffering_changed"), event);
}
void MediaStreamPlayer::_native_error(int p_error, const String &p_message, int64_t p_generation) {
	if (p_generation != generation) {
		return;
	}
	buffering = false;
	seek_pending = false;
	Dictionary event;
	event["error"] = p_error;
	event["message"] = p_message;
	_event(SNAME("playback_error"), event);
}
void MediaStreamPlayer::_native_seek(double p_position, int64_t p_generation) {
	if (p_generation != generation || !opened || !seek_pending) {
		return;
	}
	seek_pending = false;
	position = p_position;
	Dictionary event;
	event["position"] = position;
	event["seek_request_id"] = seek_request_id;
	_event(SNAME("seek_completed"), event);
}
void MediaStreamPlayer::_native_metadata(const Dictionary &p_metadata, int64_t p_generation) {
	if (p_generation != generation || !opened) {
		return;
	}
	metadata = p_metadata.duplicate(true);
	metadata["source"] = source;
	if (metadata.has("title")) {
		metadata["title"] = FFmpegMediaMetadata::title(metadata["title"]);
	}
	// Backend IDs describe its own worker. Public identity belongs to this controller.
	metadata.erase("playback_instance_id");
	metadata.erase("generation");
	Dictionary event;
	event["metadata"] = metadata.duplicate(true);
	_event(SNAME("metadata_changed"), event);
}

void MediaStreamPlayer::play() {
	if (!opened) {
		return;
	}
	play_requested = true;
	set_paused(false);
#ifdef WEB_ENABLED
	if (web_handle) {
		return;
	}
#endif
	if (ready_state && video->get_stream().is_valid() && !video->is_playing()) {
		video->play();
		_apply_pause();
		_native_bind();
	} else if (ready_state && audio->get_stream().is_valid() && !audio->is_playing()) {
		audio->play();
		_apply_pause();
		_native_bind();
	}
}
void MediaStreamPlayer::set_paused(bool p_paused) {
	paused = p_paused;
	play_requested = !paused;
	_apply_pause();
}

void MediaStreamPlayer::_apply_pause() {
	const bool effective = paused || !can_process();
	audio->set_stream_paused(effective);
	video->set_paused(effective);
#ifdef WEB_ENABLED
	if (web_handle) {
		godot_web_media_command(web_handle, effective ? "pause" : "play", 0, 0);
	}
#endif
}
void MediaStreamPlayer::stop() {
	++generation;
	_reset();
	source = String();
	options.clear();
	set_process_internal(!retiring_preparations.is_empty());
}
Error MediaStreamPlayer::seek(double p_seconds) {
	if (!ready_state) {
		return ERR_UNCONFIGURED;
	}
	if (seek_pending) {
		return ERR_BUSY;
	}
	if (!Math::is_finite(p_seconds) || p_seconds < 0 || !_contains(p_seconds, p_seconds)) {
		return ERR_INVALID_PARAMETER;
	}
	if (!bool(capabilities.get("seek", false))) {
		return ERR_UNAVAILABLE;
	}
	seek_pending = true;
	++seek_request_id;
#ifdef WEB_ENABLED
	if (web_handle) {
		godot_web_media_command(web_handle, "seek", p_seconds, seek_request_id);
		return OK;
	}
#endif
	if (video->get_stream().is_valid()) {
		if (!video->is_playing()) {
			video->play();
			_apply_pause();
		}
		video->set_stream_position(p_seconds);
	} else {
		Ref<AudioStreamPlayback> playback = bound_playback;
		if (pcm.is_null() && audio->has_stream_playback() && playback.is_valid() && playback->is_playing()) {
			// Remote playback owns a worker and can publish a seek command in
			// place. AudioStreamPlayer::seek() recreates unpaused playback,
			// discarding the already resolved source and open demuxer.
			playback->seek(p_seconds);
		} else if (audio->is_playing()) {
			audio->seek(p_seconds);
		} else {
			audio->play(p_seconds);
			_apply_pause();
		}
	}
	_apply_pause();
	_native_bind();
	if (pcm.is_valid() || !bound_playback.is_valid() || !bound_playback->has_signal(SNAME("seek_completed"))) {
		_native_seek(p_seconds, generation);
	}
	return OK;
}
Error MediaStreamPlayer::set_loop_range(double p_begin, double p_end) {
	if (!ready_state) {
		return ERR_UNCONFIGURED;
	}
	if (!Math::is_finite(p_begin) || !Math::is_finite(p_end) || p_begin < 0 || p_end <= p_begin) {
		return ERR_INVALID_PARAMETER;
	}
	if (!bool(capabilities.get("precise_loop", false)) && !bool(capabilities.get("seek_loop", false))) {
		return ERR_UNAVAILABLE;
	}
	if (!_contains(p_begin, p_end)) {
		return ERR_INVALID_PARAMETER;
	}
	if (pcm.is_valid()) {
		const int64_t begin = Math::round(p_begin * pcm->get_mix_rate()), end = Math::round(p_end * pcm->get_mix_rate());
		if (begin < 0 || end <= begin || end > pcm->get_data().size() / 4) {
			return ERR_INVALID_PARAMETER;
		}
		AudioServer::get_singleton()->lock();
		pcm->set_loop_begin(begin);
		pcm->set_loop_end(end);
		pcm->set_loop_mode(AudioStreamWAV::LOOP_FORWARD);
		AudioServer::get_singleton()->unlock();
	}
	loop_begin = p_begin;
	loop_end = p_end;
	loop_active = true;
	Dictionary event;
	event["active"] = true;
	event["begin"] = loop_begin;
	event["end"] = loop_end;
	_event(SNAME("loop_changed"), event);
	return OK;
}
void MediaStreamPlayer::clear_loop() {
	const bool changed = loop_active;
	loop_active = false;
	if (pcm.is_valid()) {
		AudioServer::get_singleton()->lock();
		pcm->set_loop_mode(AudioStreamWAV::LOOP_DISABLED);
		AudioServer::get_singleton()->unlock();
	}
	if (changed) {
		Dictionary event;
		event["active"] = false;
		event["begin"] = loop_begin;
		event["end"] = loop_end;
		_event(SNAME("loop_changed"), event);
	}
}
void MediaStreamPlayer::set_volume_linear(float p_volume) {
	if (!Math::is_finite(p_volume)) {
		return;
	}
	volume = CLAMP(p_volume, 0.0f, 1.0f);
	audio->set_volume_linear(volume);
	video->set_volume(volume);
#ifdef WEB_ENABLED
	if (web_handle) {
		godot_web_media_command(web_handle, "volume", volume, 0);
	}
#endif
}

bool MediaStreamPlayer::is_preparing() const {
	if (decoder.is_valid() && decoder->is_decoding()) {
		return true;
	}
	for (const RetiringPreparation &retired : retiring_preparations) {
		if (retired.decoder->is_decoding()) {
			return true;
		}
	}
	return false;
}

Dictionary MediaStreamPlayer::get_preparation_state() const {
	auto identity = [](const Ref<AudioStreamPCMDecoder> &p_decoder, int64_t p_generation, const String &p_source) {
		Dictionary request;
		request["decoder_instance_id"] = int64_t(uint64_t(p_decoder->get_instance_id()));
		request["request_id"] = p_decoder->get_request_id();
		request["generation"] = p_generation;
		request["source"] = p_source;
		return request;
	};
	Dictionary active;
	if (decoder.is_valid() && decoder->is_decoding()) {
		active = identity(decoder, decoder_generation, source);
	}
	Array retiring;
	for (const RetiringPreparation &retired : retiring_preparations) {
		if (retired.decoder->is_decoding()) {
			retiring.push_back(identity(retired.decoder, retired.generation, retired.source));
		}
	}
	Dictionary state;
	state["playback_instance_id"] = int64_t(uint64_t(get_instance_id()));
	state["generation"] = generation;
	state["busy"] = !active.is_empty() || !retiring.is_empty();
	state["active_count"] = active.is_empty() ? 0 : 1;
	state["retiring_count"] = retiring.size();
	state["active"] = active;
	state["retiring"] = retiring;
	return state;
}

Dictionary MediaStreamPlayer::get_snapshot() const {
	Dictionary state;
	state["playback_instance_id"] = int64_t(uint64_t(get_instance_id()));
	state["generation"] = generation;
	state["source"] = source;
	state["ready"] = ready_state;
	state["buffering"] = buffering;
	state["preparation"] = get_preparation_state();
	state["paused"] = paused;
	state["position"] = position;
	state["duration"] = duration;
	state["volume"] = volume;
	state["seek_pending"] = seek_pending;
	state["seek_request_id"] = seek_request_id;
	state["metadata"] = metadata.duplicate(true);
	state["capabilities"] = capabilities.duplicate(true);
	state["seekable_ranges"] = ranges.duplicate(true);
	Dictionary loop;
	loop["active"] = loop_active;
	loop["begin"] = loop_begin;
	loop["end"] = loop_end;
	state["loop"] = loop;
	return state;
}

Rect2 MediaStreamPlayer::_video_rect(const Size2 &p_source_size) const {
	if (p_source_size.x <= 0 || p_source_size.y <= 0) {
		return Rect2(Point2(), get_size());
	}
	const real_t scale = MIN(get_size().x / p_source_size.x, get_size().y / p_source_size.y);
	const Size2 size = p_source_size * scale;
	return Rect2((get_size() - size) * 0.5, size);
}

void MediaStreamPlayer::_portal(bool p_enabled) {
	if (web_portal == p_enabled) {
		return;
	}
	web_portal = p_enabled;
	if (p_enabled) {
		Viewport *viewport = get_viewport();
		portal_viewport = viewport->get_instance_id();
		if (!media_portal_viewports.has(portal_viewport)) {
			MediaPortalViewport state;
			state.was_transparent = viewport->has_transparent_background();
			media_portal_viewports.insert(portal_viewport, state);
		}
		media_portal_viewports[portal_viewport].users++;
		viewport->set_transparent_background(true);
		saved_material = get_material();
		Ref<Shader> shader;
		shader.instantiate();
		shader->set_code("shader_type canvas_item; render_mode unshaded, blend_disabled; void fragment() { COLOR = vec4(0.0); }");
		Ref<ShaderMaterial> shader_material;
		shader_material.instantiate();
		shader_material->set_shader(shader);
		portal_material = shader_material;
		set_material(shader_material);
	} else {
		set_material(saved_material);
		portal_material.unref();
		saved_material.unref();
		if (media_portal_viewports.has(portal_viewport) && --media_portal_viewports[portal_viewport].users == 0) {
			Viewport *viewport = Object::cast_to<Viewport>(ObjectDB::get_instance(portal_viewport));
			if (viewport) {
				viewport->set_transparent_background(media_portal_viewports[portal_viewport].was_transparent);
			}
			media_portal_viewports.erase(portal_viewport);
		}
		portal_viewport = ObjectID();
	}
	queue_redraw();
}

void MediaStreamPlayer::_update_web_layout() {
#ifdef WEB_ENABLED
	if (!web_handle || !is_inside_tree()) {
		return;
	}
	portal_rect = _video_rect(web_source_size);
	const Transform2D transform = get_viewport()->get_final_transform() * get_global_transform_with_canvas();
	const Rect2 global = transform.xform(portal_rect);
	godot_web_media_rect(web_handle, global.position.x, global.position.y, global.size.x, global.size.y, is_visible_in_tree());
	queue_redraw();
#endif
}

void MediaStreamPlayer::_poll() {
	const int64_t current = generation;
#ifdef WEB_ENABLED
	if (web_handle) {
		char json[32768];
		if (!godot_web_media_poll(web_handle, json, sizeof(json))) {
			return;
		}
		const Variant value = JSON::parse_string(String::utf8(json));
		if (value.get_type() != Variant::DICTIONARY) {
			return;
		}
		const Dictionary state = value;
		position = state.get("position", 0.0);
		duration = state.get("duration", 0.0);
		_capabilities(state.get("capabilities", Dictionary()));
		if (current != generation) {
			return;
		}
		_ranges(state.get("ranges", Array()));
		if (current != generation) {
			return;
		}
		Dictionary next_metadata = state.get("metadata", Dictionary());
		next_metadata["source"] = source;
		if (next_metadata.has("title")) {
			next_metadata["title"] = FFmpegMediaMetadata::title(next_metadata["title"]);
		}
		if (next_metadata != metadata) {
			_native_metadata(next_metadata, current);
		}
		if (current != generation) {
			return;
		}
		_native_buffering(state.get("buffering", false), current);
		if (current != generation) {
			return;
		}
		if (bool(state.get("ready", false))) {
			_native_ready(current);
		}
		if (current != generation) {
			return;
		}
		const Array events = state.get("events", Array());
		for (int i = 0; i < events.size(); i++) {
			const Dictionary event = events[i];
			const String kind = event.get("type", "");
			if (kind == "error") {
				_native_error(event.get("error", ERR_CANT_CONNECT), event.get("message", "Browser playback failed."), current);
			} else if (kind == "seek" && int64_t(event.get("request", -1)) == seek_request_id) {
				_native_seek(event.get("position", position), current);
			} else if (kind == "finished") {
				_finished();
			}
			if (current != generation) {
				return;
			}
		}
		const bool portal = state.get("portal", false);
		_portal(portal);
		web_source_size = Size2(state.get("width", 0), state.get("height", 0));
		_update_web_layout();
		if (!portal && bool(options.get("video", false))) {
			const int width = state.get("width", 0), height = state.get("height", 0);
			if (width > 0 && height > 0 && width <= 1920 && height <= 1080) {
				web_pixels.resize(width * height * 4);
				if (godot_web_media_frame(web_handle, web_pixels.ptrw(), web_pixels.size(), width, height)) {
					Ref<Image> image = Image::create_from_data(width, height, false, Image::FORMAT_RGBA8, web_pixels);
					if (web_texture.is_null() || web_texture->get_width() != width || web_texture->get_height() != height) {
						web_texture = ImageTexture::create_from_image(image);
					} else {
						web_texture->update(image);
					}
				}
			}
		}
	} else
#endif
	{
		_native_bind();
		if (current != generation) {
			return;
		}
		const bool has_video = video->get_stream().is_valid();
		if (has_video) {
			duration = video->get_stream_length();
			position = video->get_stream_position();
		} else if (audio->get_stream().is_valid()) {
			duration = audio->get_stream()->get_length();
			position = audio->get_playback_position();
		}
		if (pcm.is_null() && ready_state) {
			Dictionary dvr;
			if (has_video) {
				dvr = video->get_dvr_window();
			}
#ifdef MODULE_MEDIA_STREAM_ENABLED
			else if (bound_playback.is_valid() && bound_playback->has_method(SNAME("get_dvr_window"))) {
				dvr = bound_playback->call(SNAME("get_dvr_window"));
			}
#endif
			const bool live = metadata.get("is_live", false), rewind = dvr.get("available", false);
			Dictionary caps;
			caps["precise_loop"] = false;
			caps["seek_loop"] = !live || rewind;
			caps["seek"] = !live || rewind;
			caps["live_playback"] = live;
			caps["dvr"] = rewind;
			caps["pcm_decode"] = !live;
			caps["backend"] = "native";
			caps["volume"] = true;
			caps["godot_audio_bus"] = true;
			_capabilities(caps);
			if (current != generation) {
				return;
			}
			Array next;
			if (rewind || (!live && duration > 0)) {
				Dictionary range;
				range["start"] = rewind ? double(dvr["start"]) : 0.0;
				// Reopening DVR inputs needs the backend's safe live headroom.
				// Do not advertise the newer manifest tail as precisely seekable:
				// requests there are necessarily clamped to live_position.
				range["end"] = rewind ? double(dvr["live_position"]) : duration;
				if (rewind) {
					range["timeline_origin_utc"] = dvr["timeline_origin_utc"];
				}
				next.push_back(range);
			}
			_ranges(next);
			if (current != generation) {
				return;
			}
		}
	}
	if (loop_active && !paused && can_process() && pcm.is_null() && !seek_pending && position >= loop_end) {
		seek(loop_begin);
	}
	queue_redraw();
}

void MediaStreamPlayer::_finished() {
	if (loop_active && pcm.is_null()) {
		if (!paused && can_process() && !seek_pending) {
			seek(loop_begin);
		}
		return;
	}
	_event(SNAME("finished"));
}

void MediaStreamPlayer::_notification(int p_what) {
	if (p_what == NOTIFICATION_PAUSED || p_what == NOTIFICATION_UNPAUSED || p_what == NOTIFICATION_SUSPENDED || p_what == NOTIFICATION_UNSUSPENDED) {
		_apply_pause();
		// Reapply after child notifications, which may otherwise forget an
		// explicitly paused audio stream when the scene resumes.
		callable_mp(this, &MediaStreamPlayer::_apply_pause).call_deferred();
	}
#ifdef WEB_ENABLED
	if (p_what == NOTIFICATION_VISIBILITY_CHANGED || p_what == NOTIFICATION_RESIZED || p_what == NOTIFICATION_TRANSFORM_CHANGED) {
		_update_web_layout();
	}
#endif
	if (p_what == NOTIFICATION_INTERNAL_PROCESS) {
		_collect_retired_preparations();
		if (opened) {
			_poll();
		} else if (retiring_preparations.is_empty()) {
			set_process_internal(false);
		}
	} else if (p_what == NOTIFICATION_ENTER_TREE && is_preparing()) {
		set_process_internal(true);
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		stop();
	} else if (p_what == NOTIFICATION_DRAW) {
		Ref<Texture2D> texture = web_texture;
		if (video->get_stream().is_valid()) {
			texture = video->get_video_texture();
		}
		if (texture.is_valid()) {
			draw_texture_rect(texture, _video_rect(texture->get_size()), false);
		} else if (web_portal) {
			draw_rect(portal_rect, Color(0, 0, 0, 0));
		}
	}
}

MediaStreamPlayer::MediaStreamPlayer() {
	set_mouse_filter(MOUSE_FILTER_IGNORE);
#ifdef WEB_ENABLED
	set_notify_transform(true);
#endif
	audio = memnew(AudioStreamPlayer);
	audio->set_process_mode(PROCESS_MODE_ALWAYS);
	add_child(audio, false, INTERNAL_MODE_BACK);
	video = memnew(VideoStreamPlayer);
	video->set_process_mode(PROCESS_MODE_ALWAYS);
	video->set_expand(true);
	video->hide();
	add_child(video, false, INTERNAL_MODE_BACK);
	audio->connect(SNAME("finished"), callable_mp(this, &MediaStreamPlayer::_finished));
	video->connect(SNAME("finished"), callable_mp(this, &MediaStreamPlayer::_finished));
}
MediaStreamPlayer::~MediaStreamPlayer() {
#ifdef WEB_ENABLED
	if (web_handle) {
		godot_web_media_close(web_handle);
	}
#endif
	if (decoder.is_valid()) {
		decoder->cancel();
	}
}
#endif
