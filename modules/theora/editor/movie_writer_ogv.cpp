/**************************************************************************/
/*  movie_writer_ogv.cpp                                                  */
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

#include "movie_writer_ogv.h"

#include "rgb2yuv.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/os/os.h"

#include <cstring>

Error MovieWriterOGV::fail(Error p_error) {
	set_error(p_error);
	close();
	return p_error;
}

void MovieWriterOGV::close() {
	if (td) {
		th_encode_free(td);
		td = nullptr;
	}
	if (video_stream_initialized) {
		ogg_stream_clear(&to);
		video_stream_initialized = false;
	}
	if (audio_stream_initialized) {
		ogg_stream_clear(&vo);
		audio_stream_initialized = false;
	}
	if (vorbis_block_initialized) {
		vorbis_block_clear(&vb);
		vorbis_block_initialized = false;
	}
	if (vorbis_dsp_initialized) {
		vorbis_dsp_clear(&vd);
		vorbis_dsp_initialized = false;
	}
	if (vorbis_comment_initialized) {
		vorbis_comment_clear(&vc);
		vorbis_comment_initialized = false;
	}
	if (vorbis_info_initialized) {
		vorbis_info_clear(&vi);
		vorbis_info_initialized = false;
	}
	if (theora_comment_initialized) {
		th_comment_clear(&tc);
		theora_comment_initialized = false;
	}
	if (f.is_valid()) {
		f->close();
		set_error(f->get_error());
		f.unref();
	}
	yuv.clear();
	backup_page_data.clear();
	y = u = v = nullptr;
	audio_page = {};
	video_page = {};
	backup_page = {};
	audio_flag = video_flag = false;
	active = false;
	frame_count = 0;
}

Error MovieWriterOGV::store_page(const ogg_page &p_page) {
	if (f.is_null() || p_page.header_len < 0 || p_page.body_len < 0 ||
			(p_page.header_len > 0 && !p_page.header) || (p_page.body_len > 0 && !p_page.body)) {
		return ERR_INVALID_DATA;
	}
	if (!f->store_buffer(p_page.header, p_page.header_len) || !f->store_buffer(p_page.body, p_page.body_len)) {
		return ERR_FILE_CANT_WRITE;
	}
	return f->get_error();
}

Error MovieWriterOGV::push_audio(const int32_t *p_audio_data) {
	if (!p_audio_data) {
		return ERR_INVALID_PARAMETER;
	}
	float **buffer = vorbis_analysis_buffer(&vd, audio_frames);
	if (!buffer) {
		return ERR_OUT_OF_MEMORY;
	}
	// Godot supplies FL,FR,FC,LFE,BL,BR,SL,SR. Vorbis has a fixed
	// channel order and no 3.1 layout, so store 3.1 as 5.1 with silent rears.
	static const int channel_map[4][8] = {
		{ 0, 1 },
		{ 0, 2, 1, -1, -1, 3 },
		{ 0, 2, 1, 4, 5, 3 },
		{ 0, 2, 1, 6, 7, 4, 5, 3 },
	};
	const uint32_t encoded_channels = audio_ch == 4 ? 6 : audio_ch;
	for (uint32_t i = 0; i < audio_frames; i++) {
		for (uint32_t channel = 0; channel < encoded_channels; channel++) {
			const int source_channel = channel_map[uint32_t(speaker_mode)][channel];
			buffer[channel][i] = source_channel < 0 ? 0.0f : p_audio_data[i * audio_ch + source_channel] / 2147483647.f;
		}
	}
	return vorbis_analysis_wrote(&vd, audio_frames) == 0 ? OK : ERR_CANT_CREATE;
}

Error MovieWriterOGV::pull_audio(bool p_last) {
	if (p_last && vorbis_analysis_wrote(&vd, 0) != 0) {
		return ERR_CANT_CREATE;
	}
	int result;
	while ((result = vorbis_analysis_blockout(&vd, &vb)) > 0) {
		if (vorbis_analysis(&vb, nullptr) != 0 || vorbis_bitrate_addblock(&vb) != 0) {
			return ERR_CANT_CREATE;
		}
		ogg_packet packet;
		int packet_result;
		while ((packet_result = vorbis_bitrate_flushpacket(&vd, &packet)) > 0) {
			if (ogg_stream_packetin(&vo, &packet) != 0) {
				return ERR_CANT_CREATE;
			}
			// Put the initial zero-sample Vorbis packet on its own page. This
			// anchors short streams before their potentially truncated EOS page.
			if (packet.packetno == 3) {
				ogg_page first_page;
				int page_result;
				while ((page_result = ogg_stream_flush(&vo, &first_page)) > 0) {
					const Error error = store_page(first_page);
					if (error != OK) {
						return error;
					}
				}
				if (page_result < 0) {
					return ERR_CANT_CREATE;
				}
			}
		}
		if (packet_result < 0) {
			return ERR_CANT_CREATE;
		}
	}
	return result < 0 ? ERR_CANT_CREATE : OK;
}

