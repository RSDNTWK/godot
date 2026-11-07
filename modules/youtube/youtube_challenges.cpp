/**************************************************************************/
/*  youtube_challenges.cpp                                              */
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

#include "youtube_challenges.h"

#include "thirdparty/quickjs_ng/quickjs.h"
#include "youtube_ejs.gen.h"

#include "core/io/json.h"
#include "core/os/os.h"
#ifdef ANDROID_ENABLED
#include <pthread.h>
#endif

namespace {
struct Limits {
	const SafeFlag &cancel;
	uint64_t deadline;
};

int interrupt(JSContext *, void *p_opaque) {
	const Limits *limits = static_cast<const Limits *>(p_opaque);
	return limits->cancel.is_set() || OS::get_singleton()->get_ticks_usec() >= limits->deadline;
}

bool evaluate(JSContext *p_context, const char *p_script, size_t p_size, const char *p_name, String &r_error) {
	JSValue value = JS_Eval(p_context, p_script, p_size, p_name, JS_EVAL_TYPE_GLOBAL);
	const bool failed = JS_IsException(value);
	JS_FreeValue(p_context, value);
	if (failed) {
		JSValue exception = JS_GetException(p_context);
		const char *message = JS_ToCString(p_context, exception);
		r_error = String::utf8(p_name) + ": " + (message ? String::utf8(message).left(1024) : "JavaScript evaluation failed.");
		JS_FreeCString(p_context, message);
		JS_FreeValue(p_context, exception);
	}
	return !failed;
}
} // namespace

static Error solve_here(const String &p_player, const Array &p_requests, const SafeFlag &p_cancel, Dictionary &r_response, String &r_error) {
	Limits limits{ p_cancel, OS::get_singleton()->get_ticks_usec() + 20000000 };
	JSRuntime *runtime = JS_NewRuntime();
	if (!runtime) {
		return ERR_OUT_OF_MEMORY;
	}
	JS_SetMemoryLimit(runtime, 256 * 1024 * 1024);
	JS_SetMaxStackSize(runtime, 2 * 1024 * 1024);
	JS_SetCanBlock(runtime, false);
	JS_SetInterruptHandler(runtime, interrupt, &limits);
	JSContext *context = JS_NewContext(runtime);
	if (!context) {
		JS_FreeRuntime(runtime);
		return ERR_OUT_OF_MEMORY;
	}
	// No quickjs-libc, module loader, filesystem, process or network functions.
	// Keep the upstream solver interface; its JS dependencies are bundled.
	Dictionary input;
	input["type"] = "player";
	input["player"] = p_player;
	input["requests"] = p_requests;
	input["output_preprocessed"] = false;
	CharString invocation = ("globalThis.__godot_youtube_result = JSON.stringify(jsc(" + JSON::stringify(input) + ")); ").utf8();
	static const char assign[] = "Object.assign(globalThis, lib);";
	bool ok = evaluate(context, youtube_ejs_lib, sizeof(youtube_ejs_lib) - 1, "ejs.lib.js", r_error) &&
			evaluate(context, assign, sizeof(assign) - 1, "ejs.setup.js", r_error) &&
			evaluate(context, youtube_ejs_core, sizeof(youtube_ejs_core) - 1, "ejs.core.js", r_error) &&
			evaluate(context, invocation.get_data(), invocation.length(), "youtube.challenge.js", r_error);
	Error error = ERR_PARSE_ERROR;
	if (ok) {
		JSValue global = JS_GetGlobalObject(context);
		JSValue result = JS_GetPropertyStr(context, global, "__godot_youtube_result");
		size_t length = 0;
		const char *json = JS_ToCStringLen(context, &length, result);
		if (json && length <= 1024 * 1024) {
			JSON parser;
			if (parser.parse(String::utf8(json, length)) == OK && parser.get_data().get_type() == Variant::DICTIONARY) {
				r_response = parser.get_data();
				error = OK;
			}
		}
		JS_FreeCString(context, json);
		JS_FreeValue(context, result);
		JS_FreeValue(context, global);
	}
	if (error != OK && r_error.is_empty()) {
		r_error = "The EJS solver did not produce a JSON object.";
	}
	JS_FreeContext(context);
	JS_FreeRuntime(runtime);
	if (p_cancel.is_set()) {
		return ERR_SKIP;
	}
	if (OS::get_singleton()->get_ticks_usec() >= limits.deadline) {
		return ERR_TIMEOUT;
	}
	return error;
}

#ifdef ANDROID_ENABLED
namespace {
struct AndroidSolve {
	const String &player;
	const Array &requests;
	const SafeFlag &cancel;
	Dictionary &response;
	String &error;
	Error result = FAILED;
};
void *android_solve(void *p_work) {
	AndroidSolve &work = *static_cast<AndroidSolve *>(p_work);
	work.result = solve_here(work.player, work.requests, work.cancel, work.response, work.error);
	return nullptr;
}
} // namespace
#endif

Error YouTubeChallenges::solve(const String &p_player, const Array &p_requests, const SafeFlag &p_cancel, Dictionary &r_response, String &r_error) {
#ifdef ANDROID_ENABLED
	// Android's default pthread stack is smaller than the solver's bounded JS
	// stack. Reserve an explicit native stack; this does not expose OS APIs to JS.
	AndroidSolve work{ p_player, p_requests, p_cancel, r_response, r_error };
	pthread_attr_t attributes;
	if (pthread_attr_init(&attributes) != 0) {
		return ERR_CANT_CREATE;
	}
	if (pthread_attr_setstacksize(&attributes, 8 * 1024 * 1024) != 0) {
		pthread_attr_destroy(&attributes);
		return ERR_CANT_CREATE;
	}
	pthread_t thread;
	int error = pthread_create(&thread, &attributes, android_solve, &work);
	pthread_attr_destroy(&attributes);
	if (error != 0) {
		return ERR_CANT_CREATE;
	}
	pthread_join(thread, nullptr);
	return work.result;
#else
	return solve_here(p_player, p_requests, p_cancel, r_response, r_error);
#endif
}
