/**************************************************************************/
/*  test_youtube.h                                                      */
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

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "tests/test_macros.h"

#include "modules/media_stream/tests/media_stream_test_utils.h"
#include "modules/youtube/youtube_challenges.h"
#include "modules/youtube/youtube_extractor.h"
#include "modules/youtube/youtube_hls.h"
#include "modules/youtube/youtube_parser.h"

#include <string>

namespace TestYouTube {
TEST_CASE("[YouTube][Network] Native extraction resolves a public watch link" * doctest::skip()) {
	String url = OS::get_singleton()->get_environment("GODOT_YOUTUBE_LIVE_URL");
	REQUIRE(!url.is_empty());
	if (url.is_empty()) {
		return;
	}
	TestMediaStream::load_certificates();
	SafeFlag cancelled;
	YouTubeExtractor extractor(cancelled);
	Dictionary result;
	Error error = extractor.resolve(url, false, result);
	INFO(std::string(String(result.get("message", "")).utf8().get_data()));
	INFO(std::string(JSON::stringify(result.get("diagnostics", Array())).utf8().get_data()));
	CHECK(error == OK);
	if (error != OK) {
		return;
	}
	CHECK(YouTubeParser::https_url(result.get("url", String())));
	CHECK(String(result.get("id", "")) == YouTubeParser::video_id(url));
	CHECK_FALSE(String(result.get("title", "")).is_empty());
	Dictionary selected = result["selected_format"];
	print_line(vformat("Native YouTube selection: protocol=%s, formats=%d, paired=%s", selected.get("protocol", ""), Array(result["formats"]).size(), bool(result.get("separate_audio", false)) ? "yes" : "no"));
	if (!bool(selected.get("has_audio", false))) {
		CHECK(YouTubeParser::https_url(result.get("audio_url", String())));
		CHECK(bool(result.get("separate_audio", false)));
	}
}

TEST_CASE("[YouTube][UpstreamFixture] Embedded EJS matches upstream n/signature results" * doctest::skip()) {
	String fixture = OS::get_singleton()->get_environment("GODOT_YOUTUBE_TEST_PLAYER");
	REQUIRE(!fixture.is_empty());
	String player = FileAccess::get_file_as_string(fixture);
	REQUIRE(!player.is_empty());
	Array requests;
	Dictionary n;
	n["type"] = "n";
	Array inputs;
	inputs.append("IlLiA21ny7gqA2m4p37");
	inputs.append("eabGFpsUKuWHXGh6FR4");
	n["challenges"] = inputs;
	requests.append(n);
	Dictionary signature;
	signature["type"] = "sig";
	Array signatures;
	String signed_input = "NJAJEij0EwRgIhAI0KExTgjfPk-MPM9MAdzyyPRt=BM8-XO5tm5hlMCSVpAiEAv7eP3CURqZNSPow8BXXAoazVoXgeMP7gH9BdylHCwgw=gwzz";
	signatures.append(signed_input);
	signature["challenges"] = signatures;
	requests.append(signature);
	SafeFlag cancelled;
	Dictionary response;
	String error;
	Error result = YouTubeChallenges::solve(player, requests, cancelled, response, error);
	INFO(std::string(error.utf8().get_data()));
	REQUIRE(result == OK);
	if (result != OK) {
		return;
	}
	REQUIRE(String(response.get("type", "")) == "result");
	Array responses = response["responses"];
	REQUIRE(responses.size() == 2);
	if (responses.size() != 2) {
		return;
	}
	Dictionary ns = Dictionary(responses[0]).get("data", Dictionary());
	Dictionary sigs = Dictionary(responses[1]).get("data", Dictionary());
	CHECK(String(ns.get(inputs[0], "")) == "9nRTxrbM1f0yHg");
	CHECK(String(ns.get(inputs[1], "")) == "izmYqDEY6kl7Sg");
	CHECK(String(sigs.get(signed_input, "")) == "NJAJEij0EwRgIhAI0KExTgjfPk-MPM9MAdzyyPRt=BM8-XO5tm5hzMCSVpAiEAv7eP3CURqZNSPow8BXXAoazVoXgeMP7gH9BdylHCwgw=gwzl");
}

TEST_CASE("[YouTube] URL normalization uses exact supported hosts and video IDs") {
	CHECK(YouTubeParser::video_id("https://www.youtube.com/watch?v=BaW_jenozKc&list=ignored") == "BaW_jenozKc");
	CHECK(YouTubeParser::video_id("https://youtu.be/BaW_jenozKc?t=10") == "BaW_jenozKc");
	CHECK(YouTubeParser::video_id("https://www.youtube.com/live/BaW_jenozKc#start") == "BaW_jenozKc");
	CHECK(YouTubeParser::video_id("https://music.youtube.com/watch?v=BaW_jenozKc") == "BaW_jenozKc");
	CHECK(YouTubeParser::video_id("https://youtube.com.example/watch?v=BaW_jenozKc").is_empty());
	CHECK(YouTubeParser::video_id("https://user@youtube.com/watch?v=BaW_jenozKc").is_empty());
	CHECK(YouTubeParser::video_id("http://youtube.com/watch?v=BaW_jenozKc").is_empty());
	CHECK(YouTubeParser::video_id("https://youtube.com/watch?v=BaW_jenozKcExtra").is_empty());
}

TEST_CASE("[YouTube] Signed query values preserve unrelated URL bytes") {
	String url = "https://example.com/video?keep=A%2BB&n=old%3Dvalue&n=duplicate";
	CHECK(YouTubeParser::query_value(url, "n") == "old=value");
	CHECK(YouTubeParser::set_query_value(url, "n", "new/value=") == "https://example.com/video?keep=A%2BB&n=new%2Fvalue%3D");
	CHECK(String(YouTubeParser::query("url=https%3A%2F%2Fexample.com%2F%3Fx%3DA%252BB")["url"]) == "https://example.com/?x=A%2BB");
}

TEST_CASE("[YouTube] Embedded JSON handles strings and repeated configuration records") {
	String text = R"(ytcfg.set({"nested":{"text":"} { \" \\"}}); ytcfg.set({"next":true});)";
	int end = -1;
	Dictionary first = YouTubeParser::object_after(text, "ytcfg.set(", 0, &end);
	CHECK(first.has("nested"));
	CHECK(end > 0);
	CHECK(bool(YouTubeParser::object_after(text, "ytcfg.set(", end).get("next", false)));
}