Error MovieWriterOGV::push_video(const Ref<Image> &p_image) {
	const Vector<uint8_t> data = p_image->get_data();
	const uint64_t pixel_size = p_image->get_format() == Image::FORMAT_RGBA8 ? 4 : 3;
	const uint64_t expected = uint64_t(movie_size.width) * movie_size.height * pixel_size;
	if (uint64_t(data.size()) < expected) {
		return ERR_INVALID_DATA;
	}
	if (pixel_size == 4) {
		rgba2yuv420(y, u, v, data.ptr(), movie_size.width, movie_size.height);
	} else {
		rgb2yuv420(y, u, v, data.ptr(), movie_size.width, movie_size.height);
	}
	return th_encode_ycbcr_in(td, ycbcr) == 0 ? OK : ERR_CANT_CREATE;
}

Error MovieWriterOGV::pull_video(bool p_last) {
	ogg_packet packet;
	int result;
	while ((result = th_encode_packetout(td, p_last, &packet)) > 0) {
		if (ogg_stream_packetin(&to, &packet) != 0) {
			return ERR_CANT_CREATE;
		}
	}
	return result < 0 ? ERR_CANT_CREATE : OK;
}

uint32_t MovieWriterOGV::get_audio_mix_rate() const {
	return mix_rate;
}
AudioServer::SpeakerMode MovieWriterOGV::get_audio_speaker_mode() const {
	return speaker_mode;
}
bool MovieWriterOGV::handles_file(const String &p_path) const {
	return p_path.has_extension("ogv");
}
void MovieWriterOGV::get_supported_extensions(List<String> *r_extensions) const {
	r_extensions->push_back("ogv");
}

