/**************************************************************************/
/*  youtube_hls.cpp                                                  */
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

#include "youtube_hls.h"

#include "core/math/math_funcs.h"

#include "modules/media_stream/media_http.h"

namespace {
String resolve_url(const String &p_base, const String &p_target) {
	if (p_target.begins_with("https://")) {
		return p_target;
	}
	if (p_target.begins_with("//")) {
		return "https:" + p_target;
	}
	// Detect a scheme only before the path/query. Signed query values may
	// themselves contain :// and must remain byte-for-byte unchanged.
	const int colon = p_target.find_char(':');
	const int slash = p_target.find_char('/');
	const int query = p_target.find_char('?');
	if (colon >= 0 && (slash < 0 || colon < slash) && (query < 0 || colon < query)) {
		return String();
	}
	String scheme, host, path, fragment;
	int port;
	if (p_base.parse_url(scheme, host, port, path, fragment) != OK) {
		return String();
	}
	const String origin = "https://" + (host.contains(":") ? "[" + host + "]" : host) + (port ? ":" + itos(port) : String());
	if (p_target.begins_with("/")) {
		return origin + p_target;
	}
	const String base_path = path.get_slice("?", 0);
	if (p_target.begins_with("?")) {
		return origin + base_path + p_target;
	}
	return origin + base_path.substr(0, base_path.rfind_char('/') + 1) + p_target;
}

Error attributes(const String &p_text, Dictionary &r_attributes) {
	int at = 0;
	while (at < p_text.length()) {
		while (at < p_text.length() && (p_text[at] == ' ' || p_text[at] == '\t')) {
			at++;
		}
		const int key_start = at;
		while (at < p_text.length() && ((p_text[at] >= 'A' && p_text[at] <= 'Z') || (p_text[at] >= '0' && p_text[at] <= '9') || p_text[at] == '-')) {
			at++;
		}
		if (at == key_start || at >= p_text.length() || p_text[at] != '=') {
			return ERR_PARSE_ERROR;
		}
		const String key = p_text.substr(key_start, at - key_start);
		if (r_attributes.has(key)) {
			return ERR_PARSE_ERROR;
		}
		at++;
		String value;
		if (at < p_text.length() && p_text[at] == '"') {
			const int value_start = ++at;
			while (at < p_text.length() && p_text[at] != '"') {
				at++;
			}
			if (at >= p_text.length()) {
				return ERR_PARSE_ERROR;
			}
			value = p_text.substr(value_start, at - value_start);
			at++;
		} else {
			const int value_start = at;
			while (at < p_text.length() && p_text[at] != ',') {
				at++;
			}
			value = p_text.substr(value_start, at - value_start).strip_edges();
			if (value.is_empty() || value.contains("\"")) {
				return ERR_PARSE_ERROR;
			}
		}
		r_attributes[key] = value;
		while (at < p_text.length() && (p_text[at] == ' ' || p_text[at] == '\t')) {
			at++;
		}
		if (at < p_text.length()) {
			if (p_text[at++] != ',' || at == p_text.length()) {
				return ERR_PARSE_ERROR;
			}
		}
	}
	return r_attributes.is_empty() ? ERR_PARSE_ERROR : OK;
}

int64_t unsigned_number(const String &p_value, int64_t p_max) {
	if (p_value.is_empty()) {
		return -1;
	}
	int64_t value = 0;
	for (int i = 0; i < p_value.length(); i++) {
		const int digit = p_value[i] - '0';
		if (digit < 0 || digit > 9 || value > (p_max - digit) / 10) {
			return -1;
		}
		value = value * 10 + digit;
	}
	return value;
}

Dictionary format(const String &p_url, bool p_audio, bool p_video) {
	Dictionary selected;
	selected["url"] = p_url;
	selected["protocol"] = "hls";
	selected["has_audio"] = p_audio;
	selected["has_video"] = p_video;
	return selected;
}

bool compatible_codecs(const String &p_codecs, bool &r_audio, bool &r_video) {
	r_audio = false;
	r_video = false;
	if (p_codecs.is_empty()) {
		return false;
	}
	bool compatible = true;
	for (String codec : p_codecs.split(",", false)) {
		codec = codec.strip_edges().to_lower();
		// These codecs are present in all validated native SDKs. Do not hand
		// unknown/HDR alternatives to a decoder merely because bandwidth is low.
		if (codec.begins_with("avc1.") || codec.begins_with("avc3.")) {
			r_video = true;
		} else if (codec == "mp4a.40.2" || codec == "mp4a.40.5" || codec == "mp4a.40.29") {
			r_audio = true;
		} else {
			compatible = false;
		}
	}
	return compatible && (r_audio || r_video);
}

struct Traversal {
	YouTubeHLS::ManifestReader reader;
	void *userdata;
	int requests = 0;
	int max_height = 1080;

