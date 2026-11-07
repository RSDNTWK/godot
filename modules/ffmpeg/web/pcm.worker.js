/**************************************************************************/
/*  pcm.worker.js                                              */
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

/* Bounded finite preparation in a dedicated worker, including no-thread exports. */
'use strict';
let active = false;
const chunks = [];
let bytes = 0;
let config = null;
const INPUT_LIMIT = 64 * 1024 * 1024;
function fail(error, message) { self.postMessage({type: 'error', error, message}); }
async function decode(buffer) {
	try {
		importScripts(config.decoderURL);
		const mod = await GodotMediaPCMModule();
		const pointer = mod._malloc(buffer.byteLength);
		if (!pointer) throw new Error('Decoder input allocation failed.');
		mod.HEAPU8.set(new Uint8Array(buffer), pointer);
		buffer = null;
		const error = mod._godot_pcm_decode(pointer, bytes, config.rate, config.limit, Math.max(1, config.timeout - (performance.now() - config.started)));
		mod._free(pointer);
		if (error) { fail(error, error === 6 ? 'Decoded audio exceeds the PCM byte limit.' : error === 24 ? 'PCM preparation timed out.' : 'Unsupported or invalid finite audio.'); return; }
		const count = mod._godot_pcm_size();
		const pcm = mod.HEAPU8.slice(mod._godot_pcm_data(), mod._godot_pcm_data() + count);
		self.postMessage({type: 'pcm', rate: config.rate, frames: count / 4, title: mod.UTF8ToString(mod._godot_pcm_title()), pcm: pcm.buffer, dedicatedWorker: typeof WorkerGlobalScope !== 'undefined' && self instanceof WorkerGlobalScope}, [pcm.buffer]);
	} catch (error) { fail(30, String(error.message || error)); }
}
self.onmessage = async function (event) {
	const message = event.data;
	if (message.type === 'start' && !active) {
		active = true;
		config = message;
		config.started = performance.now();
		if (message.url) {
			try {
				const url = new URL(message.url);
				if (url.protocol !== 'https:' || url.username || url.password || url.hash || (url.port && url.port !== '443')) throw new Error('Only direct HTTPS sources are supported.');
				const response = await fetch(url.href, {mode: 'cors', credentials: 'omit', cache: 'no-store', redirect: 'error', referrerPolicy: 'no-referrer', headers: message.headers});
				if (!response.ok || response.type === 'opaque') throw new Error('Browser CORS/network request failed.');
				const length = Number(response.headers.get('Content-Length'));
				const mime = (response.headers.get('Content-Type') || '').toLowerCase();
				if (!Number.isSafeInteger(length) || length <= 0) { fail(2, 'PCM preparation requires a finite Content-Length; use browser streaming for live or unknown-length inputs.'); return; }
				if (length > INPUT_LIMIT) { fail(6, 'Encoded input exceeds the 64 MiB Web preparation limit.'); return; }
				if (mime.includes('mpegurl') || mime.includes('dash+xml') || mime.includes('event-stream')) { fail(2, 'Live/adaptive manifests require streaming, not PCM preparation.'); return; }
				const reader = response.body.getReader();
				for (;;) {
					const value = await reader.read();
					if (value.done) break;
					bytes += value.value.byteLength;
					if (bytes > INPUT_LIMIT || bytes > length) { await reader.cancel(); fail(6, 'Encoded input exceeds its declared finite size.'); return; }
					chunks.push(value.value);
				}
				if (bytes !== length) throw new Error('Finite media response was truncated.');
				const input = new Uint8Array(bytes);
				let offset = 0;
				for (const chunk of chunks) { input.set(chunk, offset); offset += chunk.length; }
				chunks.length = 0;
				await decode(input.buffer);
			} catch (error) { fail(25, 'Browser CORS, redirect or network restriction: ' + String(error.message || error)); }
		}
	} else if (message.type === 'input' && active && !config.url) {
		bytes += message.data.byteLength;
		if (bytes > INPUT_LIMIT) { fail(6, 'Encoded input exceeds the 64 MiB Web preparation limit.'); return; }
		chunks.push(new Uint8Array(message.data));
		self.postMessage({type: 'input_ack'});
		if (message.final) {
			const input = new Uint8Array(bytes);
			let offset = 0;
			for (const chunk of chunks) { input.set(chunk, offset); offset += chunk.length; }
			chunks.length = 0;
			await decode(input.buffer);
		}
	}
};
