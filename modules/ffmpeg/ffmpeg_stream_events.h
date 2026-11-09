/**************************************************************************/
/*  ffmpeg_stream_events.h                                              */
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

#include "core/object/class_db.h"
#include "core/object/ref_counted.h"
#include "core/templates/safe_refcount.h"

// A fence owns no playback reference. It becomes ready only after every
// derived backend member (including worker owners) has been destroyed.
class FFmpegRetirementFence : public RefCounted {
	GDCLASS(FFmpegRetirementFence, RefCounted);
	SafeFlag retired;

protected:
	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("is_retired"), &FFmpegRetirementFence::is_retired);
	}

public:
	bool is_retired() const { return retired.is_set(); }
	void mark_retired() { retired.set(); }
};

struct FFmpegRetirementGuard {
	Ref<FFmpegRetirementFence> fence;
	FFmpegRetirementGuard() { fence.instantiate(); }
	~FFmpegRetirementGuard() { fence->mark_retired(); }
};

struct FFmpegStreamEvents {
	bool ready = false;
	bool ready_notified = false;
	bool buffering = true;
	bool buffering_notified = false;
	Error error = OK;
	String message;
};

// Notifications are delivered on the main thread, and commands superseded by a
// later seek/stop cannot deliver stale completion events to the application.
#define FFMPEG_STREAM_EVENTS(m_class, m_override) \
	Ref<FFmpegRetirementFence> get_retirement_fence() const { return retirement.fence; } \
	FFmpegStreamEvents events; \
	FFmpegDVRTimeline dvr; \
	Dictionary stream_metadata; \
	Dictionary notified_metadata; \
	void set_metadata_locked(uint64_t p_generation, const Dictionary &p_metadata) { \
		stream_metadata = p_metadata.duplicate(); \
		stream_metadata["playback_instance_id"] = int64_t(uint64_t(get_instance_id())); \
		stream_metadata["generation"] = int64_t(p_generation); \
	} \
	Dictionary get_stream_metadata() const m_override { \
		MutexLock lock(mutex); \
		return stream_metadata.duplicate(); \
	} \
	void publish_metadata(uint64_t p_generation, const Dictionary &p_metadata) { \
		{ \
			MutexLock lock(mutex); \
			if (p_generation != generation || quit.is_set()) { \
				return; \
			} \
			set_metadata_locked(p_generation, p_metadata); \
		} \
		post_event(p_generation, 4); \
	} \
	Dictionary get_dvr_window() const m_override { \
		MutexLock lock(mutex); \
		return dvr.describe(position); \
	} \
	bool seek_to_live() m_override { \
		{ \
			MutexLock lock(mutex); \
			if (!dvr.enabled || !dvr.window.is_valid() || !bool(dvr.window->snapshot().get("available", false))) { \
				return false; \
			} \
		} \
		seek_internal(0, true); \
		return true; \
	} \
	bool is_ready() const { \
		MutexLock lock(mutex); \
		return events.ready; \
	} \
	bool is_buffering() const { \
		MutexLock lock(mutex); \
		return events.buffering; \
	} \
	int get_stream_error() const { \
		MutexLock lock(mutex); \
		return events.error; \
	} \
	void _stream_event(int64_t p_generation, int p_kind, double p_position) { \
		bool emit_event = true; \
		bool buffering = false; \
		Error error = OK; \
		String message; \
		Dictionary metadata; \
		{ \
			MutexLock lock(mutex); \
			if (uint64_t(p_generation) != generation) { \
				return; \
			} \
			if (p_kind == 0) { \
				emit_event = events.ready && !events.ready_notified; \
				events.ready_notified |= emit_event; \
			} \
			if (p_kind == 1) { \
				buffering = events.buffering; \
				emit_event = buffering != events.buffering_notified; \
				events.buffering_notified = buffering; \
			} \
			if (p_kind == 2) { \
				error = events.error; \
				message = events.message; \
			} \
			if (p_kind == 4) { \
				emit_event = stream_metadata != notified_metadata; \
				metadata = stream_metadata.duplicate(); \
				notified_metadata = metadata.duplicate(); \
			} \
		} \
		if (!emit_event) { \
			return; \
		} \
		if (p_kind == 0) { \
			emit_signal(SNAME("ready")); \
		} else if (p_kind == 1) { \
			emit_signal(SNAME("buffering_changed"), buffering); \
		} else if (p_kind == 2) { \
			emit_signal(SNAME("error"), int(error), message); \
		} else if (p_kind == 3) { \
			emit_signal(SNAME("seek_completed"), p_position); \
		} else if (p_kind == 4) { \
			emit_signal(SNAME("metadata_changed"), metadata); \
		} \
	} \
	void post_event(uint64_t p_generation, int p_kind, double p_position = 0) { \
		call_deferred(SNAME("_stream_event"), int64_t(p_generation), p_kind, p_position); \
	} \
	static void _bind_methods() { \
		ClassDB::bind_method(D_METHOD("get_retirement_fence"), &m_class::get_retirement_fence); \
		ClassDB::bind_method(D_METHOD("get_stream_metadata"), &m_class::get_stream_metadata); \
		ClassDB::bind_method(D_METHOD("get_dvr_window"), &m_class::get_dvr_window); \
		ClassDB::bind_method(D_METHOD("seek_to_live"), &m_class::seek_to_live); \
		ClassDB::bind_method(D_METHOD("is_ready"), &m_class::is_ready); \
		ClassDB::bind_method(D_METHOD("is_buffering"), &m_class::is_buffering); \
		ClassDB::bind_method(D_METHOD("get_stream_error"), &m_class::get_stream_error); \
		ClassDB::bind_method(D_METHOD("_stream_event", "generation", "kind", "position"), &m_class::_stream_event); \
		ADD_PROPERTY(PropertyInfo(Variant::BOOL, "stream_ready", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "is_ready"); \
		ADD_PROPERTY(PropertyInfo(Variant::BOOL, "buffering", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "is_buffering"); \
		ADD_PROPERTY(PropertyInfo(Variant::INT, "stream_error", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "get_stream_error"); \
		ADD_SIGNAL(MethodInfo("ready")); \
		ADD_SIGNAL(MethodInfo("buffering_changed", PropertyInfo(Variant::BOOL, "buffering"))); \
		ADD_SIGNAL(MethodInfo("error", PropertyInfo(Variant::INT, "code"), PropertyInfo(Variant::STRING, "message"))); \
		ADD_SIGNAL(MethodInfo("seek_completed", PropertyInfo(Variant::FLOAT, "position"))); \
		ADD_SIGNAL(MethodInfo("metadata_changed", PropertyInfo(Variant::DICTIONARY, "metadata"))); \
	}
