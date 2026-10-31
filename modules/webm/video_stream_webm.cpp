/**************************************************************************/
/*  video_stream_webm.cpp                                                 */
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

#include "video_stream_webm.h"

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_FFMPEG_ENABLED
#include "modules/ffmpeg/video_stream_media.h"
#endif

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "servers/audio/audio_server.h"

#ifndef MODULE_FFMPEG_ENABLED
#include "thirdparty/misc/yuv2rgb.h"

#include "core/io/image_loader_safety.h"

#include <climits>

// libsimplewebm
#include <OpusVorbisDecoder.hpp>
#include <VPXDecoder.hpp>

// libvpx
#include <vpx/vpx_image.h>

// libwebm
#include <mkvparser/mkvparser.h>

class MkvReader : public mkvparser::IMkvReader {
public:
	MkvReader(const Ref<FileAccess> &p_file) : file(p_file) {}
	~MkvReader() {
		if (file.is_valid()) {
			file.unref();
		}
	}

	virtual int Read(long long pos, long len, unsigned char *buf) {
		if (file.is_null() || pos < 0 || len < 0 || uint64_t(pos) > file->get_length() || uint64_t(len) > file->get_length() - uint64_t(pos)) {
			return -1;
		}
		file->seek(pos);
		if (file->get_position() != uint64_t(pos)) {
			return -1;
		}
		const uint64_t read = file->get_buffer(buf, len);
		const Error error = file->get_error();
		return read == uint64_t(len) && (error == OK || error == ERR_FILE_EOF) ? 0 : -1;
	}

	virtual int Length(long long *total, long long *available) {
		if (file.is_valid()) {
			const uint64_t len = file->get_length();
			if (len > uint64_t(LLONG_MAX)) {
				return -1;
			}
			if (total) {
				*total = len;
			}
			if (available) {
				*available = len;
			}
			return 0;
		}
		return -1;
	}

private:
	Ref<FileAccess> file;
};

/**/

VideoStreamPlaybackWebm::VideoStreamPlaybackWebm() :
		audio_track(0),
		webm(nullptr),
		video(nullptr),
		audio(nullptr),
		video_frames(nullptr),
		audio_frame(nullptr),
		video_frames_pos(0),
		video_frames_capacity(0),
		num_decoded_samples(0),
		samples_offset(-1),
		mix_callback(nullptr),
		mix_udata(nullptr),
		playing(false),
		paused(false),
		delay_compensation(0.0),
		time(0.0),
		video_frame_delay(0.0),
		video_pos(0.0),
		texture(memnew(ImageTexture)),
		pcm(nullptr) {}
VideoStreamPlaybackWebm::~VideoStreamPlaybackWebm() {
	delete_pointers();
}

bool VideoStreamPlaybackWebm::open_file(const String &p_file) {
	delete_pointers();
	file_name = p_file;
	auto fail = [&]() { delete_pointers(); return false; };
	Ref<FileAccess> source = FileAccess::open(file_name, FileAccess::READ);
	if (source.is_null() || source->get_length() > uint64_t(LLONG_MAX)) {
		return false;
	}
	webm = memnew(WebMDemuxer(new MkvReader(source), 0, audio_track));
	if (!webm->isOpen() || webm->getVideoCodec() == WebMDemuxer::NO_VIDEO) {
		return fail();
	}
	const int width = webm->getWidth();
	const int height = webm->getHeight();
	int64_t bytes = 0;
	if (ImageLoaderSafety::image_bytes(width, height, 4, bytes) != OK || frame_data.resize(bytes) != OK) {
		return fail();
	}
	frame_data.fill(0);
	video = memnew(VPXDecoder(*webm, OS::get_singleton()->get_processor_count()));
	if (!video->isOpen()) {
		return fail();
	}

	if (webm->getAudioCodec() != WebMDemuxer::NO_AUDIO) {
		const int channels = webm->getChannels();
		const double rate = webm->getSampleRate();
		if ((channels == 1 || channels == 2 || channels == 4 || channels == 6 || channels == 8) && Math::is_finite(rate) && rate > 0 && rate <= INT_MAX && rate == int(rate)) {
			audio = memnew(OpusVorbisDecoder(*webm));
			if (audio->isOpen()) {
				const int frames = audio->getBufferSamples();
				if (frames <= 0 || uint64_t(frames) > SIZE_MAX / sizeof(float) / channels || pcm_data.resize(int64_t(frames) * channels) != OK) {
					return fail();
				}
				pcm = pcm_data.ptrw();
				audio_frame = memnew(WebMFrame);
			} else {
				memdelete(audio);
				audio = nullptr;
			}
		}
	}
	const int channels = audio ? webm->getChannels() : 0;
	const int mix_rate = audio ? int(webm->getSampleRate()) : 0;
	if (output_channels >= 0 && (channels != output_channels || mix_rate != output_mix_rate)) {
		ERR_PRINT("Native WebM audio format changed during restart. Recreate the playback instance to change channel count or sample rate.");
		return fail();
	}
	Ref<Image> initial_image = Image::create_from_data(width, height, false, Image::FORMAT_RGBA8, frame_data);
	if (initial_image.is_null() || initial_image->is_empty()) {
		return fail();
	}
	texture->set_image(initial_image);
	output_channels = channels;
	output_mix_rate = mix_rate;
	return true;
}

