/**************************************************************************/
/*  ffmpeg_quality_lookup.h                                                 */
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
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"

// One optional lookup per playback. The decoder retains its current selection
// while this job resolves a lower rendition. Only its owner starts/takes/joins;
// the lookup thread writes the result, then publishes completion atomically.
class FFmpegQualityLookup {
public:
	struct Result {
		uint64_t generation = 0;
		int limit = 0;
		bool found = false;
		bool dvr_supported = false;
		String video;
		Dictionary video_headers;
		String audio;
		Dictionary audio_headers;
		Dictionary metadata;
		String message;
	};
	using Resolve = bool (*)(void *, const String &, const SafeFlag &, int, Result &);

private:
	Thread worker;
	SafeFlag cancel;
	SafeFlag completed;
	String source;
	Result result;
	Resolve resolve = nullptr;
	void *userdata = nullptr;

	static void run(void *p_self) {
		FFmpegQualityLookup *self = static_cast<FFmpegQualityLookup *>(p_self);
		self->result.found = self->resolve(self->userdata, self->source, self->cancel, self->result.limit, self->result);
		self->completed.set();
	}

public:
	FFmpegQualityLookup() = default;
	FFmpegQualityLookup(const FFmpegQualityLookup &) = delete;
	FFmpegQualityLookup &operator=(const FFmpegQualityLookup &) = delete;

	Error start(const String &p_source, uint64_t p_generation, int p_limit, Resolve p_resolve, void *p_userdata = nullptr) {
		if (worker.is_started()) {
			return ERR_BUSY;
		}
		if (!p_resolve || p_source.is_empty() || p_limit <= 0) {
			return ERR_INVALID_PARAMETER;
		}
		source = p_source;
		result = Result();
		result.generation = p_generation;
		result.limit = p_limit;
		resolve = p_resolve;
		userdata = p_userdata;
		cancel.clear();
		completed.clear();
		worker.start(run, this);
		if (!worker.is_started()) {
			source = String();
			result = Result();
			return ERR_CANT_CREATE;
		}
		return OK;
	}

	bool is_running() const { return worker.is_started(); }
	bool is_ready() const { return worker.is_started() && completed.is_set(); }
	bool take(Result &r_result) {
		if (!is_ready()) {
			return false;
		}
		worker.wait_to_finish();
		r_result = result;
		result = Result();
		source = String();
		completed.clear();
		return true;
	}

	void cancel_and_wait() {
		cancel.set();
		if (worker.is_started()) {
			worker.wait_to_finish();
		}
		result = Result();
		source = String();
		completed.clear();
	}
	~FFmpegQualityLookup() { cancel_and_wait(); }
};
