/**************************************************************************/
/*  youtube_resolver.cpp                                                */
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

#include "youtube_resolver.h"

#include "youtube_extractor.h"
#include "youtube_parser.h"

#include "core/object/class_db.h"

void YouTubeResolver::_bind_methods() {
	ClassDB::bind_method(D_METHOD("resolve", "url", "audio_only"), &YouTubeResolver::resolve, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("cancel"), &YouTubeResolver::cancel);
	ClassDB::bind_method(D_METHOD("is_resolving"), &YouTubeResolver::is_resolving);
	ClassDB::bind_static_method("YouTubeResolver", D_METHOD("get_video_id", "url"), &YouTubeResolver::get_video_id);
	ADD_SIGNAL(MethodInfo("completed", PropertyInfo(Variant::INT, "error"), PropertyInfo(Variant::DICTIONARY, "result")));
}

String YouTubeResolver::get_video_id(const String &p_url) {
	return YouTubeParser::video_id(p_url);
}

Error YouTubeResolver::resolve(const String &p_url, bool p_audio_only) {
	ERR_FAIL_COND_V(!is_inside_tree(), ERR_UNCONFIGURED);
	if (busy) {
		return ERR_BUSY;
	}
	String id = get_video_id(p_url);
	if (id.is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	requested_id = id;
	audio_only = p_audio_only;
	result.clear();
	result_error = FAILED;
	cancelled.clear();
	done.clear();
	busy = true;
	thread.start(_thread, this);
	if (!thread.is_started()) {
		busy = false;
		return ERR_CANT_CREATE;
	}
	set_process_internal(true);
	return OK;
}

void YouTubeResolver::_thread(void *p_self) {
	YouTubeResolver *self = static_cast<YouTubeResolver *>(p_self);
	YouTubeExtractor extractor(self->cancelled);
	self->result_error = extractor.resolve(self->requested_id, self->audio_only, self->result);
	self->done.set();
}

void YouTubeResolver::_notification(int p_what) {
	if (p_what == NOTIFICATION_INTERNAL_PROCESS && busy && done.is_set()) {
		thread.wait_to_finish();
		busy = false;
		set_process_internal(false);
		Dictionary completed_result = result;
		Error completed_error = result_error;
		emit_signal(SNAME("completed"), completed_error, completed_result);
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		cancel();
	}
}

void YouTubeResolver::cancel() {
	cancelled.set();
	if (thread.is_started()) {
		thread.wait_to_finish();
	}
	busy = false;
	set_process_internal(false);
}

YouTubeResolver::~YouTubeResolver() {
	cancelled.set();
	if (thread.is_started()) {
		thread.wait_to_finish();
	}
}