	Error walk(const String &p_url, bool p_audio_only, int p_depth, const Dictionary &p_inherited, Vector<String> p_ancestors, Dictionary &r_selected, Dictionary &r_audio) {
		if (p_depth >= YouTubeHLS::MAX_DEPTH || requests >= YouTubeHLS::MAX_REQUESTS) {
			return ERR_PARAMETER_RANGE_ERROR;
		}
		if (!MediaHTTP::valid_url(p_url)) {
			return ERR_UNAUTHORIZED;
		}
		if (p_ancestors.has(p_url)) {
			return ERR_CYCLIC_LINK;
		}
		p_ancestors.push_back(p_url);
		String body;
		String effective;
		requests++;
		Error error = reader(userdata, p_url, body, effective);
		if (error != OK) {
			return error;
		}
		if (!MediaHTTP::valid_url(effective)) {
			return ERR_UNAUTHORIZED;
		}
		if (effective != p_url) {
			if (p_ancestors.has(effective)) {
				return ERR_CYCLIC_LINK;
			}
			p_ancestors.push_back(effective);
		}
		Dictionary selected;
		Dictionary audio;
		bool media = false;
		error = YouTubeHLS::select_manifest(body, effective, p_audio_only, selected, audio, media, max_height);
		if (error != OK) {
			return error;
		}
		if (media) {
			r_selected = p_inherited.is_empty() ? selected : p_inherited.duplicate();
			r_selected["url"] = effective;
			return OK;
		}
		error = walk(selected["url"], p_audio_only, p_depth + 1, selected, p_ancestors, r_selected, r_audio);
		if (error != OK || p_audio_only || audio.is_empty()) {
			return error;
		}
		Dictionary unused_audio;
		error = walk(audio["url"], true, p_depth + 1, audio, p_ancestors, r_audio, unused_audio);
		if (error == OK) {
			r_selected["has_audio"] = false;
		}
		return error;
	}
};
} // namespace

