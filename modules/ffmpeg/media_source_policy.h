/**************************************************************************/
/*  media_source_policy.h                                              */
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

namespace MediaSourcePolicy {
inline bool browser_url(const String &p_url) {
	if (p_url.length() > 32768 || (!p_url.begins_with("https://") && !p_url.begins_with("http://")) || p_url.contains("\\")) {
		return false;
	}
	for (int i = 0; i < p_url.length(); i++) {
		if (p_url[i] <= 32 || p_url[i] == 127) {
			return false;
		}
	}
	String scheme, host, path, fragment;
	int port;
	return p_url.parse_url(scheme, host, port, path, fragment) == OK && !host.is_empty();
}
inline bool https_url(const String &p_url) {
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
	return p_url.parse_url(scheme, host, port, path, fragment) == OK && !host.is_empty() && fragment.is_empty();
}
inline bool youtube_url(const String &p_url) {
	String scheme, host, path, fragment;
	int port;
	if (p_url.parse_url(scheme, host, port, path, fragment) != OK) {
		return false;
	}
	host = host.to_lower();
	return host == "youtube.com" || host.ends_with(".youtube.com") || host == "youtu.be" || host == "youtube-nocookie.com" || host.ends_with(".youtube-nocookie.com");
}
} //namespace MediaSourcePolicy
