/**************************************************************************/
/*  youtube_parser.cpp                                                  */
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

#include "youtube_parser.h"

#include "core/io/json.h"
#include "core/math/math_funcs.h"

namespace {
bool valid_id(const String &p_id) {
	if (p_id.length() != 11) {
		return false;
	}
	for (int i = 0; i < 11; i++) {
		char32_t c = p_id[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
			return false;
		}
	}
	return true;
}

String json_string_after(const String &p_text, const String &p_marker) {
	int start = p_text.find(p_marker);
	if (start < 0) {
		return String();
	}
	start = p_text.find_char('"', start + p_marker.length());
	if (start < 0) {
		return String();
	}
	bool escaped = false;
	for (int i = start + 1; i < p_text.length(); i++) {
		if (!escaped && p_text[i] == '"') {
			JSON parser;
			if (parser.parse(p_text.substr(start, i - start + 1)) == OK && parser.get_data().get_type() == Variant::STRING) {
				return parser.get_data();
			}
			break;
		}
		escaped = !escaped && p_text[i] == '\\';
	}
	return String();
}
} // namespace

Dictionary YouTubeParser::query(const String &p_query) {
	Dictionary result;
	for (const String &pair : p_query.split("&", false)) {
		int equals = pair.find_char('=');
		String key = (equals < 0 ? pair : pair.substr(0, equals)).replace("+", " ").uri_decode();
		if (!result.has(key)) {
			result[key] = equals < 0 ? String() : pair.substr(equals + 1).replace("+", " ").uri_decode();
		}
	}
	return result;
}

bool YouTubeParser::https_url(const String &p_url) {
	if (p_url.length() > 32768 || !p_url.begins_with("https://") || p_url.contains("@") || p_url.contains("\\")) {
		return false;
	}
	for (int i = 0; i < p_url.length(); i++) {
		if (p_url[i] <= 32 || p_url[i] == 127) {
			return false;
		}
	}
	String scheme, host, path, fragment;
	int port;
	return p_url.parse_url(scheme, host, port, path, fragment) == OK && !host.is_empty() && (port == 0 || port == 443) && fragment.is_empty();
}

String YouTubeParser::video_id(const String &p_url) {
	if (valid_id(p_url)) {
		return p_url;
	}
	// Accept a fragment on user-facing watch links; it is never sent upstream.
	String url = p_url.get_slice("#", 0);
	if (!https_url(url)) {
		return String();
	}
	String scheme, host, path, fragment;
	int port;
	url.parse_url(scheme, host, port, path, fragment);
	String id;
	if (host == "youtu.be" || host == "www.youtu.be") {
		id = path.get_slice("?", 0).trim_prefix("/");
	} else if (host == "youtube.com" || host == "www.youtube.com" || host == "m.youtube.com" || host == "music.youtube.com" || host == "www.youtube-nocookie.com") {
		String base_path = path.get_slice("?", 0);
		if (base_path == "/watch") {
			id = query_value(url, "v");
		} else if (base_path.begins_with("/shorts/") || base_path.begins_with("/live/") || base_path.begins_with("/embed/")) {
			id = base_path.get_slice("/", 2);
		}
	}
	return valid_id(id) ? id : String();
}

String YouTubeParser::query_value(const String &p_url, const String &p_name) {
	int start = p_url.find_char('?');
	return start < 0 ? String() : String(query(p_url.substr(start + 1)).get(p_name, String()));
}

String YouTubeParser::set_query_value(const String &p_url, const String &p_name, const String &p_value) {
	int start = p_url.find_char('?');
	String base = start < 0 ? p_url : p_url.substr(0, start);
	String updated;
	if (start >= 0) {
		for (const String &pair : p_url.substr(start + 1).split("&", false)) {
			if (pair.get_slice("=", 0).replace("+", " ").uri_decode() != p_name) {
				updated += pair + "&";
			}
		}
	}
	return base + "?" + updated + p_name.uri_encode() + "=" + p_value.uri_encode();
}

Dictionary YouTubeParser::object_after(const String &p_text, const String &p_marker, int p_from, int *r_end) {
	int marker = p_text.find(p_marker, p_from);
	if (r_end) {
		*r_end = -1;
	}
	if (marker < 0) {
		return Dictionary();
	}
	int start = marker + p_marker.length();
	while (start < p_text.length() && (p_text[start] == ' ' || p_text[start] == '\n' || p_text[start] == '\r' || p_text[start] == '\t')) {
		start++;
	}
	if (start == p_text.length() || p_text[start] != '{') {
		return Dictionary();
	}
	int depth = 0;
	bool quoted = false;
	bool escaped = false;
	for (int i = start; i < p_text.length(); i++) {
		char32_t c = p_text[i];
		if (quoted) {
			if (!escaped && c == '"') {
				quoted = false;
			}
			escaped = !escaped && c == '\\';
			continue;
		}
		if (c == '"') {
			quoted = true;
		} else if (c == '{') {
			depth++;
		} else if (c == '}' && --depth == 0) {
			if (r_end) {
				*r_end = i + 1;
			}
			JSON parser;
			if (parser.parse(p_text.substr(start, i - start + 1)) == OK && parser.get_data().get_type() == Variant::DICTIONARY) {
				return parser.get_data();
			}
			return Dictionary();
		}
	}
	return Dictionary();
}

