/**************************************************************************/
/*  youtube_extractor.h                                                 */
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

#include "core/string/ustring.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"

// Internal C++ API. Call only on a media/resolver worker.
class YouTubeExtractor {
	const SafeFlag &cancelled;
	String requested_id;
	bool audio_only = true;
	int max_height = 1080;
	Dictionary result;
	uint64_t deadline = 0;
	int hls_manifest_requests = 0;
	bool _interrupted() const;
	Error _http(const String &p_url, const String &p_agent, String &r_body, const String &p_post = String(), const Dictionary &p_headers = Dictionary());
	Error _hls_manifest(const String &p_url, const String &p_agent, String &r_body, String &r_effective_url);
	Error _prepare(const Dictionary &p_player, const String &p_player_js, const String &p_agent);
	Error _resolve();

public:
	explicit YouTubeExtractor(const SafeFlag &p_cancelled) : cancelled(p_cancelled) {}
	Error resolve(const String &p_url, bool p_audio_only, Dictionary &r_result, int p_max_height = 1080);
};