void VideoStreamPlaybackWebm::stop() {
	if (playing) {
		// Rewind only to a successfully reopened decoder; failure remains stopped.
		open_file(file_name);
	}
	playing = false;
	time = video_pos = 0.0;
}
void VideoStreamPlaybackWebm::play() {
	stop();
	if (!video && !open_file(file_name)) {
		return;
	}
	delay_compensation = ProjectSettings::get_singleton()->get("audio/video_delay_compensation_ms");
	delay_compensation /= 1000.0;
	playing = true;
}

bool VideoStreamPlaybackWebm::is_playing() const {
	return playing;
}

void VideoStreamPlaybackWebm::set_paused(bool p_paused) {
	paused = p_paused;
}
bool VideoStreamPlaybackWebm::is_paused() const {
	return paused;
}

void VideoStreamPlaybackWebm::set_loop(bool p_enable) {
	//Empty
}
bool VideoStreamPlaybackWebm::has_loop() const {
	return false;
}

double VideoStreamPlaybackWebm::get_length() const {
	if (webm) {
		return webm->getLength();
	}
	return 0.0f;
}

double VideoStreamPlaybackWebm::get_playback_position() const {
	return video_pos;
}
void VideoStreamPlaybackWebm::seek(double p_time) {
	WARN_PRINT_ONCE("Seeking in Theora and WebM videos is not implemented yet (it's only supported for GDNative-provided video streams).");
}

void VideoStreamPlaybackWebm::set_audio_track(int p_idx) {
	audio_track = p_idx;
}

Ref<Texture2D> VideoStreamPlaybackWebm::get_texture() const {
	return texture;
}

