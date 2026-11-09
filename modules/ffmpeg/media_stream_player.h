/**************************************************************************/
/*  media_stream_player.h                                              */
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
#if defined(WEB_ENABLED) || defined(MODULE_MEDIA_STREAM_ENABLED)
#include "audio_stream_pcm_decoder.h"

#include "core/templates/vector.h"
#include "scene/audio/audio_stream_player.h"
#include "scene/gui/control.h"
#include "scene/gui/video_stream_player.h"
#include "scene/resources/audio_stream_wav.h"
#include "scene/resources/image_texture.h"

// A controller owns one opening generation. Backends never own application
// queue entries, evidence identities, titles chosen by users, or server time.
class MediaStreamPlayer : public Control {
	GDCLASS(MediaStreamPlayer, Control);

	String source;
	Dictionary options;
	Dictionary metadata;
	Dictionary capabilities;
	Array ranges;
	Ref<AudioStreamPCMDecoder> decoder;
	int64_t decoder_generation = 0;
	struct RetiringPreparation {
		Ref<AudioStreamPCMDecoder> decoder;
		int64_t generation = 0;
		String source;
	};
	Vector<RetiringPreparation> retiring_preparations;
	Ref<AudioStreamWAV> pcm;
	AudioStreamPlayer *audio = nullptr;
	VideoStreamPlayer *video = nullptr;
	Ref<RefCounted> bound_playback;
	int64_t bound_generation = 0;
	struct RetiringBackend {
		Ref<RefCounted> fence;
		int64_t backend_instance_id = 0;
		int64_t generation = 0;
		String source;
	};
	Vector<RetiringBackend> retiring_backends;
	int64_t generation = 0;
	int64_t seek_request_id = 0;
	bool opened = false;
	bool ready_state = false;
	bool buffering = false;
	bool paused = false;
	bool play_requested = false;
	bool seek_pending = false;
	bool loop_active = false;
	double loop_begin = 0;
	double loop_end = 0;
	double position = 0;
	double duration = 0;
	float volume = 1;
	int web_handle = 0;
	Ref<ImageTexture> web_texture;
	Vector<uint8_t> web_pixels;
	bool web_portal = false;
	ObjectID portal_viewport;
	Rect2 portal_rect;
	Size2 web_source_size;
	Ref<Material> portal_material;
	Ref<Material> saved_material;

	void _reset();
	void _collect_retired_preparations();
	void _collect_retired_backends();
	void _event(const StringName &p_kind, Dictionary p_payload = Dictionary());
	void _capabilities(const Dictionary &p_capabilities);
	void _ranges(const Array &p_ranges);
	void _invalidate_loop(const String &p_reason);
	bool _contains(double p_begin, double p_end) const;
	void _pcm_completed(int p_error, const Dictionary &p_result, int64_t p_generation);
	Error _open_backend();
	void _poll();
	void _native_bind();
	void _native_unbind();
	void _native_ready(int64_t p_generation);
	void _native_buffering(bool p_buffering, int64_t p_generation);
	void _native_error(int p_error, const String &p_message, int64_t p_generation);
	void _native_seek(double p_position, int64_t p_generation);
	void _native_metadata(const Dictionary &p_metadata, int64_t p_generation);
	void _finished();
	Rect2 _video_rect(const Size2 &p_source_size) const;
	void _portal(bool p_enabled);
	void _update_web_layout();
	void _apply_pause();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	Error open(const String &p_source, const Dictionary &p_options = Dictionary());
	void stop();
	void play();
	void set_paused(bool p_paused);
	bool is_paused() const { return paused; }
	Error seek(double p_seconds);
	Error set_loop_range(double p_begin, double p_end);
	void clear_loop();
	void set_volume_linear(float p_volume);
	float get_volume_linear() const { return volume; }
	Dictionary get_snapshot() const;
	bool is_preparing() const;
	Dictionary get_preparation_state() const;
	bool is_backend_retiring() const;
	Dictionary get_backend_retirement_state() const;
	Dictionary get_capabilities() const { return capabilities.duplicate(true); }
	Array get_seekable_ranges() const { return ranges.duplicate(true); }
	int64_t get_generation() const { return generation; }
	MediaStreamPlayer();
	~MediaStreamPlayer();
};
#endif
