/**************************************************************************/
/*  web_pcm_worker_faults.worker.js                                       */
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

// Run only from web_pcm_worker_faults.html. Faults stay outside production workers.
'use strict';
const parameters = new URL(self.location.href).searchParams;
const mode = parameters.get('mode');
const load = self.importScripts.bind(self);
let wrapped = false;
if (mode === 'join-oom' || mode === 'remote-join-oom') {
	self.Uint8Array = new Proxy(Uint8Array, {
		construct(target, args, newTarget) {
			if (typeof args[0] === 'number' && args[0] === Number(parameters.get('inputBytes'))) throw new RangeError('Injected valid-size input allocation failure.');
			return Reflect.construct(target, args, newTarget);
		},
	});
}
function exhaust(module) {
	let allocated = 0;
	for (const size of [8388608, 65536, 4096, 256, 1]) {
		for (let count = 0; count < 20000; count++) {
			if (!module._malloc(size)) break;
			allocated += size;
		}
	}
	self.postMessage({ type: 'diagnostic', allocated, heap: module.HEAPU8.byteLength, exhausted: module._malloc(1) === 0 });
}
self.importScripts = function (...urls) {
	if (mode === 'load-error') throw new DOMException('Injected decoder load failure.', 'NetworkError');
	load(...urls);
	if (wrapped || typeof self.GodotMediaPCMModule !== 'function') return;
	wrapped = true;
	const create = self.GodotMediaPCMModule;
	self.GodotMediaPCMModule = async function (...args) {
		if (mode === 'init-error') throw new Error('Injected module initialization failure.');
		const module = await create(...args);
		if (mode === 'input-oom') exhaust(module);
		if (mode === 'throw-cleanup') {
			let inputPointer = 0;
			const allocate = module._malloc, free = module._free;
			module._malloc = size => { inputPointer = allocate(size); return inputPointer; };
			module._free = pointer => {
				free(pointer);
				if (pointer === inputPointer) self.postMessage({ type: 'diagnostic', inputFreed: true });
			};
		}
		const decode = module._godot_pcm_decode;
		module._godot_pcm_decode = function (...values) {
			if (mode === 'throw-cleanup') throw new Error('Injected decoder exception.');
			if (mode === 'state-oom') exhaust(module);
			if (mode === 'output-oom') {
				const reserve = module._malloc(4194304);
				if (!reserve) throw new Error('Test could not reserve decoder working space.');
				exhaust(module);
				module._free(reserve);
			}
			let descriptor;
			if (mode === 'deadline') {
				descriptor = Object.getOwnPropertyDescriptor(performance, 'now');
				let ticks = performance.now();
				Object.defineProperty(performance, 'now', { value: () => ticks += 100, configurable: true });
				values[4] = 1;
			}
			try {
				const error = decode(...values);
				self.postMessage({ type: 'native-result', error, partialBytes: error ? module._godot_pcm_size() : 0 });
				if (mode === 'transfer-oom' && !error) module.HEAPU8.slice = () => { throw new RangeError('Injected PCM transfer allocation failure.'); };
				return error;
			} finally {
				if (mode === 'deadline') {
					if (descriptor) Object.defineProperty(performance, 'now', descriptor);
					else delete performance.now;
				}
			}
		};
		return module;
	};
};
load(parameters.get('worker'));
if (mode === 'remote-join-oom') {
	const receive = self.onmessage;
	self.onmessage = function (event) {
		if (event.data.type === 'start') {
			const fixture = event.data.fixture;
			self.fetch = async () => new Response(fixture, { headers: { 'Content-Length': String(fixture.byteLength), 'Content-Type': 'audio/wav' } });
		}
		return receive.call(self, event);
	};
}