Dictionary YouTubeParser::configuration(const String &p_webpage) {
	Dictionary result;
	const String marker = "ytcfg.set(";
	int offset = 0;
	for (int i = 0; i < 64; i++) {
		int start = p_webpage.find(marker, offset);
		if (start < 0) {
			break;
		}
		// The bootstrap also uses set(name, value). A non-object call must not
		// hide the actual JSON configuration records that follow it.
		offset = start + marker.length();
		int end = -1;
		Dictionary part = object_after(p_webpage, marker, start, &end);
		if (end >= 0) {
			offset = end;
		}
		result.merge(part, true);
	}
	return result;
}

String YouTubeParser::player_url(const String &p_webpage, const Dictionary &p_config) {
	String url = p_config.get("PLAYER_JS_URL", String());
	if (url.is_empty()) {
		url = json_string_after(p_webpage, "\"jsUrl\":");
	}
	if (url.is_empty()) {
		url = json_string_after(p_webpage, "\"js\":");
	}
	if (url.begins_with("/s/player/")) {
		url = "https://www.youtube.com" + url;
	}
	return https_url(url) && url.begins_with("https://www.youtube.com/s/player/") ? url : String();
}

Array YouTubeParser::formats(const Dictionary &p_player) {
	Dictionary streaming = p_player.get("streamingData", Dictionary());
	Dictionary details = p_player.get("videoDetails", Dictionary());
	const String live_hls = streaming.get("hlsManifestUrl", String());
	if (bool(details.get("isLive", false))) {
		// Live adaptive URLs may require DASH segment enumeration, which the
		// native player does not implement. Use the advertised HLS transport,
		// without solving challenges for discarded adaptive candidates.
		Array live_formats;
		if (!https_url(live_hls)) {
			return live_formats;
		}
		Dictionary format;
		format["url"] = live_hls;
		format["protocol"] = "hls";
		format["has_audio"] = true;
		format["has_video"] = true;
		format["signature"] = String();
		format["signature_parameter"] = String();
		format["n"] = String();
		live_formats.append(format);
		return live_formats;
	}
	Array raw = Array(streaming.get("formats", Array())).duplicate();
	raw.append_array(streaming.get("adaptiveFormats", Array()));
	Array result;
	for (int i = 0; i < MIN(raw.size(), 512); i++) {
		if (raw[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary data = raw[i];
		if (data.has("drmFamilies") || data.has("drmTrackType")) {
			continue;
		}
		String mime = data.get("mimeType", String());
		// The media wrapper decodes an elementary audio stream, not an IAMF
		// stream-group presentation. Prefer ordinary AAC/Opus alternatives.
		if (mime.to_lower().contains("iamf.")) {
			continue;
		}
		String url = data.get("url", String());
		Dictionary cipher = query(data.get("signatureCipher", data.get("cipher", String())));
		if (url.is_empty()) {
			url = cipher.get("url", String());
		}
		if (!https_url(url)) {
			continue;
		}
		Dictionary format;
		format["url"] = url;
		format["protocol"] = "https";
		format["itag"] = data.get("itag", 0);
		format["mime_type"] = mime;
		format["bitrate"] = data.get("bitrate", 0);
		format["width"] = data.get("width", 0);
		format["height"] = data.get("height", 0);
		format["fps"] = data.get("fps", 0);
		format["has_audio"] = mime.begins_with("audio/") || data.has("audioQuality") || data.has("audioChannels");
		format["has_video"] = mime.begins_with("video/");
		format["signature"] = cipher.get("s", String());
		format["signature_parameter"] = cipher.get("sp", "signature");
		format["n"] = query_value(url, "n");
		result.append(format);
	}
	return result;
}

Dictionary YouTubeParser::select_format(const Array &p_formats, bool p_audio_only, int p_max_height) {
	Dictionary best;
	int64_t best_rank = -1;
	for (int i = 0; i < p_formats.size(); i++) {
		Dictionary format = p_formats[i];
		bool audio = format.get("has_audio", false);
		bool video = format.get("has_video", false);
		if ((p_audio_only && !audio) || (!p_audio_only && !video)) {
			continue;
		}
		// Match the remote decoder's pixel allocation limit when selecting a
		// direct track. Otherwise an 8K choice could hide usable lower resolutions.
		int64_t width = format.get("width", 0);
		int64_t source_height = format.get("height", 0);
		const double fps = format.get("fps", 0.0);
		// Prefer the best declared rendition within the 1080p60 ceiling.
		// Missing frame-rate metadata remains usable, ranked below known rates.
		if (!p_audio_only && (width < 0 || source_height < 0 || width > 1920 || source_height > MIN(p_max_height, 1080) || !Math::is_finite(fps) || fps < 0.0 || fps > 60.0)) {
			continue;
		}
		int64_t bitrate = CLAMP(int64_t(format.get("bitrate", 0)), int64_t(0), int64_t(1000000000));
		int64_t height = CLAMP(int64_t(format.get("height", 0)), int64_t(0), int64_t(16384));
		// Separate strides order resolution, then frame rate, then bitrate.
		int64_t rank = p_audio_only ? ((!video ? int64_t(1) << 50 : 0) + bitrate) : height * INT64_C(100000000000000) + int64_t(fps * 1000.0) * INT64_C(1000000001) + bitrate;
		if (rank > best_rank) {
			best = format;
			best_rank = rank;
		}
	}
	return best;
}
