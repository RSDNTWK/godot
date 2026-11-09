/**************************************************************************/
/*  ffmpeg_async_playback.h                                             */
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

#include "video_stream_media.h"

Ref<AudioStreamPlayback> ffmpeg_async_audio(const String &p_path, const Dictionary &p_headers, const Ref<AudioStreamMedia> &p_owner);
Ref<VideoStreamPlayback> ffmpeg_async_video(const String &p_path, const Dictionary &p_headers, const String &p_audio_file, const Dictionary &p_audio_headers, bool p_live_dvr_enabled = false, bool p_allow_http = false);
// Linked only in FFmpeg test builds. Exercises the real worker, PCM queue,
// frontend backpressure and terminal handling with a deterministic input.
using FFmpegTestVideoFactory = Ref<VideoStreamPlayback> (*)(void *);
Ref<VideoStreamPlayback> ffmpeg_test_async_video(FFmpegTestVideoFactory p_factory, void *p_userdata);
void register_ffmpeg_async_playback_types();
