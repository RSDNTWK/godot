/**************************************************************************/
/*  pcm_decoder.cpp                                              */
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

// Standalone FFmpeg worker module. Never linked into the browser UI runtime.
#include <emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace {
const uint8_t *input = nullptr;
int64_t input_size = 0, input_position = 0;
double deadline = 0;
std::vector<uint8_t> output;
std::string title;
int read_input(void *, uint8_t *buffer, int count) {
	count = int(std::min<int64_t>(count, input_size - input_position));
	if (count <= 0) {
		return AVERROR_EOF;
	}
	memcpy(buffer, input + input_position, count);
	input_position += count;
	return count;
}
int64_t seek_input(void *, int64_t offset, int whence) {
	if (whence == AVSEEK_SIZE) {
		return input_size;
	}
	whence &= ~AVSEEK_FORCE;
	int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? input_position
			: whence == SEEK_END							   ? input_size
															   : -1;
	if (base < 0 || offset < -base || offset > input_size - base) {
		return AVERROR(EINVAL);
	}
	return input_position = base + offset;
}
int interrupt(void *) {
	return emscripten_get_now() >= deadline;
}
int deny_secondary(AVFormatContext *, AVIOContext **, const char *, int, AVDictionary **) {
	return AVERROR(EACCES);
}
} //namespace

extern "C" EMSCRIPTEN_KEEPALIVE int godot_pcm_decode(const uint8_t *data, int size, int rate, int limit, int timeout) {
	output.clear();
	title.clear();
	if (!data || size <= 0 || size > 64 * 1024 * 1024 || rate < 8000 || rate > 192000 || limit < 4 || limit > 128 * 1024 * 1024) {
		return 31;
	}
	input = data;
	input_size = size;
	input_position = 0;
	deadline = emscripten_get_now() + timeout;
	AVFormatContext *format = avformat_alloc_context();
	AVIOContext *io = avio_alloc_context(static_cast<uint8_t *>(av_malloc(65536)), 65536, 0, nullptr, read_input, nullptr, seek_input);
	AVCodecContext *codec = nullptr;
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	SwrContext *resampler = nullptr;
	int error = 30;
	if (!format || !io || !io->buffer || !packet || !frame) {
		goto cleanup;
	}
	format->pb = io;
	format->flags |= AVFMT_FLAG_CUSTOM_IO;
	format->interrupt_callback = { interrupt, nullptr };
	format->io_open = deny_secondary;
	// No filesystem/network protocols or secondary requests in this module.
	if (avformat_open_input(&format, nullptr, nullptr, nullptr) < 0 || avformat_find_stream_info(format, nullptr) < 0) {
		goto cleanup;
	}
	if (const AVDictionaryEntry *entry = av_dict_get(format->metadata, "title", nullptr, 0)) {
		title = std::string(entry->value).substr(0, 1024);
	}
	{
		const AVCodec *decoder = nullptr;
		int stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
		if (stream < 0 || !decoder) {
			goto cleanup;
		}
		codec = avcodec_alloc_context3(decoder);
		if (!codec || avcodec_parameters_to_context(codec, format->streams[stream]->codecpar) < 0) {
			goto cleanup;
		}
		codec->thread_count = 1;
		if (avcodec_open2(codec, decoder, nullptr) < 0) {
			goto cleanup;
		}
		bool draining = false, pending = false, eof = false;
		while (!eof) {
			if (interrupt(nullptr)) {
				error = 24;
				goto cleanup;
			}
			int received = avcodec_receive_frame(codec, frame);
			if (received >= 0 || received == AVERROR_EOF) {
				bool flush = received == AVERROR_EOF;
				if (!resampler && !flush) {
					AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
					if (swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_FLT, rate, &frame->ch_layout, AVSampleFormat(frame->format), frame->sample_rate, 0, nullptr) < 0 || !resampler || swr_init(resampler) < 0) {
						goto cleanup;
					}
				}
				if (resampler) {
					int capacity = swr_get_out_samples(resampler, flush ? 0 : frame->nb_samples);
					if (capacity < 0 || capacity > 262144) {
						goto cleanup;
					}
					std::vector<float> samples(size_t(capacity) * 2);
					uint8_t *dest[] = { reinterpret_cast<uint8_t *>(samples.data()) };
					int count = swr_convert(resampler, dest, capacity, flush ? nullptr : const_cast<const uint8_t **>(frame->extended_data), flush ? 0 : frame->nb_samples);
					if (count < 0) {
						goto cleanup;
					}
					if (size_t(count) * 4 > size_t(limit) - output.size()) {
						error = 6;
						goto cleanup;
					}
					for (int i = 0; i < count * 2; ++i) {
						if (!std::isfinite(samples[i])) {
							goto cleanup;
						}
						int16_t value = int16_t(std::clamp(std::round(samples[i] * 32767.0f), -32768.0f, 32767.0f));
						output.push_back(uint8_t(value));
						output.push_back(uint8_t(uint16_t(value) >> 8));
					}
					if (flush && count) {
						continue;
					}
				}
				av_frame_unref(frame);
				if (flush) {
					eof = true;
				}
				continue;
			}
			if (received != AVERROR(EAGAIN) || draining) {
				goto cleanup;
			}
			if (!pending) {
				int read;
				do {
					av_packet_unref(packet);
					read = av_read_frame(format, packet);
				} while (read >= 0 && packet->stream_index != stream && !interrupt(nullptr));
				if (read < 0 && read != AVERROR_EOF) {
					goto cleanup;
				}
				if (read == AVERROR_EOF) {
					draining = true;
				}
			}
			int sent = avcodec_send_packet(codec, draining ? nullptr : packet);
			pending = sent == AVERROR(EAGAIN);
			if (sent < 0 && !pending) {
				goto cleanup;
			}
		}
	}
	error = output.empty() ? 30 : 0;
cleanup:
	if (codec) {
		avcodec_free_context(&codec);
	}
	if (format) {
		avformat_close_input(&format);
	}
	if (io) {
		av_freep(&io->buffer);
		avio_context_free(&io);
	}
	av_packet_free(&packet);
	av_frame_free(&frame);
	swr_free(&resampler);
	input = nullptr;
	input_size = input_position = 0;
	if (error) {
		output.clear();
	}
	return error;
}
extern "C" EMSCRIPTEN_KEEPALIVE const uint8_t *godot_pcm_data() {
	return output.data();
}
extern "C" EMSCRIPTEN_KEEPALIVE int godot_pcm_size() {
	return int(output.size());
}
extern "C" EMSCRIPTEN_KEEPALIVE const char *godot_pcm_title() {
	return title.c_str();
}