TEST_CASE("[YouTube] Named bootstrap calls cannot hide later watch configuration") {
	String page = R"(ytcfg.set('EMERGENCY_BASE_URL', '/error');
        ytcfg.set({"VISITOR_DATA":"public-fixture","INNERTUBE_CONTEXT":{"client":{"clientVersion":"current"}}});
        ytcfg.set('FLAG', true); ytcfg.set({broken:true});
        ytcfg.set({"PLAYER_JS_URL":"/s/player/test/base.js","replace":1});
        ytcfg.set({"replace":2});)";
	Dictionary config = YouTubeParser::configuration(page);
	CHECK(String(config.get("VISITOR_DATA", "")) == "public-fixture");
	Dictionary context = config.get("INNERTUBE_CONTEXT", Dictionary());
	Dictionary client = context.get("client", Dictionary());
	CHECK(String(client.get("clientVersion", "")) == "current");
	CHECK(YouTubeParser::player_url(page, config) == "https://www.youtube.com/s/player/test/base.js");
	CHECK(int(config.get("replace", 0)) == 2);
	CHECK(YouTubeParser::configuration("ytcfg.set('FLAG', true)").is_empty());
}

TEST_CASE("[YouTube] Unsupported IAMF presentations cannot hide elementary audio") {
	Dictionary iamf;
	iamf["url"] = "https://example.com/immersive";
	iamf["mimeType"] = "audio/mp4; codecs=\"iamf.001.001.Opus\"";
	iamf["bitrate"] = 1000000;
	iamf["itag"] = 774;
	Dictionary aac = iamf.duplicate();
	aac["url"] = "https://example.com/stereo";
	aac["mimeType"] = "audio/mp4; codecs=\"mp4a.40.2\"";
	aac["bitrate"] = 128000;
	aac["itag"] = 140;
	Array source;
	source.append(iamf);
	source.append(aac);
	Dictionary streaming;
	streaming["adaptiveFormats"] = source;
	Dictionary player;
	player["streamingData"] = streaming;
	Array usable = YouTubeParser::formats(player);
	REQUIRE(usable.size() == 1);
	CHECK(int(YouTubeParser::select_format(usable, true)["itag"]) == 140);
}

