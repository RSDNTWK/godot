/**************************************************************************/
/*  web_pcm_bridge_test.cjs                                               */
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

'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(process.argv[2] || path.join(__dirname, '../web/library_godot_media.js'), 'utf8');
function setup() {
	const workers = [];
	const timers = [];
	class Worker {
		constructor() { this.messages = []; this.terminated = false; workers.push(this); }
		postMessage(value) { if (this.postError) throw new Error('Injected post failure'); this.messages.push(value); }
		terminate() { this.terminated = true; }
	}
	const heap = new Uint8Array(512);
	const context = { Worker, HEAPU8: heap, Uint8Array, ArrayBuffer, RangeError, Error, Map, JSON,
		LibraryManager: { library: {} }, autoAddDeps() {}, mergeInto: Object.assign,
		GodotConfig: { locate_file: value => value }, GodotRuntime: { parseString: value => value, stringToHeap() {} },
		setTimeout: callback => { const timer = { callback, cleared: false }; timers.push(timer); return timer; }, clearTimeout: timer => { if (timer) timer.cleared = true; },
	};
	vm.createContext(context);
	vm.runInContext(source + '\nglobalThis.state = GodotMedia;', context);
	const api = context.LibraryManager.library, state = context.state;
	// Emscripten emits the $GodotMedia state as the runtime GodotMedia object.
	Object.assign(state, state.$GodotMedia);
	const start = () => api.godot_web_pcm_start('', 48000, 1024, 10000, '{}');
	return { api, state, start, workers, heap, timers };
}
const checks = [];
function check(name, test) {
	try { test(); checks.push({ name, passed: true }); }
	catch (error) { checks.push({ name, passed: false, error: String(error.stack || error) }); }
}
check('Worker failure is generic failure, not malformed media', () => {
	const t = setup(), id = t.start(), request = t.state.requests.get(id);
	t.workers[0].onerror({ message: 'Injected worker failure' });
	assert.equal(request.error, 1); assert.equal(request.state, -1);
	assert.ok(request.worker.terminated && request.timer.cleared);
});
check('Malformed PCM still returns invalid data', () => {
	const t = setup(), id = t.start(); t.workers[0].onmessage({ data: { type: 'pcm' } });
	assert.equal(t.state.requests.get(id).error, 30);
});
check('Worker allocation failure remains out of memory', () => {
	const t = setup(), id = t.start(); t.workers[0].onmessage({ data: { type: 'error', error: 6, message: 'allocation failed' } });
	assert.equal(t.state.requests.get(id).error, 6);
});
check('Input transfer allocation failure is contained and retained for the collector', () => {
	const t = setup(), id = t.start(); t.heap.slice = () => { throw new RangeError('Injected allocation failure'); };
	assert.equal(t.api.godot_web_pcm_input(id, 8, 16, 1), 0);
	assert.equal(t.state.requests.get(id).error, 6); assert.ok(t.workers[0].terminated);
});
check('Input post failure is contained', () => {
	const t = setup(), id = t.start(); t.workers[0].postError = true;
	assert.equal(t.api.godot_web_pcm_input(id, 8, 16, 1), 0);
	assert.equal(t.state.requests.get(id).error, 1); assert.ok(t.workers[0].terminated);
});
check('Startup post failure releases its worker, timer and request slot', () => {
	const t = setup();
	const prototype = Object.getPrototypeOf(t.start() && t.workers[0]);
	t.api.godot_web_pcm_free(1);
	prototype.postError = true;
	assert.equal(t.start(), 0); assert.equal(t.state.requests.size, 0);
	assert.ok(t.workers[1].terminated);
	assert.ok(t.timers[1].cleared);
});
check('Stale callbacks do not alter a replacement request', () => {
	const t = setup(), first = t.start(), stale = t.workers[0]; t.api.godot_web_pcm_free(first);
	const second = t.start(); stale.onmessage({ data: { type: 'error', error: 6, message: 'stale' } });
	assert.equal(t.state.requests.get(second).state, 0); assert.equal(t.state.requests.size, 1);
});
check('Deadline and concurrency remain bounded', () => {
	const t = setup(), first = t.start();
	t.start(); t.start(); t.start(); assert.equal(t.start(), 0);
	t.state.requests.get(first).timer.callback(); assert.equal(t.state.requests.get(first).error, 24);
});
const result = { passed: checks.every(value => value.passed), checks };
console.log(JSON.stringify(result, null, 2));
process.exitCode = result.passed ? 0 : 1;