Error MovieWriterOGV::write_begin(const Size2i &p_movie_size, uint32_t p_fps, const String &p_base_path) {
	close();
	reset_error();
	if (p_movie_size.width <= 0 || p_movie_size.height <= 0 ||
			p_movie_size.width > INT32_MAX - 15 || p_movie_size.height > INT32_MAX - 15 ||
			(p_movie_size.width & 1) || (p_movie_size.height & 1) ||
			p_fps == 0 || p_fps > INT32_MAX || mix_rate < 8000 || mix_rate > 192000 || mix_rate / p_fps == 0 ||
			speaker_mode < AudioServer::SPEAKER_MODE_STEREO || speaker_mode > AudioServer::SPEAKER_SURROUND_71 ||
			!Math::is_finite(audio_quality) || audio_quality < -0.1f || audio_quality > 1.0f ||
			!Math::is_finite(video_quality) || video_quality < 0.0f || video_quality > 1.0f ||
			speed < 1 || speed > 4 || keyframe_frequency < 1 || keyframe_frequency > 1024) {
		return fail(ERR_INVALID_PARAMETER);
	}
	const uint64_t pixels = uint64_t(p_movie_size.width) * p_movie_size.height;
	if (pixels + pixels / 2 > INT32_MAX) {
		return fail(ERR_INVALID_PARAMETER);
	}
	if (yuv.resize(pixels + pixels / 2) != OK) {
		return fail(ERR_OUT_OF_MEMORY);
	}
	movie_size = p_movie_size;
	y = yuv.ptrw();
	u = y + pixels;
	v = u + pixels / 4;
	ycbcr[0] = { movie_size.width, movie_size.height, movie_size.width, y };
	ycbcr[1] = { movie_size.width / 2, movie_size.height / 2, movie_size.width / 2, u };
	ycbcr[2] = { movie_size.width / 2, movie_size.height / 2, movie_size.width / 2, v };
	fps = p_fps;
	audio_ch = (uint32_t(speaker_mode) + 1) * 2;
	audio_frames = mix_rate / fps;

	base_path = p_base_path.get_basename() + ".ogv";
	if (base_path.is_relative_path()) {
		base_path = "res://" + base_path;
	}
	Error open_error = OK;
	f = FileAccess::open(base_path, FileAccess::WRITE_READ, &open_error);
	if (f.is_null()) {
		return fail(open_error == OK ? ERR_CANT_OPEN : open_error);
	}

	const uint32_t serial = uint32_t(OS::get_singleton()->get_ticks_usec()) ^ uint32_t(uint64_t(get_instance_id()));
	if (ogg_stream_init(&to, int(serial)) != 0) {
		return fail(ERR_OUT_OF_MEMORY);
	}
	video_stream_initialized = true;
	if (ogg_stream_init(&vo, int(serial ^ 0x9e3779b9U)) != 0) {
		return fail(ERR_OUT_OF_MEMORY);
	}
	audio_stream_initialized = true;
	vorbis_info_init(&vi);
	vorbis_info_initialized = true;
	if (vorbis_encode_init_vbr(&vi, audio_ch == 4 ? 6 : audio_ch, mix_rate, audio_quality) != 0) {
		return fail(ERR_UNAVAILABLE);
	}
	vorbis_comment_init(&vc);
	vorbis_comment_initialized = true;
	if (vorbis_analysis_init(&vd, &vi) != 0) {
		return fail(ERR_UNAVAILABLE);
	}
	vorbis_dsp_initialized = true;
	if (vorbis_block_init(&vd, &vb) != 0) {
		return fail(ERR_OUT_OF_MEMORY);
	}
	vorbis_block_initialized = true;

	th_info info;
	th_info_init(&info);
	info.frame_width = (movie_size.width + 15) & ~0xf;
	info.frame_height = (movie_size.height + 15) & ~0xf;
	info.pic_width = movie_size.width;
	info.pic_height = movie_size.height;
	info.pic_x = 0;
	info.pic_y = 0;
	info.fps_numerator = fps;
	info.fps_denominator = 1;
	info.aspect_numerator = info.aspect_denominator = 1;
	info.colorspace = TH_CS_UNSPECIFIED;
	info.target_bitrate = int(64870 * int64_t(video_bitrate) >> 16);
	info.quality = int(video_quality * 63);
	info.pixel_fmt = TH_PF_420;
	td = th_encode_alloc(&info);
	th_info_clear(&info);
	if (!td) {
		return fail(ERR_UNAVAILABLE);
	}
	if (th_encode_ctl(td, TH_ENCCTL_SET_KEYFRAME_FREQUENCY_FORCE, &keyframe_frequency, sizeof(keyframe_frequency)) < 0) {
		return fail(ERR_UNAVAILABLE);
	}
	int speed_max = 0;
	if (th_encode_ctl(td, TH_ENCCTL_GET_SPLEVEL_MAX, &speed_max, sizeof(speed_max)) < 0 || speed_max < 0) {
		return fail(ERR_UNAVAILABLE);
	}
	int selected_speed = MIN(speed, speed_max);
	if (th_encode_ctl(td, TH_ENCCTL_SET_SPLEVEL, &selected_speed, sizeof(selected_speed)) < 0) {
		return fail(ERR_UNAVAILABLE);
	}

	th_comment_init(&tc);
	theora_comment_initialized = true;
	ogg_packet packet;
	if (th_encode_flushheader(td, &tc, &packet) <= 0 || ogg_stream_packetin(&to, &packet) != 0 ||
			ogg_stream_pageout(&to, &video_page) != 1) {
		return fail(ERR_CANT_CREATE);
	}
	Error error = store_page(video_page);
	if (error != OK) {
		return fail(error);
	}
	int result;
	while ((result = th_encode_flushheader(td, &tc, &packet)) > 0) {
		if (ogg_stream_packetin(&to, &packet) != 0) {
			return fail(ERR_CANT_CREATE);
		}
	}
	if (result < 0) {
		return fail(ERR_CANT_CREATE);
	}
	ogg_packet id, comment, code;
	if (vorbis_analysis_headerout(&vd, &vc, &id, &comment, &code) != 0 || ogg_stream_packetin(&vo, &id) != 0 ||
			ogg_stream_pageout(&vo, &audio_page) != 1) {
		return fail(ERR_CANT_CREATE);
	}
	error = store_page(audio_page);
	if (error != OK) {
		return fail(error);
	}
	if (ogg_stream_packetin(&vo, &comment) != 0 || ogg_stream_packetin(&vo, &code) != 0) {
		return fail(ERR_CANT_CREATE);
	}
	while ((result = ogg_stream_flush(&to, &video_page)) > 0) {
		error = store_page(video_page);
		if (error != OK) {
			return fail(error);
		}
	}
	if (result < 0) {
		return fail(ERR_CANT_CREATE);
	}
	while ((result = ogg_stream_flush(&vo, &audio_page)) > 0) {
		error = store_page(audio_page);
		if (error != OK) {
			return fail(error);
		}
	}
	if (result < 0) {
		return fail(ERR_CANT_CREATE);
	}
	active = true;
	return OK;
}

