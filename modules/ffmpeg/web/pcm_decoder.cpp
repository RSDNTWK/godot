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
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
uint8_t *output = nullptr;
size_t output_size = 0, output_capacity = 0;
char title[1025] = {};
const char *failure_message = "Unsupported or invalid finite audio.";
void clear_output() {
	av_freep(&output);
	output_size = output_capacity = 0;
}
bool reserve_output(size_t required, size_t limit) {
	if (required <= output_capacity) {
		return true;
	}
	const size_t capacity = std::min(limit, std::max(required, std::max(size_t(4096), output_capacity * 2)));
	void *resized = av_realloc(output, capacity);
	if (!resized) {
		return false;
	}
	output = static_cast<uint8_t *>(resized);
	output_capacity = capacity;
	return true;
}
int decode_error(int error) {
	if (emscripten_get_now() >= deadline) {
		return 24;
	}
	return error == AVERROR(ENOMEM) ? 6 : 30;
}
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
	clear_output();
	title[0] = '\0';
	failure_message = "Invalid finite PCM preparation parameters.";
	if (!data || size <= 0 || size > 64 * 1024 * 1024 || rate < 8000 || rate > 192000 || limit < 4 || limit > 128 * 1024 * 1024) {
		return 31;
	}
	input = data;
	input_size = size;
	input_position = 0;
	deadline = emscripten_get_now() + timeout;
	AVFormatContext *format = avformat_alloc_context();
	uint8_t *io_buffer = static_cast<uint8_t *>(av_malloc(65536));
	AVIOContext *io = io_buffer ? avio_alloc_context(io_buffer, 65536, 0, nullptr, read_input, nullptr, seek_input) : nullptr;
	if (io) {
		io_buffer = nullptr; // The context now owns the buffer, which FFmpeg may replace.
	}
	AVCodecContext *codec = nullptr;
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	SwrContext *resampler = nullptr;
	float *samples = nullptr;
	int sample_capacity = 0;
	int error = 6;
	failure_message = "Could not allocate PCM decoder state.";
	if (!format || !io || !io->buffer || !packet || !frame) {
		goto cleanup;
	}
	error = 30;
	format->pb = io;
	format->flags |= AVFMT_FLAG_CUSTOM_IO;
	format->interrupt_callback = { interrupt, nullptr };
	format->io_open = deny_secondary;
	// No filesystem/network protocols or secondary requests in this module.
	{
		failure_message = "Could not read the finite media header.";
		int status = avformat_open_input(&format, nullptr, nullptr, nullptr);
		if (status < 0) {
			error = decode_error(status);
			goto cleanup;
		}
		failure_message = "Could not inspect finite media streams.";
		status = avformat_find_stream_info(format, nullptr);
		if (status < 0) {
			error = decode_error(status);
			goto cleanup;
		}
	}
	if (const AVDictionaryEntry *entry = av_dict_get(format->metadata, "title", nullptr, 0)) {
		size_t length = 0;
		while (length < sizeof(title) - 1 && entry->value[length]) {
			title[length] = entry->value[length];
			length++;
		}
		title[length] = '\0';
	}
	{
		const AVCodec *decoder = nullptr;
		int stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
		failure_message = "No supported finite audio stream was found.";
		if (stream < 0 || !decoder) {
			error = stream < 0 ? decode_error(stream) : 30;
			goto cleanup;
		}
		codec = avcodec_alloc_context3(decoder);
		if (!codec) {
			error = 6;
			failure_message = "Could not allocate the audio decoder.";
			goto cleanup;
		}
		failure_message = "Could not configure the audio decoder.";
		int status = avcodec_parameters_to_context(codec, format->streams[stream]->codecpar);
		if (status < 0) {
			error = decode_error(status);
			goto cleanup;
		}
		codec->thread_count = 1;
		status = avcodec_open2(codec, decoder, nullptr);
		if (status < 0) {
			error = decode_error(status);
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
					failure_message = "Could not configure PCM resampling.";
					status = swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_FLT, rate, &frame->ch_layout, AVSampleFormat(frame->format), frame->sample_rate, 0, nullptr);
					if (status < 0 || !resampler) {
						error = status < 0 ? decode_error(status) : 6;
						goto cleanup;
					}
					status = swr_init(resampler);
					if (status < 0) {
						error = decode_error(status);
						goto cleanup;
					}
				}
				if (resampler) {
					int capacity = swr_get_out_samples(resampler, flush ? 0 : frame->nb_samples);
					if (capacity < 0 || capacity > 262144) {
						failure_message = "Invalid PCM resampling capacity.";
						goto cleanup;
					}
					if (capacity > sample_capacity) {
						void *resized = av_realloc(samples, size_t(capacity) * 2 * sizeof(float));
						if (!resized) {
							error = 6;
							failure_message = "Could not allocate the PCM conversion buffer.";
							goto cleanup;
						}
						samples = static_cast<float *>(resized);
						sample_capacity = capacity;
					}
					uint8_t *dest[] = { reinterpret_cast<uint8_t *>(samples) };
					int count = swr_convert(resampler, dest, capacity, flush ? nullptr : const_cast<const uint8_t **>(frame->extended_data), flush ? 0 : frame->nb_samples);
					if (count < 0) {
						error = decode_error(count);
						failure_message = "Could not convert decoded audio to PCM.";
						goto cleanup;
					}
					if (size_t(count) * 4 > size_t(limit) - output_size) {
						error = 6;
						failure_message = "Decoded audio exceeds the PCM byte limit.";
						goto cleanup;
					}
					if (!reserve_output(output_size + size_t(count) * 4, size_t(limit))) {
						error = 6;
						failure_message = "Could not allocate the bounded PCM output.";
						goto cleanup;
					}
					for (int i = 0; i < count * 2; ++i) {
						if (!std::isfinite(samples[i])) {
							failure_message = "Decoded audio contains a non-finite sample.";
							goto cleanup;
						}
						int16_t value = int16_t(std::clamp(std::round(samples[i] * 32767.0f), -32768.0f, 32767.0f));
						output[output_size++] = uint8_t(value);
						output[output_size++] = uint8_t(uint16_t(value) >> 8);
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
				error = decode_error(received);
				failure_message = "Could not receive a decoded audio frame.";
				goto cleanup;
			}
			if (!pending) {
				int read;
				do {
					av_packet_unref(packet);
					read = av_read_frame(format, packet);
				} while (read >= 0 && packet->stream_index != stream && !interrupt(nullptr));
				if (read < 0 && read != AVERROR_EOF) {
					error = decode_error(read);
					failure_message = "Could not read a finite audio packet.";
					goto cleanup;
				}
				if (read == AVERROR_EOF) {
					draining = true;
				}
			}
			int sent = avcodec_send_packet(codec, draining ? nullptr : packet);
			pending = sent == AVERROR(EAGAIN);
			if (sent < 0 && !pending) {
				error = decode_error(sent);
				failure_message = "Could not submit an audio packet to the decoder.";
				goto cleanup;
			}
		}
	}
	error = output_size ? 0 : 30;
	if (error) {
		failure_message = "The finite audio stream produced no PCM frames.";
	}
cleanup:
	av_freep(&samples);
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
	av_freep(&io_buffer);
	av_packet_free(&packet);
	av_frame_free(&frame);
	swr_free(&resampler);
	input = nullptr;
	input_size = input_position = 0;
	if (error) {
		clear_output();
		if (error == 24) {
			failure_message = "PCM preparation timed out.";
		}
	}
	return error;
}
extern "C" EMSCRIPTEN_KEEPALIVE const uint8_t *godot_pcm_data() {
	return output;
}
extern "C" EMSCRIPTEN_KEEPALIVE int godot_pcm_size() {
	return int(output_size);
}
extern "C" EMSCRIPTEN_KEEPALIVE const char *godot_pcm_title() {
	return title;
}
extern "C" EMSCRIPTEN_KEEPALIVE const char *godot_pcm_error_message() {
	return failure_message;
}
