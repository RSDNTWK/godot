/**************************************************************************/
/*  youtube_extractor.cpp                                               */
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

#include "youtube_extractor.h"

#include "youtube_challenges.h"
#include "youtube_hls.h"
#include "youtube_parser.h"

#include "core/io/json.h"
#include "core/os/os.h"

#include "modules/media_stream/media_http.h"

#include <cstring>

namespace {
constexpr int MAX_BODY = 16 * 1024 * 1024;
constexpr char WEB_AGENT[] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";

bool youtube_endpoint(const String &p_url) {
	if (!YouTubeParser::https_url(p_url)) {
		return false;
	}
	String scheme, host, path, fragment;
	int port;
	p_url.parse_url(scheme, host, port, path, fragment);
	return host == "www.youtube.com" || host == "youtube.com";
}

Dictionary parse_object(const String &p_json) {
	JSON parser;
	if (parser.parse(p_json) == OK && parser.get_data().get_type() == Variant::DICTIONARY) {
		return parser.get_data();
	}
	return Dictionary();
}

int signature_timestamp(const String &p_player) {
	for (const String &marker : { String("signatureTimestamp"), String("\"sts\"") }) {
		int start = p_player.find(marker);
		if (start < 0) {
			continue;
		}
		start += marker.length();
		if (start < p_player.length() && p_player[start] == '"') {
			start++;
		}
		while (start < p_player.length() && p_player[start] <= 32) {
			start++;
		}
		if (start >= p_player.length() || p_player[start] != ':') {
			continue;
		}
		start++;
		while (start < p_player.length() && p_player[start] <= 32) {
			start++;
		}
		int end = start;
		while (end < p_player.length() && end - start < 8 && p_player[end] >= '0' && p_player[end] <= '9') {
			end++;
		}
		if (end - start >= 5 && end - start <= 7) {
			return p_player.substr(start, end - start).to_int();
		}
	}
	return 0;
}

// Client fields and ordering follow the pinned yt-dlp YouTube extractor.
Dictionary player_client(int p_index, const Dictionary &p_config) {
	Dictionary client;
	if (p_index == 0) {
		client["clientName"] = "TVHTML5";
		client["clientVersion"] = "5.20260707";
		client["deviceMake"] = "Samsung";
		client["deviceModel"] = "UKS9800";
		client["osName"] = "Tizen";
		client["osVersion"] = "2.4.0";
		client["userAgent"] = "Mozilla/5.0 (SMART-TV; Linux; Tizen 2.4.0) AppleWebKit/538.1 (KHTML, like Gecko) Version/2.4.0 TV Safari/538.1";
	} else if (p_index == 1) {
		client["clientName"] = "VISIONOS";
		client["clientVersion"] = "1.02";
		client["deviceMake"] = "Apple";
		client["deviceModel"] = "RealityDevice17,1";
		client["osName"] = "visionOS";
		client["osVersion"] = "26.5.23O471";
		client["userAgent"] = "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";
	} else {
		Dictionary context = p_config.get("INNERTUBE_CONTEXT", Dictionary());
		client = context.get("client", Dictionary());
		if (client.is_empty()) {
			client["clientName"] = "WEB";
			client["clientVersion"] = "2.20260708.00.00";
		}
		client["userAgent"] = WEB_AGENT;
	}
	client["hl"] = "en";
	String visitor = p_config.get("VISITOR_DATA", String());
	if (visitor.is_empty()) {
		Dictionary context = p_config.get("INNERTUBE_CONTEXT", Dictionary());
		Dictionary web_client = context.get("client", Dictionary());
		visitor = web_client.get("visitorData", String());
	}
	if (!visitor.is_empty()) {
		client["visitorData"] = visitor;
	}
	return client;
}
} // namespace

