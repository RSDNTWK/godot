/**************************************************************************/
/*  library_godot_media.js                                              */
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

/* Engine-owned browser workers. No native transport or shared memory required. */
const GodotMedia = {
	$GodotMedia__deps: ['$GodotRuntime', '$GodotConfig'],
	$GodotMedia: {
		next: 1,
		requests: new Map(),
		title: function (value) {
			return Array.from(String(value || '')).filter(function (character) {
				const code = character.codePointAt(0); return code >= 32 && code !== 127;
			}).slice(0, 256).map(function (character) {
				const code = character.codePointAt(0); return code >= 0xd800 && code <= 0xdfff ? '\ufffd' : character;
			}).join('');
		},
		finish: function (id, error, message) {
			const r = GodotMedia.requests.get(id);
			if (!r || r.state !== 0) return;
			r.state = -1; r.error = error; r.message = String(message).slice(0, 1024);
			r.worker.terminate(); clearTimeout(r.timer);
		},
	},
	godot_web_pcm_start__proxy: 'sync',
	godot_web_pcm_start__sig: 'iiiiii',
	godot_web_pcm_start: function (url, rate, limit, timeout, headers) {
		if (GodotMedia.requests.size >= 4) return 0;
		const id = GodotMedia.next++;
		try {
			const workerURL = GodotConfig.locate_file('godot.media.worker.js');
			const worker = new Worker(workerURL);
			const r = {worker, state: 0, error: 0, message: '', data: null, rate, inputReady: true, frames: 0, timer: null};
			GodotMedia.requests.set(id, r);
			r.timer = setTimeout(function () { GodotMedia.finish(id, 24, 'PCM preparation timed out.'); }, timeout);
			worker.onerror = function (event) { GodotMedia.finish(id, 1, event.message || 'Dedicated decoder worker failed.'); };
			worker.onmessage = function (event) {
				if (GodotMedia.requests.get(id) !== r || r.state !== 0) return;
				const value = event.data;
				if (value['type'] === 'input_ack') { r.inputReady = true; return; }
				if (value['type'] === 'error') { GodotMedia.finish(id, value['error'], value['message']); return; }
				if (value['type'] !== 'pcm' || !value['dedicatedWorker'] || value['rate'] !== rate || !(value['pcm'] instanceof ArrayBuffer) || value['pcm'].byteLength > limit || value['pcm'].byteLength % 4 || value['frames'] !== value['pcm'].byteLength / 4 || value['frames'] <= 0) {
					GodotMedia.finish(id, 30, 'Invalid dedicated-worker PCM response.'); return;
				}
				r.data = new Uint8Array(value['pcm']); r.frames = value['frames']; r.title = GodotMedia.title(value['title']); r.state = 1;
				clearTimeout(r.timer); worker.terminate();
			};
			worker.postMessage({'type': 'start', 'url': GodotRuntime.parseString(url), 'rate': rate, 'limit': limit, 'timeout': timeout, 'headers': JSON.parse(GodotRuntime.parseString(headers)), 'decoderURL': GodotConfig.locate_file('godot.media.decoder.js')});
			return id;
		} catch (error) {
			const r = GodotMedia.requests.get(id);
			if (r) { r.worker.terminate(); clearTimeout(r.timer); }
			GodotMedia.requests.delete(id);
			return 0;
		}
	},
	godot_web_pcm_status__proxy: 'sync',
	godot_web_pcm_status__sig: 'iiii',
	godot_web_pcm_status: function (id, pointer, capacity) {
		const r = GodotMedia.requests.get(id);
		if (!r) return -2;
		const json = JSON.stringify({'error': r.error, 'message': r.message, 'title': r.title || '', 'rate': r.rate, 'frames': r.frames, 'bytes': r.data ? r.data.length : 0, 'input_ready': r.inputReady});
		GodotRuntime.stringToHeap(json, pointer, capacity);
		return r.state;
	},
	godot_web_pcm_input__proxy: 'sync',
	godot_web_pcm_input__sig: 'iiiii',
	godot_web_pcm_input: function (id, pointer, size, final) {
		const r = GodotMedia.requests.get(id);
		if (!r || r.state !== 0 || !r.inputReady || size < 0 || size > 262144) return 0;
		if (pointer < 0 || pointer + size > HEAPU8.byteLength) { GodotMedia.finish(id, 30, 'Invalid encoded input buffer.'); return 0; }
		let data;
		try { data = HEAPU8.slice(pointer, pointer + size); }
		catch (error) { GodotMedia.finish(id, error instanceof RangeError ? 6 : 1, 'Could not allocate the encoded input transfer buffer.'); return 0; }
		try {
			r.inputReady = false;
			r.worker.postMessage({'type': 'input', 'data': data.buffer, 'final': !!final}, [data.buffer]);
			return 1;
		} catch (error) { GodotMedia.finish(id, 1, 'Could not submit encoded input to the decoder worker.'); return 0; }
	},
	godot_web_pcm_read__proxy: 'sync',
	godot_web_pcm_read__sig: 'iiiii',
	godot_web_pcm_read: function (id, pointer, offset, size) {
		const r = GodotMedia.requests.get(id);
		if (!r || r.state !== 1 || offset < 0 || size < 0 || size > 262144 || offset + size > r.data.length) return 0;
		HEAPU8.set(r.data.subarray(offset, offset + size), pointer);
		return size;
	},
	godot_web_pcm_free__proxy: 'sync',
	godot_web_pcm_free__sig: 'vi',
	godot_web_pcm_free: function (id) {
		const r = GodotMedia.requests.get(id);
		if (!r) return;
		GodotMedia.requests.delete(id); clearTimeout(r.timer); r.worker.terminate(); r.data = null;
	},
};
autoAddDeps(GodotMedia, '$GodotMedia');
mergeInto(LibraryManager.library, GodotMedia);