Error YouTubeHLS::select_manifest(const String &p_body, const String &p_base, bool p_audio_only, Dictionary &r_selected, Dictionary &r_audio, bool &r_media, int p_max_height) {
	r_selected.clear();
	r_audio.clear();
	r_media = false;
	if (p_body.length() > MAX_BODY || p_body.utf8().length() > MAX_BODY) {
		return ERR_PARAMETER_RANGE_ERROR;
	}
	if (!MediaHTTP::valid_url(p_base)) {
		return ERR_UNAUTHORIZED;
	}
	Vector<Dictionary> variants;
	Vector<Dictionary> renditions;
	Dictionary pending;
	bool header = false;
	bool master = false;
	bool media = false;
	for (String line : p_body.split("\n")) {
		line = line.strip_edges();
		if (!header) {
			line = line.trim_prefix(String::chr(0xfeff));
			if (line.is_empty()) {
				continue;
			}
			if (line != "#EXTM3U") {
				return ERR_PARSE_ERROR;
			}
			header = true;
			continue;
		}
		if (line.begins_with("#EXT-X-STREAM-INF:")) {
			if (!pending.is_empty()) {
				return ERR_PARSE_ERROR;
			}
			master = true;
			if (variants.size() >= MAX_VARIANTS) {
				return ERR_PARAMETER_RANGE_ERROR;
			}
			Error error = attributes(line.trim_prefix("#EXT-X-STREAM-INF:"), pending);
			if (error != OK) {
				return error;
			}
		} else if (line.begins_with("#EXT-X-MEDIA:")) {
			master = true;
			if (renditions.size() >= MAX_VARIANTS) {
				return ERR_PARAMETER_RANGE_ERROR;
			}
			Dictionary rendition;
			Error error = attributes(line.trim_prefix("#EXT-X-MEDIA:"), rendition);
			if (error != OK) {
				return error;
			}
			renditions.push_back(rendition);
		} else if (line.begins_with("#EXTINF:") || line.begins_with("#EXT-X-TARGETDURATION:")) {
			media = true;
		} else if (!line.is_empty() && !line.begins_with("#") && !pending.is_empty()) {
			pending["url"] = resolve_url(p_base, line);
			variants.push_back(pending);
			// Dictionary copies share storage; clear() would erase the variant
			// just appended above. Give the next record independent storage.
			pending = Dictionary();
		}
	}
	if (!header || !pending.is_empty() || (master && media)) {
		return ERR_PARSE_ERROR;
	}
	if (media && !master) {
		r_media = true;
		r_selected = format(p_base, true, !p_audio_only);
		return OK;
	}
	int64_t best_bandwidth = INT64_MAX;
	int64_t best_video_rank = -1;
	int best_audio_priority = INT32_MAX;
	for (const Dictionary &variant : variants) {
		const String url = variant.get("url", String());
		bool has_audio;
		bool has_video;
		const bool compatible = compatible_codecs(variant.get("CODECS", String()), has_audio, has_video);
		if (!MediaHTTP::valid_url(url) || !has_audio || (!p_audio_only && (!has_video || !compatible))) {
			continue;
		}
		const int64_t bandwidth = unsigned_number(variant.get("AVERAGE-BANDWIDTH", variant.get("BANDWIDTH", String())), 1000000000);
		if (bandwidth <= 0) {
			continue;
		}
		int64_t width = 0;
		int64_t height = 0;
		double fps = 0.0;
		if (has_video) {
			const String resolution = variant.get("RESOLUTION", String());
			width = unsigned_number(resolution.get_slice("x", 0), 16384);
			height = unsigned_number(resolution.get_slice("x", 1), 16384);
			if (width <= 0 || height <= 0 || width * height > 16777216 || resolution.get_slice_count("x") != 2) {
				continue;
			}
			const String frame_rate = variant.get("FRAME-RATE", String());
			if (!frame_rate.is_empty()) {
				if (!frame_rate.is_valid_float()) {
					continue;
				}
				fps = frame_rate.to_float();
				if (!Math::is_finite(fps) || fps <= 0.0) {
					continue;
				}
			}
			if (!p_audio_only && (width > 1920 || height > MIN(p_max_height, 1080) || fps > 60.0)) {
				continue;
			}
		}
		Dictionary audio;
		const String group = variant.get("AUDIO", String());
		if (!group.is_empty()) {
			int best_preference = -1;
			bool group_found = false;
			for (const Dictionary &rendition : renditions) {
				if (String(rendition.get("TYPE", "")) != "AUDIO" || String(rendition.get("GROUP-ID", "")) != group) {
					continue;
				}
				const String uri = rendition.get("URI", String());
				const String audio_url = uri.is_empty() ? String() : resolve_url(p_base, uri);
				if (!uri.is_empty() && !MediaHTTP::valid_url(audio_url)) {
					continue;
				}
				const int preference = String(rendition.get("DEFAULT", "")) == "YES" ? 2 : String(rendition.get("AUTOSELECT", "")) == "YES" ? 1
																																			: 0;
				if (preference > best_preference) {
					best_preference = preference;
					group_found = true;
					audio = uri.is_empty() ? Dictionary() : format(audio_url, true, false);
				}
			}
			if (!group_found) {
				continue;
			}
		}
		// An audio-only request can use a declared AAC rendition without
		// decoding the associated unsupported video codec. A muxed unknown
		// codec or an undeclared audio codec still fails explicitly.
		if (!compatible && (!p_audio_only || audio.is_empty())) {
			continue;
		}
		const int priority = p_audio_only ? (!audio.is_empty() ? 0 : !has_video ? 1
																				: 2)
										  : 0;
		const int64_t video_rank = p_audio_only ? 0 : height * INT64_C(100000000000000) + int64_t(fps * 1000.0) * INT64_C(1000000001) + bandwidth;
		if (p_audio_only ? (priority > best_audio_priority || (priority == best_audio_priority && bandwidth >= best_bandwidth)) : video_rank <= best_video_rank) {
			continue;
		}
		best_video_rank = video_rank;
		best_audio_priority = priority;
		best_bandwidth = bandwidth;
		r_selected = p_audio_only && !audio.is_empty() ? audio : format(url, audio.is_empty(), has_video);
		r_selected["bitrate"] = p_audio_only && !audio.is_empty() ? int64_t(0) : bandwidth;
		r_selected["width"] = p_audio_only ? int64_t(0) : width;
		r_selected["height"] = p_audio_only ? int64_t(0) : height;
		r_selected["fps"] = p_audio_only ? 0.0 : fps;
		r_audio = p_audio_only ? Dictionary() : audio;
	}
	return r_selected.is_empty() ? ERR_UNAVAILABLE : OK;
}

Error YouTubeHLS::resolve(const String &p_url, bool p_audio_only, ManifestReader p_reader, void *p_userdata, Dictionary &r_selected, Dictionary &r_audio, int *r_requests, int p_max_height) {
	r_selected.clear();
	r_audio.clear();
	if (!p_reader) {
		return ERR_INVALID_PARAMETER;
	}
	Traversal state{ p_reader, p_userdata };
	state.max_height = p_max_height;
	Error error = state.walk(p_url, p_audio_only, 0, Dictionary(), Vector<String>(), r_selected, r_audio);
	if (r_requests) {
		*r_requests = state.requests;
	}
	if (error != OK) {
		r_selected.clear();
		r_audio.clear();
	}
	return error;
}
