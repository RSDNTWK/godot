/**************************************************************************/
/*  ffmpeg_media_metadata.h                                              */
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

#include "core/math/math_funcs.h"
#include "core/variant/dictionary.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace FFmpegMediaMetadata {
inline String title(const String &p_title) {
	String result;
	for (int i = 0; i < p_title.length() && result.length() < 256; i++) {
		char32_t c = p_title[i];
		if (c == '\r' || c == '\n' || c == '\t') {
			result += ' ';
		} else if (c >= 32 && c != 127) {
			result += c;
		}
	}
	return result.strip_edges();
}

inline Dictionary source(const String &p_source) {
	String scheme, host, path, fragment;
	int port = 0;
	String fallback = p_source.get_file();
	if (p_source.parse_url(scheme, host, port, path, fragment) == OK) {
		fallback = path.get_slice("?", 0).get_file().uri_decode();
		if (fallback.is_empty()) {
			fallback = host;
		}
	}
	Dictionary metadata;
	metadata["title"] = title(fallback);
	metadata["title_source"] = "filename";
	metadata["source"] = p_source;
	metadata["source_type"] = "direct";
	metadata["duration"] = 0.0;
	metadata["is_live"] = false;
	metadata["opened"] = false;
	return metadata;
}

inline Dictionary container(AVFormatContext *p_format, int p_stream, const String &p_source, double p_duration, bool p_live) {
	Dictionary metadata = source(p_source);
	AVDictionaryEntry *entry = p_format ? av_dict_get(p_format->metadata, "title", nullptr, 0) : nullptr;
	if (!entry && p_format && p_stream >= 0 && p_stream < int(p_format->nb_streams)) {
		entry = av_dict_get(p_format->streams[p_stream]->metadata, "title", nullptr, 0);
	}
	if (entry && entry->value) {
		int bytes = 0;
		while (bytes < 4096 && entry->value[bytes]) {
			bytes++;
		}
		// Avoid cutting a UTF-8 sequence at the bounded conversion limit.
		if (bytes == 4096) {
			while (bytes > 0 && (uint8_t(entry->value[bytes]) & 0xc0) == 0x80) {
				bytes--;
			}
		}
		String tagged = title(String::utf8(entry->value, bytes));
		if (!tagged.is_empty()) {
			metadata["title"] = tagged;
			metadata["title_source"] = "container";
		}
	}
	metadata["duration"] = Math::is_finite(p_duration) ? MAX(p_duration, 0.0) : 0.0;
	metadata["is_live"] = p_live;
	metadata["opened"] = true;
	if (p_format && p_stream >= 0 && p_stream < int(p_format->nb_streams) && p_format->streams[p_stream]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
		AVStream *stream = p_format->streams[p_stream];
		metadata["video_width"] = stream->codecpar->width;
		metadata["video_height"] = stream->codecpar->height;
		metadata["video_fps"] = stream->avg_frame_rate.den > 0 ? av_q2d(stream->avg_frame_rate) : 0.0;
		metadata["video_codec"] = avcodec_get_name(stream->codecpar->codec_id);
	}
	return metadata;
}

inline Dictionary opened(const Dictionary &p_source, const Dictionary &p_container) {
	Dictionary result = p_container.duplicate();
	result["source"] = p_source.get("source", String());
	result["source_type"] = p_source.get("source_type", "direct");
	if (String(result["source_type"]) == "youtube") {
		result["adaptive_quality"] = p_source.get("adaptive_quality", false);
		result["quality_limit_height"] = p_source.get("quality_limit_height", 1080);
		if (double(result.get("video_fps", 0.0)) <= 0.0) {
			result["video_fps"] = p_source.get("video_fps", 0.0);
		}
		result["title"] = p_source["title"];
		result["title_source"] = "youtube";
		result["is_live"] = p_source["is_live"];
		if (bool(result["is_live"]) || double(result.get("duration", 0.0)) <= 0) {
			result["duration"] = p_source["duration"];
		}
	} else if (String(result.get("title_source", "filename")) != "container") {
		result["title"] = p_source["title"];
	}
	result["opened"] = true;
	return result;
}
} // namespace FFmpegMediaMetadata