TEST_CASE("[YouTube] Live HLS takes precedence over adaptive challenge candidates") {
	JSON parser;
	REQUIRE(parser.parse(R"({"videoDetails":{"isLive":true},"streamingData":{"hlsManifestUrl":"https://example.com/live.m3u8","adaptiveFormats":[{"itag":137,"mimeType":"video/mp4","height":1080,"signatureCipher":"url=https%3A%2F%2Fexample.com%2Flive-dash%3Fn%3Dunsolved&s=unsolved&sp=sig"}]}})") == OK);
	Dictionary player = parser.get_data();
	Array formats = YouTubeParser::formats(player);
	REQUIRE(formats.size() == 1);
	Dictionary live = formats[0];
	CHECK(String(live["protocol"]) == "hls");
	CHECK(String(live["signature"]).is_empty());
	CHECK(String(live["n"]).is_empty());
	CHECK(String(YouTubeParser::select_format(formats, false)["url"]) == "https://example.com/live.m3u8");
	CHECK(String(YouTubeParser::select_format(formats, true)["url"]) == "https://example.com/live.m3u8");
	Dictionary streaming = player["streamingData"];
	streaming.erase("hlsManifestUrl");
	CHECK(YouTubeParser::formats(player).is_empty());
	Dictionary details = player["videoDetails"];
	details["isLive"] = false;
	formats = YouTubeParser::formats(player);
	REQUIRE(formats.size() == 1);
	Dictionary vod = formats[0];
	CHECK(String(vod["protocol"]) == "https");
	CHECK_FALSE(String(vod["signature"]).is_empty());
}

TEST_CASE("[YouTube] Selection prefers audio-only and keeps video audio together") {
	Array formats;
	Dictionary muxed;
	muxed["has_audio"] = true;
	muxed["has_video"] = true;
	muxed["bitrate"] = 1000000;
	muxed["height"] = 720;
	muxed["itag"] = 22;
	formats.append(muxed);
	Dictionary audio;
	audio["has_audio"] = true;
	audio["has_video"] = false;
	audio["bitrate"] = 128000;
	audio["itag"] = 140;
	formats.append(audio);
	CHECK(int(YouTubeParser::select_format(formats, true)["itag"]) == 140);
	CHECK(int(YouTubeParser::select_format(formats, false)["itag"]) == 22);
	Dictionary adaptive = muxed.duplicate();
	adaptive["has_audio"] = false;
	adaptive["height"] = 1080;
	adaptive["itag"] = 137;
	formats.append(adaptive);
	CHECK(int(YouTubeParser::select_format(formats, false)["itag"]) == 137);
	CHECK(int(YouTubeParser::select_format(formats, true)["itag"]) == 140);
}

TEST_CASE("[YouTube][QualityCap] Direct selection prefers 1080p60 and rejects larger or faster video") {
	JSON parser;
	REQUIRE(parser.parse(R"({"streamingData":{"adaptiveFormats":[{"itag":1,"url":"https://example.com/4k","mimeType":"video/mp4","width":3840,"height":2160,"fps":60,"bitrate":9000000},{"itag":2,"url":"https://example.com/1080-120","mimeType":"video/mp4","width":1920,"height":1080,"fps":120,"bitrate":8000000},{"itag":3,"url":"https://example.com/1080-30","mimeType":"video/mp4","width":1920,"height":1080,"fps":30,"bitrate":7000000},{"itag":4,"url":"https://example.com/1080-60","mimeType":"video/mp4","width":1920,"height":1080,"fps":60,"bitrate":4000000},{"itag":5,"url":"https://example.com/720-60","mimeType":"video/mp4","width":1280,"height":720,"fps":60,"bitrate":3000000},{"itag":6,"url":"https://example.com/audio","mimeType":"audio/mp4","bitrate":128000}]}})") == OK);
	Array formats = YouTubeParser::formats(parser.get_data());
	CHECK(int(YouTubeParser::select_format(formats, false)["itag"]) == 4);
	CHECK(double(YouTubeParser::select_format(formats, false)["fps"]) == 60.0);
	CHECK(int(YouTubeParser::select_format(formats, false, 720)["itag"]) == 5);
	CHECK(int(YouTubeParser::select_format(formats, true)["itag"]) == 6);
	formats.remove_at(3);
	CHECK(int(YouTubeParser::select_format(formats, false)["itag"]) == 3);
	formats.remove_at(2);
	CHECK(int(YouTubeParser::select_format(formats, false)["itag"]) == 5);
	formats.remove_at(2);
	CHECK(YouTubeParser::select_format(formats, false).is_empty());
}

TEST_CASE("[YouTube][HLS][QualityCap] Selects capped resolution and frame rate with lower-rung fallback") {
	const String prefix = "#EXTM3U\n";
	const String high = "#EXT-X-STREAM-INF:BANDWIDTH=9000000,CODECS=\"avc1.640028,mp4a.40.2\",RESOLUTION=3840x2160,FRAME-RATE=60\n4k.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=8000000,CODECS=\"avc1.640028,mp4a.40.2\",RESOLUTION=1920x1080,FRAME-RATE=120\nfast.m3u8\n";
	const String hd30 = "#EXT-X-STREAM-INF:BANDWIDTH=7000000,CODECS=\"avc1.640028,mp4a.40.2\",RESOLUTION=1920x1080,FRAME-RATE=30\nhd30.m3u8\n";
	const String hd60 = "#EXT-X-STREAM-INF:BANDWIDTH=4000000,CODECS=\"avc1.640028,mp4a.40.2\",RESOLUTION=1920x1080,FRAME-RATE=60\nhd60.m3u8\n";
	const String lower = "#EXT-X-STREAM-INF:BANDWIDTH=3000000,CODECS=\"avc1.640028,mp4a.40.2\",RESOLUTION=1280x720,FRAME-RATE=60\nlower.m3u8\n";
	Dictionary selected, audio;
	bool media = false;
	REQUIRE(YouTubeHLS::select_manifest(prefix + high + hd30 + hd60 + lower, "https://example.com/master.m3u8", false, selected, audio, media) == OK);
	CHECK(String(selected["url"]) == "https://example.com/hd60.m3u8");
	REQUIRE(YouTubeHLS::select_manifest(prefix + high + hd30 + hd60 + lower, "https://example.com/master.m3u8", false, selected, audio, media, 720) == OK);
	CHECK(String(selected["url"]) == "https://example.com/lower.m3u8");
	REQUIRE(YouTubeHLS::select_manifest(prefix + high + hd30 + lower, "https://example.com/master.m3u8", false, selected, audio, media) == OK);
	CHECK(String(selected["url"]) == "https://example.com/hd30.m3u8");
	REQUIRE(YouTubeHLS::select_manifest(prefix + high + lower, "https://example.com/master.m3u8", false, selected, audio, media) == OK);
	CHECK(int(selected["height"]) == 720);
	CHECK(YouTubeHLS::select_manifest(prefix + high, "https://example.com/master.m3u8", false, selected, audio, media) == ERR_UNAVAILABLE);
	CHECK(YouTubeHLS::select_manifest(prefix + hd60.replace("FRAME-RATE=60", "FRAME-RATE=invalid"), "https://example.com/master.m3u8", false, selected, audio, media) == ERR_UNAVAILABLE);
}

static String hls_master(const String &p_video, const String &p_audio = String(), bool p_audio_only = false) {
	String body = "#EXTM3U\n";
	if (!p_audio.is_empty()) {
		body += "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=\"English, Main\",DEFAULT=YES,URI=\"" + p_audio + "\"\n";
	}
	body += "#EXT-X-STREAM-INF:BANDWIDTH=300000,CODECS=\"";
	body += p_audio_only ? "mp4a.40.2\"" : "avc1.4d4015,mp4a.40.2\",RESOLUTION=426x240";
	if (!p_audio.is_empty()) {
		body += ",AUDIO=\"audio\"";
	}
	return body + "\n" + p_video + "\n";
}

static const char *HLS_MEDIA = "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:10\n#EXTINF:6,\nsegment.m4s\n";

struct HLSFixtureReader {
	Dictionary bodies;
	Dictionary redirects;
	int calls = 0;
	static Error read(void *p_self, const String &p_url, String &r_body, String &r_effective) {
		HLSFixtureReader *self = static_cast<HLSFixtureReader *>(p_self);
		self->calls++;
		if (!self->bodies.has(p_url)) {
			return ERR_FILE_NOT_FOUND;
		}
		r_body = self->bodies[p_url];
		r_effective = self->redirects.get(p_url, p_url);
		return OK;
	}
};

TEST_CASE("[YouTube][HLS] Bounded master selection chooses one compatible rung and preserves signed query bytes") {
	String master = "#EXTM3U\r\n#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=1,URI=\"iframe.m3u8\"\r\n";
	const int widths[] = { 426, 640, 854, 1280, 1920, 2560, 3840, 7680 };
	const int heights[] = { 240, 360, 480, 720, 1080, 1440, 2160, 4320 };
	for (int i = 0; i < 8; i++) {
		master += vformat("#EXT-X-STREAM-INF:BANDWIDTH=%d,CODECS=\"avc1.4d4015,mp4a.40.2\",RESOLUTION=%dx%d\r\n", (i + 1) * 300000, widths[i], heights[i]);
		master += i == 0 ? "low.m3u8?sig=a%2Bb&n=x%2fy&next=https://example.com/a,b\r\n" : i == 4 ? "rung4.m3u8?sig=a%2Bb&n=x%2fy&next=https://example.com/a,b\r\n"
																								  : vformat("rung%d.m3u8\r\n", i);
	}
	master += "#EXT-X-STREAM-INF:BANDWIDTH=1,CODECS=\"av01.0.08M.08,mp4a.40.2\",RESOLUTION=256x144\nunsupported.m3u8\n";
	Dictionary selected, audio;
	bool media = false;
	REQUIRE(YouTubeHLS::select_manifest(master, "https://cdn.example.com/final/master.m3u8?unused=1", false, selected, audio, media) == OK);
	CHECK_FALSE(media);
	CHECK(String(selected["url"]) == "https://cdn.example.com/final/rung4.m3u8?sig=a%2Bb&n=x%2fy&next=https://example.com/a,b");
	CHECK(int(selected["width"]) == 1920);
	CHECK(int(selected["height"]) == 1080);
	CHECK(int(selected["bitrate"]) == 1500000);
	CHECK(bool(selected["has_audio"]));
	CHECK(bool(selected["has_video"]));
	CHECK(audio.is_empty());
	REQUIRE(YouTubeHLS::select_manifest(master, "https://cdn.example.com/final/master.m3u8", true, selected, audio, media) == OK);
	CHECK(String(selected["url"]).contains("/low.m3u8?"));
}

TEST_CASE("[YouTube][HLS] Published VISIONOS master attributes retain their rendition records") {
	// Captured public player-response manifest, 2026-11-06. Signed locations
	// are replaced with static distinct URLs; provider attributes are retained.
	const String master = R"(#EXTM3U
#EXT-X-INDEPENDENT-SEGMENTS
#EXT-X-MEDIA:URI="https://example.com/live/audio-233.m3u8",TYPE=AUDIO,GROUP-ID="233",NAME="Default",DEFAULT=YES,AUTOSELECT=YES
#EXT-X-MEDIA:URI="https://example.com/live/audio-234.m3u8",TYPE=AUDIO,GROUP-ID="234",NAME="Default",DEFAULT=YES,AUTOSELECT=YES
#EXT-X-STREAM-INF:BANDWIDTH=507418,CODECS="avc1.4D4015,mp4a.40.5",RESOLUTION=426x240,FRAME-RATE=30,VIDEO-RANGE=SDR,AUDIO="233",CLOSED-CAPTIONS=NONE
https://example.com/live/rendition-0.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=591418,CODECS="avc1.4D4015,mp4a.40.2",RESOLUTION=426x240,FRAME-RATE=30,VIDEO-RANGE=SDR,AUDIO="234",CLOSED-CAPTIONS=NONE
https://example.com/live/rendition-1.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=962461,CODECS="avc1.4D401E,mp4a.40.2",RESOLUTION=640x360,FRAME-RATE=30,VIDEO-RANGE=SDR,AUDIO="234",CLOSED-CAPTIONS=NONE
https://example.com/live/rendition-2.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=1282517,CODECS="avc1.4D401F,mp4a.40.2",RESOLUTION=854x480,FRAME-RATE=30,VIDEO-RANGE=SDR,AUDIO="234",CLOSED-CAPTIONS=NONE
https://example.com/live/rendition-3.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=2447970,CODECS="avc1.4D401F,mp4a.40.2",RESOLUTION=1280x720,FRAME-RATE=30,VIDEO-RANGE=SDR,AUDIO="234",CLOSED-CAPTIONS=NONE
https://example.com/live/rendition-4.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=269034,CODECS="avc1.42C00B,mp4a.40.5",RESOLUTION=256x144,FRAME-RATE=15,VIDEO-RANGE=SDR,AUDIO="233",CLOSED-CAPTIONS=NONE
https://example.com/live/rendition-5.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=4561186,CODECS="avc1.640028,mp4a.40.2",RESOLUTION=1920x1080,FRAME-RATE=30,VIDEO-RANGE=SDR,AUDIO="234",CLOSED-CAPTIONS=NONE
https://example.com/live/rendition-6.m3u8
)";
	Dictionary selected, audio;
	bool media = false;
	REQUIRE(YouTubeHLS::select_manifest(master, "https://example.com/master.m3u8", false, selected, audio, media) == OK);
	CHECK(int(selected["width"]) == 1920);
	CHECK(int(selected["height"]) == 1080);
	CHECK(int(selected["bitrate"]) == 4561186);
	CHECK(double(selected["fps"]) == 30.0);
	CHECK(String(selected["url"]) == "https://example.com/live/rendition-6.m3u8");
	CHECK_FALSE(bool(selected["has_audio"]));
	CHECK(String(audio["url"]) == "https://example.com/live/audio-234.m3u8");
	CHECK_FALSE(media);
	REQUIRE(YouTubeHLS::select_manifest(master, "https://example.com/master.m3u8", true, selected, audio, media) == OK);
	CHECK(String(selected["url"]) == "https://example.com/live/audio-233.m3u8");
	CHECK_FALSE(bool(selected["has_video"]));
	CHECK(audio.is_empty());
}

TEST_CASE("[YouTube][HLS] Default audio groups select separate inputs without probing video for audio-only playback") {
	const String master = "#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=\"Alternate\",AUTOSELECT=YES,URI=\"alt.m3u8\"\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=\"English, Main\",DEFAULT=YES,URI=\"/audio.m3u8?sig=raw+%2F,a\"\n#EXT-X-STREAM-INF:BANDWIDTH=300000,CODECS=\"avc1.4d4015,mp4a.40.2\",RESOLUTION=426x240,AUDIO=\"audio\"\nvideo.m3u8\n";
	HLSFixtureReader fixture;
	fixture.bodies["https://origin.example.com/master.m3u8"] = master;
	fixture.redirects["https://origin.example.com/master.m3u8"] = "https://cdn.example.com/final/master.m3u8";
	fixture.bodies["https://cdn.example.com/final/video.m3u8"] = HLS_MEDIA;
	fixture.bodies["https://cdn.example.com/audio.m3u8?sig=raw+%2F,a"] = HLS_MEDIA;
	fixture.redirects["https://cdn.example.com/audio.m3u8?sig=raw+%2F,a"] = "https://audio.example.com/leaf.m3u8?sig=a%2Fb";
	Dictionary selected, audio;
	int requests = 0;
	REQUIRE(YouTubeHLS::resolve("https://origin.example.com/master.m3u8", false, HLSFixtureReader::read, &fixture, selected, audio, &requests) == OK);
	CHECK(requests == 3);
	CHECK(fixture.calls == 3);
	CHECK(String(selected["url"]) == "https://cdn.example.com/final/video.m3u8");
	CHECK_FALSE(bool(selected["has_audio"]));
	CHECK(String(audio["url"]) == "https://audio.example.com/leaf.m3u8?sig=a%2Fb");
	CHECK(bool(audio["has_audio"]));
	CHECK_FALSE(bool(audio["has_video"]));
	fixture.calls = 0;
	REQUIRE(YouTubeHLS::resolve("https://origin.example.com/master.m3u8", true, HLSFixtureReader::read, &fixture, selected, audio, &requests) == OK);
	CHECK(requests == 2);
	CHECK(fixture.calls == 2);
	CHECK(String(selected["url"]) == "https://audio.example.com/leaf.m3u8?sig=a%2Fb");
	CHECK_FALSE(bool(selected["has_video"]));
	CHECK(audio.is_empty());
}

TEST_CASE("[YouTube][HLS] Media leaves and URI forms remain HTTPS without changing signed queries") {
	Dictionary selected, audio;
	bool media = false;
	REQUIRE(YouTubeHLS::select_manifest(HLS_MEDIA, "https://example.com/leaf.m3u8?sig=raw+%2F", false, selected, audio, media) == OK);
	CHECK(media);
	CHECK(String(selected["url"]) == "https://example.com/leaf.m3u8?sig=raw+%2F");
	REQUIRE(YouTubeHLS::select_manifest(hls_master("//other.example.com/v.m3u8?sig=x%2fy"), "https://example.com/x/master.m3u8", false, selected, audio, media) == OK);
	CHECK(String(selected["url"]) == "https://other.example.com/v.m3u8?sig=x%2fy");
	REQUIRE(YouTubeHLS::select_manifest(hls_master("?sig=raw+%2F&next=https://example.org/"), "https://example.com/x/master.m3u8?old=1", false, selected, audio, media) == OK);
	CHECK(String(selected["url"]) == "https://example.com/x/master.m3u8?sig=raw+%2F&next=https://example.org/");
	CHECK(YouTubeHLS::select_manifest(hls_master("http://example.com/unsafe.m3u8"), "https://example.com/master.m3u8", false, selected, audio, media) == ERR_UNAVAILABLE);
	CHECK(YouTubeHLS::select_manifest(hls_master("data:unsupported"), "https://example.com/master.m3u8", false, selected, audio, media) == ERR_UNAVAILABLE);
	CHECK(YouTubeHLS::select_manifest(hls_master("https://user@example.com/v.m3u8"), "https://example.com/master.m3u8", false, selected, audio, media) == ERR_UNAVAILABLE);
}

TEST_CASE("[YouTube][HLS] Declared external AAC can be selected without its unsupported video codec") {
	const String master = hls_master("video.m3u8", "audio.m3u8").replace("avc1.4d4015", "av01.0.08M.08");
	Dictionary selected, audio;
	bool media = false;
	CHECK(YouTubeHLS::select_manifest(master, "https://example.com/master.m3u8", true, selected, audio, media) == OK);
	CHECK(String(selected["url"]) == "https://example.com/audio.m3u8");
	CHECK(audio.is_empty());
	CHECK(YouTubeHLS::select_manifest(master, "https://example.com/master.m3u8", false, selected, audio, media) == ERR_UNAVAILABLE);
	CHECK(selected.is_empty());
}

TEST_CASE("[YouTube][HLS] Malformed and excessive master records fail explicitly") {
	Dictionary selected, audio;
	bool media = false;
	CHECK(YouTubeHLS::select_manifest("#EXTM3U\n#EXT-X-STREAM-INF:CODECS=\"unterminated\nvideo.m3u8\n", "https://example.com/master.m3u8", false, selected, audio, media) == ERR_PARSE_ERROR);
	CHECK(YouTubeHLS::select_manifest("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1,BANDWIDTH=2\nvideo.m3u8\n", "https://example.com/master.m3u8", false, selected, audio, media) == ERR_PARSE_ERROR);
	CHECK(YouTubeHLS::select_manifest(hls_master("v.m3u8").replace("RESOLUTION=426x240", "RESOLUTION=426x240,AUDIO=\"missing\""), "https://example.com/master.m3u8", false, selected, audio, media) == ERR_UNAVAILABLE);
	CHECK(YouTubeHLS::select_manifest(hls_master("v.m3u8").replace("avc1.4d4015", "unknown.codec"), "https://example.com/master.m3u8", false, selected, audio, media) == ERR_UNAVAILABLE);
	String many = "#EXTM3U\n";
	for (int i = 0; i < YouTubeHLS::MAX_VARIANTS; i++) {
		many += hls_master(vformat("v%d.m3u8", i)).trim_prefix("#EXTM3U\n");
	}
	CHECK(YouTubeHLS::select_manifest(many, "https://example.com/master.m3u8", false, selected, audio, media) == OK);
	many += hls_master("overflow.m3u8").trim_prefix("#EXTM3U\n");
	CHECK(YouTubeHLS::select_manifest(many, "https://example.com/master.m3u8", false, selected, audio, media) == ERR_PARAMETER_RANGE_ERROR);
	CHECK(YouTubeHLS::select_manifest(String("x").repeat(YouTubeHLS::MAX_BODY + 1), "https://example.com/master.m3u8", false, selected, audio, media) == ERR_PARAMETER_RANGE_ERROR);
}

TEST_CASE("[YouTube][HLS] Nested selection enforces depth and detects redirect cycles") {
	HLSFixtureReader fixture;
	fixture.bodies["https://example.com/root.m3u8"] = hls_master("one.m3u8");
	fixture.bodies["https://example.com/one.m3u8"] = hls_master("two.m3u8");
	fixture.bodies["https://example.com/two.m3u8"] = hls_master("leaf.m3u8");
	fixture.bodies["https://example.com/leaf.m3u8"] = HLS_MEDIA;
	Dictionary selected, audio;
	int requests = 0;
	CHECK(YouTubeHLS::resolve("https://example.com/root.m3u8", false, HLSFixtureReader::read, &fixture, selected, audio, &requests) == OK);
	CHECK(requests == 4);
	fixture.bodies["https://example.com/leaf.m3u8"] = hls_master("too-deep.m3u8");
	CHECK(YouTubeHLS::resolve("https://example.com/root.m3u8", false, HLSFixtureReader::read, &fixture, selected, audio, &requests) == ERR_PARAMETER_RANGE_ERROR);
	CHECK(requests == 4);
	CHECK(selected.is_empty());
	fixture.bodies["https://example.com/one.m3u8"] = hls_master("root.m3u8");
	CHECK(YouTubeHLS::resolve("https://example.com/root.m3u8", false, HLSFixtureReader::read, &fixture, selected, audio, &requests) == ERR_CYCLIC_LINK);
	fixture.redirects["https://example.com/one.m3u8"] = "https://example.com/root.m3u8";
	CHECK(YouTubeHLS::resolve("https://example.com/root.m3u8", false, HLSFixtureReader::read, &fixture, selected, audio, &requests) == ERR_CYCLIC_LINK);
	CHECK(requests == 2);
}

TEST_CASE("[YouTube][HLS] Total selected-branch manifest requests remain bounded") {
	HLSFixtureReader fixture;
	fixture.bodies["https://example.com/root.m3u8"] = hls_master("v1.m3u8", "root-a1.m3u8");
	fixture.bodies["https://example.com/v1.m3u8"] = hls_master("v2.m3u8", "v1-a1.m3u8");
	fixture.bodies["https://example.com/v2.m3u8"] = hls_master("video-leaf.m3u8", "v2-a.m3u8");
	fixture.bodies["https://example.com/video-leaf.m3u8"] = HLS_MEDIA;
	fixture.bodies["https://example.com/v2-a.m3u8"] = HLS_MEDIA;
	fixture.bodies["https://example.com/v1-a1.m3u8"] = hls_master("v1-a2.m3u8", String(), true);
	fixture.bodies["https://example.com/v1-a2.m3u8"] = HLS_MEDIA;
	fixture.bodies["https://example.com/root-a1.m3u8"] = hls_master("root-a2.m3u8", String(), true);
	fixture.bodies["https://example.com/root-a2.m3u8"] = hls_master("root-a3.m3u8", String(), true);
	fixture.bodies["https://example.com/root-a3.m3u8"] = HLS_MEDIA;
	Dictionary selected, audio;
	int requests = 0;
	CHECK(YouTubeHLS::resolve("https://example.com/root.m3u8", false, HLSFixtureReader::read, &fixture, selected, audio, &requests) == ERR_PARAMETER_RANGE_ERROR);
	CHECK(requests == YouTubeHLS::MAX_REQUESTS);
	CHECK(fixture.calls == YouTubeHLS::MAX_REQUESTS);
	CHECK(selected.is_empty());
	CHECK(audio.is_empty());
}
} // namespace TestYouTube