void VideoStreamPlaybackWebm::update(double p_delta) {
	if ((!playing || paused) || !video) {
		return;
	}

	time += p_delta;

	if (time < video_pos) {
		return;
	}

	bool audio_buffer_full = false;

	if (samples_offset > -1 && !mix_callback) {
		samples_offset = -1;
	}
	if (samples_offset > -1) {
		//Mix remaining samples
		const int to_read = num_decoded_samples - samples_offset;
		const int mixed = mix_callback(mix_udata, pcm + samples_offset * webm->getChannels(), to_read);
		if (mixed < 0 || mixed > to_read) {
			playing = false;
			return;
		}
		if (mixed != to_read) {
			samples_offset += mixed;
			audio_buffer_full = true;
		} else {
			samples_offset = -1;
		}
	}

	const bool hasAudio = (audio && mix_callback);
	while ((hasAudio && !audio_buffer_full && !has_enough_video_frames()) ||
			(!hasAudio && video_frames_pos == 0)) {
		if (hasAudio && !audio_buffer_full && audio_frame->isValid() &&
				audio->getPCMF(*audio_frame, pcm, num_decoded_samples) && num_decoded_samples > 0) {
			const int mixed = mix_callback(mix_udata, pcm, num_decoded_samples);
			if (mixed < 0 || mixed > num_decoded_samples) {
				playing = false;
				return;
			}

			if (mixed != num_decoded_samples) {
				samples_offset = mixed;
				audio_buffer_full = true;
			}
		}

		WebMFrame *video_frame;
		if (video_frames_pos >= video_frames_capacity) {
			if (video_frames_capacity == INT_MAX || uint64_t(video_frames_capacity) + 1 > SIZE_MAX / sizeof(WebMFrame *)) {
				playing = false;
				return;
			}
			const int capacity = video_frames_capacity + 1;
			WebMFrame **grown = (WebMFrame **)memrealloc(video_frames, size_t(capacity) * sizeof(WebMFrame *));
			if (!grown) {
				playing = false;
				return;
			}
			video_frames = grown;
			video_frames[video_frames_capacity] = memnew(WebMFrame);
			video_frames_capacity = capacity;
		}
		video_frame = video_frames[video_frames_pos];

		if (!webm->readFrame(video_frame, audio_frame)) { //This will invalidate frames
			break; //Can't demux, EOS?
		}

		if (video_frame->isValid()) {
			++video_frames_pos;
		}
	};

	bool video_frame_done = false;
	while (video_frames_pos > 0 && !video_frame_done) {
		WebMFrame *video_frame = video_frames[0];

		// It seems VPXDecoder::decode has to be executed even though we might skip this frame
		if (video->decode(*video_frame)) {
			VPXDecoder::IMAGE_ERROR err;
			VPXDecoder::Image image;

			if (should_process(*video_frame)) {
				if ((err = video->getImage(image)) != VPXDecoder::NO_FRAME) {
					if (err == VPXDecoder::NO_ERROR && image.w == webm->getWidth() && image.h == webm->getHeight()) {
						Vector<uint8_t> decoded;
						if (decoded.resize(frame_data.size()) != OK) {
							playing = false;
							return;
						}
						uint8_t *w = decoded.ptrw();
						bool converted = false;

						if (image.chromaShiftW == 0 && image.chromaShiftH == 0 && image.cs == VPX_CS_SRGB) {
							uint8_t *wp = w;
							unsigned char *rRow = image.planes[2];
							unsigned char *gRow = image.planes[0];
							unsigned char *bRow = image.planes[1];
							for (int i = 0; i < image.h; i++) {
								for (int j = 0; j < image.w; j++) {
									*wp++ = rRow[j];
									*wp++ = gRow[j];
									*wp++ = bRow[j];
									*wp++ = 255;
								}
								rRow += image.linesize[2];
								gRow += image.linesize[0];
								bRow += image.linesize[1];
							}
							converted = true;
						} else if (image.chromaShiftW == 1 && image.chromaShiftH == 1) {
							yuv420_2_rgb8888(w, image.planes[0], image.planes[1], image.planes[2], image.w, image.h, image.linesize[0], image.linesize[1], image.w << 2);
							//libyuv::I420ToARGB(image.planes[0], image.linesize[0], image.planes[2], image.linesize[2], image.planes[1], image.linesize[1], w, image.w << 2, image.w, image.h);
							converted = true;
						} else if (image.chromaShiftW == 1 && image.chromaShiftH == 0) {
							yuv422_2_rgb8888(w, image.planes[0], image.planes[1], image.planes[2], image.w, image.h, image.linesize[0], image.linesize[1], image.w << 2);
							//libyuv::I422ToARGB(image.planes[0], image.linesize[0], image.planes[2], image.linesize[2], image.planes[1], image.linesize[1], w, image.w << 2, image.w, image.h);
							converted = true;
						} else if (image.chromaShiftW == 0 && image.chromaShiftH == 0) {
							yuv444_2_rgb8888(w, image.planes[0], image.planes[1], image.planes[2], image.w, image.h, image.linesize[0], image.linesize[1], image.w << 2);
							//libyuv::I444ToARGB(image.planes[0], image.linesize[0], image.planes[2], image.linesize[2], image.planes[1], image.linesize[1], w, image.w << 2, image.w, image.h);
							converted = true;
						} else if (image.chromaShiftW == 2 && image.chromaShiftH == 0) {
							//libyuv::I411ToARGB(image.planes[0], image.linesize[0], image.planes[2], image.linesize[2] image.planes[1], image.linesize[1], w, image.w << 2, image.w, image.h);
							//converted = true;
						}

						if (converted) {
							Ref<Image> img = Image::create_from_data(image.w, image.h, false, Image::FORMAT_RGBA8, decoded);
							if (img.is_null() || img->is_empty()) {
								playing = false;
								return;
							}
							frame_data = std::move(decoded);
							texture->update(img); // Zero-copy send to rendering server
							video_frame_done = true;
						}
					}
				}
			}
		}

		video_pos = video_frame->time;
		memmove(video_frames, video_frames + 1, (--video_frames_pos) * sizeof(void *));
		video_frames[video_frames_pos] = video_frame;
	}

	if (video_frames_pos == 0 && webm->isEOS()) {
		stop();
	}
}

