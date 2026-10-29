/**************************************************************************/
/*  movie_writer_ogv.h                                                    */
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

#include "servers/audio/audio_server.h"
#include "servers/movie_writer/movie_writer.h"

#include <theora/theoraenc.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>

class MovieWriterOGV : public MovieWriter {
	GDCLASS(MovieWriterOGV, MovieWriter)

	uint32_t mix_rate = 48000;
	AudioServer::SpeakerMode speaker_mode = AudioServer::SPEAKER_MODE_STEREO;
	String base_path;
	Size2i movie_size;
	uint64_t frame_count = 0;
	uint32_t fps = 0;
	uint32_t audio_ch = 0;
	uint32_t audio_frames = 0;
	Ref<FileAccess> f;
	float audio_quality = 0.5;
	int video_bitrate = 0;
	float video_quality = 0.75;
	ogg_uint32_t keyframe_frequency = 64;
	int speed = 4;

	ogg_stream_state to = {};
	ogg_stream_state vo = {};
	th_enc_ctx *td = nullptr;
	th_comment tc = {};
	vorbis_info vi = {};
	vorbis_comment vc = {};
	vorbis_dsp_state vd = {};
	vorbis_block vb = {};
	bool video_stream_initialized = false;
	bool audio_stream_initialized = false;
	bool theora_comment_initialized = false;
	bool vorbis_info_initialized = false;
	bool vorbis_comment_initialized = false;
	bool vorbis_dsp_initialized = false;
	bool vorbis_block_initialized = false;
	bool active = false;

	Vector<uint8_t> yuv;
	uint8_t *y = nullptr;
	uint8_t *u = nullptr;
	uint8_t *v = nullptr;
	th_ycbcr_buffer ycbcr = {};
	bool audio_flag = false;
	bool video_flag = false;
	ogg_page audio_page = {};
	ogg_page video_page = {};
	ogg_page backup_page = {};
	Vector<uint8_t> backup_page_data;

	void close();
	Error fail(Error p_error);
	Error store_page(const ogg_page &p_page);
	Error write_to_file(bool p_finish = false);
	Error push_audio(const int32_t *p_audio_data);
	Error push_video(const Ref<Image> &p_image);
	Error pull_audio(bool p_last = false);
	Error pull_video(bool p_last = false);
	Error save_page(const ogg_page &p_page);

protected:
	uint32_t get_audio_mix_rate() const override;
	AudioServer::SpeakerMode get_audio_speaker_mode() const override;
	void get_supported_extensions(List<String> *r_extensions) const override;
	Error write_begin(const Size2i &p_movie_size, uint32_t p_fps, const String &p_base_path) override;
	Error write_frame(const Ref<Image> &p_image, const int32_t *p_audio_data) override;
	void write_end() override;
	bool handles_file(const String &p_path) const override;

public:
	MovieWriterOGV();
	~MovieWriterOGV() override;
};
