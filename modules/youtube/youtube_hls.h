/**************************************************************************/
/*  youtube_hls.h                                                  */
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

#include "core/error/error_list.h"
#include "core/variant/dictionary.h"

// Bounded manifest selection for the YouTube live path. The reader must use
// validated HTTPS and return its final URL so relative references survive redirects.
namespace YouTubeHLS {
constexpr int MAX_BODY = 1024 * 1024;
constexpr int MAX_VARIANTS = 128;
constexpr int MAX_DEPTH = 4;
constexpr int MAX_REQUESTS = 8;
using ManifestReader = Error (*)(void *p_userdata, const String &p_url, String &r_body, String &r_effective_url);

Error select_manifest(const String &p_body, const String &p_base, bool p_audio_only, Dictionary &r_selected, Dictionary &r_audio, bool &r_media, int p_max_height = 1080);
Error resolve(const String &p_url, bool p_audio_only, ManifestReader p_reader, void *p_userdata, Dictionary &r_selected, Dictionary &r_audio, int *r_requests = nullptr, int p_max_height = 1080);
} // namespace YouTubeHLS