// Keep one submitted frame pending so finalization can mark the actual last packet.
Error MovieWriterOGV::write_frame(const Ref<Image> &p_image, const int32_t *p_audio_data) {
	if (get_error() != OK) {
		return get_error();
	}
	if (!active || p_image.is_null() || p_image->is_empty() || p_image->get_size() != movie_size || !p_audio_data) {
		return fail(ERR_INVALID_PARAMETER);
	}
	Ref<Image> image = p_image;
	if (image->get_format() != Image::FORMAT_RGB8 && image->get_format() != Image::FORMAT_RGBA8) {
		image = p_image->duplicate();
		if (image.is_null()) {
			return fail(ERR_OUT_OF_MEMORY);
		}
		image->convert(Image::FORMAT_RGBA8);
		if (image->is_empty() || image->get_format() != Image::FORMAT_RGBA8 || image->get_size() != movie_size) {
			return fail(ERR_CANT_CREATE);
		}
	}
	if (frame_count >= INT64_MAX) {
		return fail(ERR_INVALID_PARAMETER);
	}
	Error error = pull_audio();
	if (error == OK) {
		error = pull_video();
	}
	if (error == OK && ((frame_count + 1) % 8) == 0) {
		error = write_to_file();
	}
	if (error == OK) {
		error = push_audio(p_audio_data);
	}
	if (error == OK) {
		error = push_video(image);
	}
	if (error != OK) {
		return fail(error);
	}
	frame_count++;
	return OK;
}

Error MovieWriterOGV::save_page(const ogg_page &p_page) {
	if (p_page.header_len < 0 || p_page.body_len < 0 || !p_page.header || (p_page.body_len && !p_page.body)) {
		return ERR_INVALID_DATA;
	}
	const uint64_t size = uint64_t(p_page.header_len) + uint64_t(p_page.body_len);
	if (size > INT32_MAX) {
		return ERR_INVALID_DATA;
	}
	if (p_page.header == backup_page_data.ptr()) {
		return OK;
	}
	if (backup_page_data.resize(size) != OK) {
		return ERR_OUT_OF_MEMORY;
	}
	uint8_t *data = backup_page_data.ptrw();
	backup_page = {};
	backup_page.header = data;
	backup_page.header_len = p_page.header_len;
	backup_page.body = data + p_page.header_len;
	backup_page.body_len = p_page.body_len;
	memcpy(backup_page.header, p_page.header, p_page.header_len);
	if (p_page.body_len) {
		memcpy(backup_page.body, p_page.body, p_page.body_len);
	}
	return OK;
}

Error MovieWriterOGV::write_to_file(bool p_finish) {
	if (audio_flag) {
		audio_page = backup_page;
	} else {
		const int result = ogg_stream_flush(&vo, &audio_page);
		if (result < 0) {
			return ERR_CANT_CREATE;
		}
		audio_flag = result > 0;
	}
	if (video_flag) {
		video_page = backup_page;
	} else {
		const int result = ogg_stream_flush(&to, &video_page);
		if (result < 0) {
			return ERR_CANT_CREATE;
		}
		video_flag = result > 0;
	}
	while ((audio_flag && video_flag) || (p_finish && (audio_flag || video_flag))) {
		const bool video_first = video_flag && (!audio_flag || th_granule_time(td, ogg_page_granulepos(&video_page)) <= vorbis_granule_time(&vd, ogg_page_granulepos(&audio_page)));
		Error error = store_page(video_first ? video_page : audio_page);
		if (error != OK) {
			return error;
		}
		const int result = video_first ? ogg_stream_flush(&to, &video_page) : ogg_stream_flush(&vo, &audio_page);
		if (result < 0) {
			return ERR_CANT_CREATE;
		}
		if (video_first) {
			video_flag = result > 0;
		} else {
			audio_flag = result > 0;
		}
	}
	return video_flag ? save_page(video_page) : audio_flag ? save_page(audio_page)
														   : OK;
}

void MovieWriterOGV::write_end() {
	if (active && get_error() == OK) {
		active = false;
		Error error = pull_audio(true);
		if (error == OK) {
			error = pull_video(true);
		}
		if (error == OK) {
			error = write_to_file(true);
		}
		set_error(error);
		if (f.is_valid()) {
			f->flush();
			set_error(f->get_error());
		}
	}
	close();
}

MovieWriterOGV::MovieWriterOGV() {
	mix_rate = GLOBAL_GET("editor/movie_writer/mix_rate");
	speaker_mode = AudioServer::SpeakerMode(int(GLOBAL_GET("editor/movie_writer/speaker_mode")));
	video_quality = GLOBAL_GET("editor/movie_writer/video_quality");
	audio_quality = GLOBAL_GET("editor/movie_writer/ogv/audio_quality");
	speed = GLOBAL_GET("editor/movie_writer/ogv/encoding_speed");
	keyframe_frequency = GLOBAL_GET("editor/movie_writer/ogv/keyframe_interval");
}
MovieWriterOGV::~MovieWriterOGV() {
	close();
}
