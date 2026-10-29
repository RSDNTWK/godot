/**************************************************************************/
/*  test_gdscript_operator_cache.cpp                                    */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_gdscript_operator_cache)

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_GDSCRIPT_ENABLED

#include "core/os/os.h"
#include "core/os/thread.h"

#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/gdscript_function.h"

#include <atomic>
#include <cstdint>

namespace TestGDScriptOperatorCache {

struct Access {
	static int count(const GDScriptFunction *p_function) { return p_function->_operator_caches_count; }
	static bool aligned(const GDScriptFunction *p_function) {
		if (p_function->_operator_caches_ptr == nullptr) {
			return false;
		}
		for (int i = 0; i < p_function->_operator_caches_count; i++) {
			const auto &cache = p_function->_operator_caches_ptr[i];
			if (uintptr_t(&cache.evaluator) % alignof(Variant::ValidatedOperatorEvaluator) != 0) {
				return false;
			}
		}
		return true;
	}
	static void disable(GDScriptFunction *p_function) {
		if (p_function->_operator_caches_ptr != nullptr) {
			memdelete_arr(p_function->_operator_caches_ptr);
			p_function->_operator_caches_ptr = nullptr;
		}
	}
};

static Ref<GDScript> script() {
	GDScriptLanguage::get_singleton()->init();
	Ref<GDScript> result;
	result.instantiate();
	result->set_source_code(R"GD(
extends RefCounted

static func chain(a: Variant, b: Variant):
	var total: Variant = a + b
	return -total

static func equal_values(a: Variant, b: Variant):
	return a == b

static func logical_not(a: Variant):
	return not a

static func sum_loop(a: Variant, b: Variant, n: int = 9):
	var total: Variant = a
	for i in range(n):
		total = total + b
	return -total

static func divide_values(a: Variant, b: Variant):
	return a / b

static func remainder_values(a: Variant, b: Variant):
	return a % b
)GD");
	ERR_PRINT_OFF;
	const Error error = result->reload();
	ERR_PRINT_ON;
	REQUIRE(error == OK);
	return result;
}

static GDScriptFunction *function(const Ref<GDScript> &p_script, const StringName &p_name) {
	const auto *entry = p_script->get_member_functions().getptr(p_name);
	REQUIRE(entry != nullptr);
	return *entry;
}

static Variant call(GDScriptFunction *p_function, const Variant &p_a, const Variant &p_b) {
	const Variant *args[] = { &p_a, &p_b };
	Callable::CallError error;
	Variant result = p_function->call(nullptr, args, 2, error);
	CHECK(error.error == Callable::CallError::CALL_OK);
	return result;
}

TEST_CASE("[F23][GDScriptOperator] Consecutive generic binary and unary operations preserve cold warm and mixed type results") {
	const Ref<GDScript> source = script();
	GDScriptFunction *chain = function(source, "chain");
	REQUIRE(Access::count(chain) >= 2);
	CHECK(Access::aligned(chain));
	for (int i = 0; i < 128; i++) {
		CHECK(int64_t(call(chain, int64_t(4), int64_t(5))) == -9);
		CHECK(double(call(chain, 1.25, 2.75)) == -4.0);
	}
}

TEST_CASE("[F23][GDScriptOperator] Nil signatures and mismatched string and logical operands retain generic semantics") {
	const Ref<GDScript> source = script();
	GDScriptFunction *equal = function(source, "equal_values");
	for (int i = 0; i < 3; i++) {
		CHECK(bool(call(equal, Variant(), Variant())));
	}
	CHECK(bool(call(equal, int64_t(7), int64_t(7))));
	CHECK_FALSE(bool(call(equal, "first", "second")));
	CHECK(bool(call(equal, Variant(), Variant())));
	GDScriptFunction *logical = function(source, "logical_not");
	const Variant yes = true;
	const Variant zero = int64_t(0);
	Callable::CallError error;
	const Variant *args[] = { &yes };
	CHECK_FALSE(bool(logical->call(nullptr, args, 1, error)));
	CHECK(error.error == Callable::CallError::CALL_OK);
	args[0] = &zero;
	CHECK(bool(logical->call(nullptr, args, 1, error)));
	CHECK(error.error == Callable::CallError::CALL_OK);
}

TEST_CASE("[F23][GDScriptOperator] Existing instruction widths preserve loops jumps and default arguments") {
	const Ref<GDScript> source = script();
	GDScriptFunction *loop = function(source, "sum_loop");
	CHECK(int64_t(call(loop, int64_t(2), int64_t(3))) == -29);
	const Variant a = int64_t(2);
	const Variant b = int64_t(3);
	const Variant n = int64_t(4);
	const Variant *args[] = { &a, &b, &n };
	Callable::CallError error;
	CHECK(int64_t(loop->call(nullptr, args, 3, error)) == -14);
	CHECK(error.error == Callable::CallError::CALL_OK);
	CHECK(double(call(loop, 0.25, 0.5)) == -4.75);
}

TEST_CASE("[F23][GDScriptOperator] Unavailable optional cache storage retains functional generic evaluation") {
	const Ref<GDScript> source = script();
	GDScriptFunction *chain = function(source, "chain");
	REQUIRE(Access::count(chain) >= 2);
	CHECK(int64_t(call(chain, int64_t(4), int64_t(5))) == -9);
	Access::disable(chain);
	for (int i = 0; i < 16; i++) {
		CHECK(int64_t(call(chain, int64_t(4), int64_t(5))) == -9);
		CHECK(double(call(chain, 1.25, 2.75)) == -4.0);
	}
}

struct ScriptErrors {
	ErrorHandlerList handler;
	int count = 0;
	String messages;
	bool previous = CoreGlobals::print_error_enabled;
	static void record(void *p_user, const char *, const char *, int, const char *p_error, const char *p_message, bool, ErrorHandlerType p_type) {
		if (p_type == ERR_HANDLER_SCRIPT) {
			ScriptErrors *self = static_cast<ScriptErrors *>(p_user);
			self->count++;
			self->messages += String::utf8(p_error) + String::utf8(p_message);
		}
	}
	ScriptErrors() {
		handler.errfunc = &record;
		handler.userdata = this;
		add_error_handler(&handler);
		CoreGlobals::print_error_enabled = false;
	}
	~ScriptErrors() {
		remove_error_handler(&handler);
		CoreGlobals::print_error_enabled = previous;
	}
};

TEST_CASE("[F23][GDScriptOperator] Division and modulo retain debug validation across repeated calls") {
	const Ref<GDScript> source = script();
	GDScriptFunction *divide = function(source, "divide_values");
	GDScriptFunction *remainder = function(source, "remainder_values");
	for (int i = 0; i < 4; i++) {
		CHECK(int64_t(call(divide, int64_t(9), int64_t(3))) == 3);
		CHECK(int64_t(call(remainder, int64_t(9), int64_t(4))) == 1);
	}
#ifdef DEBUG_ENABLED
	ScriptErrors errors;
	CHECK(call(divide, int64_t(9), int64_t(0)).get_type() == Variant::NIL);
	CHECK(errors.count == 1);
	CHECK(errors.messages.contains("zero"));
	CHECK(call(remainder, int64_t(9), int64_t(0)).get_type() == Variant::NIL);
	CHECK(errors.count == 2);
	CHECK(int64_t(call(divide, int64_t(9), int64_t(3))) == 3);
	CHECK(int64_t(call(remainder, int64_t(9), int64_t(4))) == 1);
#endif
}

struct ConcurrentCalls {
	GDScriptFunction *target = nullptr;
	bool equality = false;
	std::atomic<int> ready{ 0 };
	std::atomic<bool> go{ false };
	std::atomic<int> failures{ 0 };
	static void run(void *p_userdata) {
		ConcurrentCalls *work = static_cast<ConcurrentCalls *>(p_userdata);
		const int ticket = work->ready.fetch_add(1, std::memory_order_release);
		while (!work->go.load(std::memory_order_acquire)) {
			OS::get_singleton()->delay_usec(10);
		}
		for (int i = 0; i < 512; i++) {
			Variant a = (i + ticket) % 2 ? Variant(1.25) : Variant(int64_t(4));
			Variant b = (i + ticket) % 2 ? Variant(2.75) : Variant(int64_t(5));
			if (work->equality) {
				a = (i + ticket) % 2 ? Variant() : Variant(int64_t(7));
				b = a;
			}
			const Variant *args[] = { &a, &b };
			Callable::CallError error;
			const Variant result = work->target->call(nullptr, args, 2, error);
			const bool valid = work->equality ? result.get_type() == Variant::BOOL && bool(result) : ((i + ticket) % 2 ? result.get_type() == Variant::FLOAT && double(result) == -4.0 : result.get_type() == Variant::INT && int64_t(result) == -9);
			if (error.error != Callable::CallError::CALL_OK || !valid) {
				work->failures.fetch_add(1, std::memory_order_relaxed);
			}
		}
	}
};

TEST_CASE("[F23][GDScriptOperator] Native threads share first specialization without changing opcode storage") {
	for (bool equality : { false, true }) {
		const Ref<GDScript> source = script();
		ConcurrentCalls work;
		work.target = function(source, equality ? "equal_values" : "chain");
		work.equality = equality;
#ifdef THREADS_ENABLED
		Thread workers[4];
		int started = 0;
		for (Thread &worker : workers) {
			worker.start(&ConcurrentCalls::run, &work);
			if (worker.is_started()) {
				started++;
			}
		}
		const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 5000000;
		while (work.ready.load(std::memory_order_acquire) < started && OS::get_singleton()->get_ticks_usec() < deadline) {
			OS::get_singleton()->delay_usec(100);
		}
		const bool simultaneous = work.ready.load(std::memory_order_acquire) == started;
		work.go.store(true, std::memory_order_release);
		for (Thread &worker : workers) {
			if (worker.is_started()) {
				worker.wait_to_finish();
			}
		}
		CHECK(started == 4);
		CHECK(simultaneous);
#else
		work.go.store(true, std::memory_order_release);
		ConcurrentCalls::run(&work);
#endif
		CHECK(work.failures.load(std::memory_order_relaxed) == 0);
		CHECK(Access::aligned(work.target));
	}
}

} // namespace TestGDScriptOperatorCache

#endif // MODULE_GDSCRIPT_ENABLED