Error YouTubeExtractor::resolve(const String &p_url, bool p_audio_only, Dictionary &r_result, int p_max_height) {
	max_height = CLAMP(p_max_height, 144, 1080);
	requested_id = YouTubeParser::video_id(p_url);
	if (requested_id.is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	audio_only = p_audio_only;
	hls_manifest_requests = 0;
	deadline = OS::get_singleton()->get_ticks_usec() + 90000000;
	Error error = _resolve();
	r_result = result;
	return error;
}

bool YouTubeExtractor::_interrupted() const {
	return cancelled.is_set() || OS::get_singleton()->get_ticks_usec() >= deadline;
}

Error YouTubeExtractor::_http(const String &p_url, const String &p_agent, String &r_body, const String &p_post, const Dictionary &p_headers) {
	if (!youtube_endpoint(p_url)) {
		return ERR_UNAUTHORIZED;
	}
	Dictionary headers = p_headers.duplicate();
	headers["User-Agent"] = p_agent;
	headers["Referer"] = "https://www.youtube.com/";
	headers["Origin"] = "https://www.youtube.com";
	MediaHTTP http(&cancelled);
	Error error = http.open(p_url, headers, 0, p_post);
	if (error != OK) {
		result["message"] = vformat("YouTube HTTPS request failed (HTTP %d, error %d).", http.get_response_status(), int(error));
		return error;
	}
	Vector<uint8_t> body;
	uint8_t buffer[16384];
	while (!_interrupted()) {
		int count = http.read(buffer, sizeof(buffer));
		if (count < 0) {
			return ERR_CONNECTION_ERROR;
		}
		if (count == 0) {
			r_body = String::utf8(reinterpret_cast<const char *>(body.ptr()), body.size());
			return OK;
		}
		if (count > MAX_BODY - body.size()) {
			return ERR_OUT_OF_MEMORY;
		}
		int old_size = body.size();
		error = body.resize(old_size + count);
		if (error != OK) {
			return error;
		}
		memcpy(body.ptrw() + old_size, buffer, count);
	}
	return cancelled.is_set() ? ERR_SKIP : ERR_TIMEOUT;
}

Error YouTubeExtractor::_hls_manifest(const String &p_url, const String &p_agent, String &r_body, String &r_effective_url) {
	if (_interrupted()) {
		return cancelled.is_set() ? ERR_SKIP : ERR_TIMEOUT;
	}
	if (hls_manifest_requests >= YouTubeHLS::MAX_REQUESTS) {
		return ERR_PARAMETER_RANGE_ERROR;
	}
	hls_manifest_requests++;
	Dictionary headers;
	headers["User-Agent"] = p_agent;
	headers["Referer"] = "https://www.youtube.com/";
	MediaHTTP http(&cancelled);
	Error error = http.open(p_url, headers);
	if (error != OK) {
		return error;
	}
	if (http.get_size() > YouTubeHLS::MAX_BODY) {
		return ERR_PARAMETER_RANGE_ERROR;
	}
	Vector<uint8_t> body;
	uint8_t buffer[16384];
	while (!_interrupted()) {
		const int count = http.read(buffer, sizeof(buffer));
		if (_interrupted()) {
			return cancelled.is_set() ? ERR_SKIP : ERR_TIMEOUT;
		}
		if (count < 0) {
			return ERR_CONNECTION_ERROR;
		}
		if (count == 0) {
			r_body = String::utf8(reinterpret_cast<const char *>(body.ptr()), body.size());
			r_effective_url = http.get_url();
			return OK;
		}
		if (count > YouTubeHLS::MAX_BODY - body.size()) {
			return ERR_PARAMETER_RANGE_ERROR;
		}
		const int old_size = body.size();
		error = body.resize(old_size + count);
		if (error != OK) {
			return error;
		}
		memcpy(body.ptrw() + old_size, buffer, count);
	}
	return cancelled.is_set() ? ERR_SKIP : ERR_TIMEOUT;
}

Error YouTubeExtractor::_prepare(const Dictionary &p_player, const String &p_player_js, const String &p_agent) {
	Dictionary playability = p_player.get("playabilityStatus", Dictionary());
	if (String(playability.get("status", String())) != "OK") {
		result["message"] = playability.get("reason", "This video is not available for public playback.");
		return ERR_UNAVAILABLE;
	}
	Dictionary details = p_player.get("videoDetails", Dictionary());
	if (String(details.get("videoId", String())) != requested_id) {
		result["message"] = "YouTube returned mismatched or missing video metadata.";
		return ERR_INVALID_DATA;
	}
	Array formats = YouTubeParser::formats(p_player);
	Array signatures;
	Array ns;
	for (int i = 0; i < formats.size(); i++) {
		Dictionary format = formats[i];
		String signature = format["signature"];
		String n = format["n"];
		if (!signature.is_empty() && !signatures.has(signature)) {
			signatures.append(signature);
		}
		if (!n.is_empty() && !ns.has(n)) {
			ns.append(n);
		}
	}
	Dictionary solved_signatures;
	Dictionary solved_ns;
	if (!signatures.is_empty() || !ns.is_empty()) {
		if (p_player_js.is_empty()) {
			result["message"] = "YouTube player JavaScript is required to resolve these streams.";
			return ERR_UNAVAILABLE;
		}
		Array requests;
		for (int type = 0; type < 2; type++) {
			Dictionary request;
			request["type"] = type == 0 ? "sig" : "n";
			request["challenges"] = type == 0 ? signatures : ns;
			requests.append(request);
		}
		Dictionary response;
		String message;
		Error error = YouTubeChallenges::solve(p_player_js, requests, cancelled, response, message);
		if (error != OK) {
			result["message"] = "YouTube challenge solver: " + message;
			return error;
		}
		Array responses = response.get("responses", Array());
		if (String(response.get("type", String())) != "result" || responses.size() != 2) {
			result["message"] = "The YouTube challenge solver returned an unsupported response.";
			return ERR_PARSE_ERROR;
		}
		Dictionary sig_response = responses[0];
		Dictionary n_response = responses[1];
		if ((!signatures.is_empty() && String(sig_response.get("type", String())) != "result") ||
				(!ns.is_empty() && String(n_response.get("type", String())) != "result")) {
			result["message"] = "The YouTube player uses an unsupported signature or n challenge.";
			return ERR_UNAVAILABLE;
		}
		solved_signatures = sig_response.get("data", Dictionary());
		solved_ns = n_response.get("data", Dictionary());
	}
	Array resolved;
	for (int i = 0; i < formats.size(); i++) {
		Dictionary format = formats[i];
		String url = format["url"];
		String signature = format["signature"];
		String n = format["n"];
		if (!signature.is_empty()) {
			String output = solved_signatures.get(signature, String());
			String parameter = format["signature_parameter"];
			if (output.is_empty() || parameter.is_empty() || parameter.length() > 64) {
				continue;
			}
			url = YouTubeParser::set_query_value(url, parameter, output);
		}
		if (!n.is_empty()) {
			String output = solved_ns.get(n, String());
			if (output.is_empty() || output.begins_with("enhanced_except_")) {
				continue;
			}
			url = YouTubeParser::set_query_value(url, "n", output);
		}
		format["url"] = url;
		format.erase("signature");
		format.erase("signature_parameter");
		format.erase("n");
		resolved.append(format);
	}
	Dictionary streaming = p_player.get("streamingData", Dictionary());
	String hls = streaming.get("hlsManifestUrl", String());
	if (!bool(details.get("isLive", false)) && YouTubeParser::https_url(hls)) {
		Dictionary format;
		format["url"] = hls;
		format["protocol"] = "hls";
		format["has_audio"] = true;
		format["has_video"] = true;
		resolved.append(format);
	}
	Dictionary selected = YouTubeParser::select_format(resolved, audio_only, max_height);
	Dictionary paired_audio;
	if (!audio_only && !selected.is_empty() && !bool(selected.get("has_audio", false))) {
		paired_audio = YouTubeParser::select_format(resolved, true);
		if (paired_audio.is_empty()) {
			selected.clear();
		}
	}
	if (!selected.is_empty() && bool(details.get("isLive", false))) {
		struct HLSReadContext {
			YouTubeExtractor *extractor;
			const String *agent;
		} read_context{ this, &p_agent };
		auto reader = [](void *p_userdata, const String &p_url, String &r_body, String &r_effective_url) -> Error {
			HLSReadContext *context = static_cast<HLSReadContext *>(p_userdata);
			return context->extractor->_hls_manifest(p_url, *context->agent, r_body, r_effective_url);
		};
		Dictionary leaf;
		Dictionary audio_leaf;
		Error error = YouTubeHLS::resolve(selected["url"], audio_only, reader, &read_context, leaf, audio_leaf, nullptr, max_height);
		if (error != OK) {
			result["message"] = vformat("YouTube live HLS rendition selection failed (error %d). Only bounded compatible H.264/AAC playlists are supported.", int(error));
			return error;
		}
		selected = leaf;
		paired_audio = audio_leaf;
		resolved.clear();
		resolved.append(selected);
		if (!paired_audio.is_empty()) {
			resolved.append(paired_audio);
		}
	}
	if (selected.is_empty()) {
		if (bool(details.get("isLive", false))) {
			result["message"] = "This live player response has no supported HLS feed; segmented live adaptive URLs require a different transport.";
			return ERR_UNAVAILABLE;
		}
		Array raw = streaming.get("formats", Array());
		Array adaptive = streaming.get("adaptiveFormats", Array());
		result["message"] = vformat("No supported playable stream was returned (%d raw, %d adaptive, %d HTTPS formats, %d resolved, SABR %s). Login, DRM and proof-of-origin token generation are not implemented.", raw.size(), adaptive.size(), formats.size(), resolved.size(), streaming.has("serverAbrStreamingUrl") ? "present" : "absent");
		return ERR_UNAVAILABLE;
	}
	Dictionary headers;
	headers["User-Agent"] = p_agent;
	headers["Referer"] = "https://www.youtube.com/";
	result.clear();
	result["id"] = requested_id;
	result["title"] = details.get("title", String());
	result["is_live"] = details.get("isLive", false);
	result["is_live_dvr_enabled"] = bool(details.get("isLive", false)) && bool(details.get("isLiveDvrEnabled", false));
	result["duration"] = String(details.get("lengthSeconds", "0")).to_float();
	result["formats"] = resolved;
	result["selected_format"] = selected;
	result["url"] = selected["url"];
	result["http_headers"] = headers;
	result["separate_audio"] = !paired_audio.is_empty();
	if (!paired_audio.is_empty()) {
		result["audio_url"] = paired_audio["url"];
		result["audio_format"] = paired_audio;
		result["audio_http_headers"] = headers;
	}
	result["upstream_version"] = "yt-dlp 2026.11.04.232659 / EJS 0.8.0";
	if (bool(details.get("isLive", false))) {
		result["hls_manifest_requests"] = hls_manifest_requests;
	}
	return OK;
}

Error YouTubeExtractor::_resolve() {
	String webpage;
	Error error = _http("https://www.youtube.com/watch?v=" + requested_id + "&hl=en", WEB_AGENT, webpage);
	if (error != OK) {
		result["message"] = "Could not fetch the YouTube watch page.";
		return error;
	}
	Dictionary config = YouTubeParser::configuration(webpage);
	String player_url = YouTubeParser::player_url(webpage, config);
	String player_js;
	if (!player_url.is_empty()) {
		_http(player_url, WEB_AGENT, player_js);
	}
	int sts = int(config.get("STS", 0));
	if (sts <= 0) {
		sts = signature_timestamp(player_js);
	}
	String key = config.get("INNERTUBE_API_KEY", String());
	String endpoint = "https://www.youtube.com/youtubei/v1/player?prettyPrint=false";
	if (!key.is_empty()) {
		endpoint += "&key=" + key.uri_encode();
	}
	Array diagnostics;
	for (int index = 0; index < 3 && !_interrupted(); index++) {
		Dictionary client = player_client(index, config);
		Dictionary context;
		context["client"] = client;
		Dictionary query;
		query["context"] = context;
		query["videoId"] = requested_id;
		query["contentCheckOk"] = true;
		query["racyCheckOk"] = true;
		{
			Dictionary playback;
			playback["html5Preference"] = "HTML5_PREF_WANTS";
			if (sts > 0) {
				playback["signatureTimestamp"] = sts;
			}
			Dictionary wrapper;
			wrapper["contentPlaybackContext"] = playback;
			query["playbackContext"] = wrapper;
		}
		String response;
		String agent = client["userAgent"];
		Dictionary headers;
		headers["X-YouTube-Client-Name"] = index == 0 ? "7" : index == 1 ? "101"
																		 : "1";
		headers["X-YouTube-Client-Version"] = client["clientVersion"];
		headers["X-Goog-Visitor-Id"] = client.get("visitorData", String());
		error = _http(endpoint, agent, response, JSON::stringify(query), headers);
		if (error == OK) {
			error = _prepare(parse_object(response), player_js, agent);
			if (error == OK) {
				if (String(result.get("title", String())).is_empty()) {
					Dictionary page = YouTubeParser::object_after(webpage, "ytInitialPlayerResponse =");
					Dictionary details = page.get("videoDetails", Dictionary());
					if (String(details.get("videoId", String())) == requested_id) {
						result["title"] = details.get("title", String());
					}
				}
				return OK;
			}
		}
		diagnostics.append(vformat("%s: %s", client["clientName"], result.get("message", "Player request failed.")));
	}
	if (_interrupted()) {
		return cancelled.is_set() ? ERR_SKIP : ERR_TIMEOUT;
	}
	Dictionary initial = YouTubeParser::object_after(webpage, "ytInitialPlayerResponse =");
	if (!initial.is_empty()) {
		error = _prepare(initial, player_js, WEB_AGENT);
		if (error == OK) {
			return OK;
		}
		diagnostics.append("Watch page: " + String(result.get("message", "No playable formats.")));
	}
	result["diagnostics"] = diagnostics;
	if (!result.has("message")) {
		result["message"] = "YouTube returned no supported player response.";
	}
	return error;
}