void VideoStreamPlaybackWebm::set_mix_callback(VideoStreamPlayback::AudioMixCallback p_callback, void *p_userdata) {
	mix_callback = p_callback;
	mix_udata = p_userdata;
}
int VideoStreamPlaybackWebm::get_channels() const {
	if (audio) {
		return webm->getChannels();
	}
	return 0;
}
int VideoStreamPlaybackWebm::get_mix_rate() const {
	if (audio) {
		return webm->getSampleRate();
	}
	return 0;
}

inline bool VideoStreamPlaybackWebm::has_enough_video_frames() const {
	if (video_frames_pos > 0) {
		// FIXME: AudioServer output latency was fixed in af9bb0e, previously it used to
		// systematically return 0. Now that it gives a proper latency, it broke this
		// code where the delay compensation likely never really worked.
		//const double audio_delay = AudioServer::get_singleton()->get_output_latency();
		const double video_time = video_frames[video_frames_pos - 1]->time;
		return video_time >= time + /* audio_delay + */ delay_compensation;
	}
	return false;
}

bool VideoStreamPlaybackWebm::should_process(WebMFrame &video_frame) {
	// FIXME: AudioServer output latency was fixed in af9bb0e, previously it used to
	// systematically return 0. Now that it gives a proper latency, it broke this
	// code where the delay compensation likely never really worked.
	//const double audio_delay = AudioServer::get_singleton()->get_output_latency();
	return video_frame.time >= time + /* audio_delay + */ delay_compensation;
}

void VideoStreamPlaybackWebm::delete_pointers() {
	pcm = nullptr;
	pcm_data.clear();

	if (audio_frame) {
		memdelete(audio_frame);
	}
	if (video_frames) {
		for (int i = 0; i < video_frames_capacity; ++i) {
			memdelete(video_frames[i]);
		}
		memfree(video_frames);
	}

	if (video) {
		memdelete(video);
	}
	if (audio) {
		memdelete(audio);
	}

	if (webm) {
		memdelete(webm);
	}
	webm = nullptr;
	video = nullptr;
	audio = nullptr;
	audio_frame = nullptr;
	video_frames = nullptr;
	video_frames_pos = video_frames_capacity = 0;
	num_decoded_samples = 0;
	samples_offset = -1;
	playing = false;
	time = video_frame_delay = video_pos = 0.0;
	frame_data.clear();
}
#endif

/**/

VideoStreamWebm::VideoStreamWebm() :
		audio_track(0) {}

Ref<VideoStreamPlayback> VideoStreamWebm::instantiate_playback() {
#ifdef MODULE_FFMPEG_ENABLED
	Ref<VideoStreamMedia> media;
	media.instantiate();
	media->set_file(file);
	media->set_audio_track(audio_track);
	return media->instantiate_playback();
#else
	Ref<VideoStreamPlaybackWebm> playback;
	playback.instantiate();
	playback->set_audio_track(audio_track);
	if (!playback->open_file(file)) {
		return Ref<VideoStreamPlayback>();
	}
	return playback;
#endif
}

void VideoStreamWebm::_bind_methods() {}

void VideoStreamWebm::set_audio_track(int p_track) {
	audio_track = p_track;
}

////////////

Ref<Resource> ResourceFormatLoaderWebm::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (!f.is_valid()) {
		if (r_error) {
			*r_error = ERR_CANT_OPEN;
		}
		return Ref<Resource>();
	}

	Ref<VideoStreamWebm> webm_stream;
	webm_stream.instantiate();
	webm_stream->set_file(p_path);

	if (r_error) {
		*r_error = OK;
	}
	return webm_stream;
}

void ResourceFormatLoaderWebm::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("webm");
}

bool ResourceFormatLoaderWebm::handles_type(const String &p_type) const {
	return ClassDB::is_parent_class(p_type, "VideoStream");
}

String ResourceFormatLoaderWebm::get_resource_type(const String &p_path) const {
	String el = p_path.get_extension().to_lower();
	if (el == "webm") {
		return "VideoStreamWebm";
	}
	return "";
}
