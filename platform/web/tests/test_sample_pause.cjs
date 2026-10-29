/**************************************************************************/
/*  test_sample_pause.cjs                                                */
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
const { test } = require('node:test');
const sourcePath = process.env.GODOT_AUDIO_TEST_SOURCE || path.resolve(__dirname, '../js/libs/library_godot_audio.js');
async function fixture(options = {}) {
	const sources = [];
	const worklet = {
		parameters: new Map([['reset', { setValueAtTime() {} }]]),
		port: { onmessage: null, messages: [], postMessage(value) {
			this.messages.push(value);
		} },
		disconnect() {},
	};
	const context = {
		LibraryManager: { library: {} }, autoAddDeps() {},
		mergeInto(destination, library) {
			Object.assign(destination, library);
		},
		GodotRuntime: { error(error) {
			throw error;
		}, parseString(value) {
			return value;
		} },
	};
	vm.createContext(context);
	vm.runInContext(fs.readFileSync(sourcePath, 'utf8'), context, { filename: sourcePath });
	const audio = context.LibraryManager.library.$GodotAudio;
	context.GodotAudio = audio;
	audio.ctx = {
		currentTime: 10, sampleRate: 48000,
		createBufferSource() {
			const source = {
				playbackRate: { value: 1 }, listeners: new Set(), connections: new Set(), starts: [], stops: 0, buffer: null,
				connect(node) {
					this.connections.add(node);
				},
				disconnect(node) {
					if (node) {
						this.connections.delete(node);
					} else {
						this.connections.clear();
					}
				},
				addEventListener(type, callback) {
					assert.equal(type, 'ended');
					this.listeners.add(callback);
				},
				removeEventListener(type, callback) {
					assert.equal(type, 'ended');
					this.listeners.delete(callback);
				},
				start(when, offset) {
					assert.equal(this.starts.length, 0);
					this.starts.push({ when, offset, rate: this.playbackRate.value });
				},
				stop() {
					this.stops++;
				},
				ended() {
					for (const callback of [...this.listeners]) {
						callback();
					}
				},
			};
			sources.push(source);
			return source;
		},
	};
	const sample = { sampleRate: 48000, loopMode: options.loopMode || 'disabled', getAudioBuffer() {
		return { duration: 1000 };
	} };
	audio.samples = new Map([['stream', sample]]);
	audio.sampleNodes = new Map();
	audio.audioPositionWorkletPromise = Promise.resolve();
	audio.audioPositionWorkletNodes = [worklet];
	audio.Bus.getBus = () => ({});
	const output = {};
	audio.SampleNodeBus.create = () => ({ getInputNode() {
		return output;
	}, setVolume() {}, clear() {} });
	let deleted = 0;
	audio.deleteSampleNode = () => {
		deleted++;
	};
	const node = new audio.SampleNode({ id: 'playback', streamObjectId: 'stream', busIndex: 0 },
		{ playbackRate: 1, offset: 0, ...options, start: false });
	audio.sampleNodes.set(node.id, node);
	await node.connectPositionWorklet(false);
	await Promise.resolve();
	return { node, sources, worklet, audio, bridge: context.LibraryManager.library,
		setTime(value) {
			audio.ctx.currentTime = value;
		}, get deleted() {
			return deleted;
		} };
}
function near(actual, expected) {
	assert.ok(Math.abs(actual - expected) < 1e-9, `expected ${expected}, got ${actual}`);
}
test('F17 repeated pauses retain content progress and exclude long paused intervals', async () => {
	const f = await fixture({ offset: 3 });
	f.node.start();
	f.setTime(12);
	f.node.pause();
	near(f.node.pauseTime, 2);
	f.setTime(112);
	f.node.pause(false);
	near(f.sources.at(-1).starts[0].offset, 5);
	f.setTime(115);
	f.node.pause();
	near(f.node.pauseTime, 5);
	f.setTime(1000);
	f.node.pause(false);
	near(f.sources.at(-1).starts[0].offset, 8);
	f.setTime(1004);
	f.node.pause();
	near(f.node.pauseTime, 9);
});
test('F17 resumed sources retain fractional playback rate and pitch', async () => {
	for (const rate of [0.5, 1, 2]) {
		for (const pitch of [0.5, 1, 2]) {
			// eslint-disable-next-line no-await-in-loop -- Exercise each independent rate pair through its full lifecycle.
			const f = await fixture({ playbackRate: rate, pitchScale: pitch, offset: 1.25 });
			f.node.start();
			f.setTime(12);
			f.node.pause();
			near(f.node.pauseTime, 2 * rate * pitch);
			f.setTime(100);
			f.node.pause(false);
			near(f.sources.at(-1).starts[0].offset, 1.25 + 2 * rate * pitch);
			near(f.sources.at(-1).playbackRate.value, rate * pitch);
			f.setTime(103);
			f.node.pause();
			near(f.node.pauseTime, 5 * rate * pitch);
		}
	}
});
test('F17 active rate and pitch changes settle time at the previously applied rate', async () => {
	const f = await fixture();
	f.node.start();
	f.setTime(12);
	f.node.setPitchScale(2);
	f.setTime(15);
	f.node.setPlaybackRate(0.5);
	f.setTime(19);
	f.node.pause();
	near(f.node.pauseTime, 12);
	f.setTime(100);
	f.node.pause(false);
	near(f.sources.at(-1).starts[0].offset, 12);
	near(f.sources.at(-1).playbackRate.value, 1);
});
test('F17 rate changes while paused do not accumulate paused wall time', async () => {
	const f = await fixture();
	f.node.start();
	f.setTime(12);
	f.node.pause();
	f.setTime(40);
	f.node.setPitchScale(2);
	f.setTime(70);
	f.node.setPlaybackRate(0.5);
	f.setTime(100);
	f.node.pause(false);
	near(f.sources.at(-1).starts[0].offset, 2);
	f.setTime(103);
	f.node.pause();
	near(f.node.pauseTime, 5);
});
test('F17 redundant pause and unpause preserve one source and its cursor', async () => {
	const f = await fixture();
	f.node.start();
	f.node.pause(false);
	assert.equal(f.sources.length, 1);
	f.setTime(12);
	f.node.pause();
	f.setTime(100);
	f.node.pause();
	near(f.node.pauseTime, 2);
	assert.equal(f.sources[0].stops, 1);
	f.node.pause(false);
	f.node.pause(false);
	assert.equal(f.sources.length, 2);
	near(f.sources[1].starts[0].offset, 2);
});
test('F17 scheduled sources exclude context time before their onset', async () => {
	const f = await fixture({ startTime: 20, offset: 3 });
	f.node.start();
	f.setTime(15);
	f.node.setPitchScale(2);
	f.setTime(16);
	f.node.pause();
	near(f.node.pauseTime, 0);
	f.setTime(18);
	f.node.pause(false);
	near(f.sources[1].starts[0].offset, 3);
	assert.equal(f.sources[1].starts[0].when, 20);
	f.setTime(22);
	f.node.pause();
	near(f.node.pauseTime, 4);
});
test('F17 pausing before asynchronous start resumes at the original offset', async () => {
	const f = await fixture({ offset: 3 });
	f.node.pause();
	f.node.start();
	assert.equal(f.sources[0].starts.length, 0);
	f.setTime(100);
	f.node.pause(false);
	assert.equal(f.sources.length, 1);
	near(f.sources[0].starts[0].offset, 3);
	f.setTime(102);
	f.node.pause();
	near(f.node.pauseTime, 2);
});
test('F17 new worklet counts remain relative to the resumed content offset', async () => {
	const f = await fixture({ offset: 3 });
	f.node.start();
	f.worklet.port.onmessage({ data: { type: 'position', data: '48000' } });
	near(f.node.getPlaybackPosition(), 4);
	f.setTime(12);
	f.node.pause();
	f.setTime(100);
	f.node.pause(false);
	near(f.node.getPlaybackPosition(), 5);
	f.worklet.port.onmessage({ data: { type: 'position', data: '96000' } });
	near(f.node.getPlaybackPosition(), 7);
	f.setTime(103);
	f.node.pause();
	f.worklet.port.onmessage({ data: { type: 'position', data: '999999' } });
	near(f.node.getPlaybackPosition(), 7);
});
test('F17 the ended event from a stopped source cannot stop the resumed source', async () => {
	const f = await fixture();
	f.node.start();
	f.setTime(12);
	f.node.pause();
	assert.equal(f.sources[0].listeners.size, 0);
	f.node.pause(false);
	f.sources[0].ended();
	assert.equal(f.deleted, 0);
	assert.equal(f.node.isCanceled, false);
	assert.equal(f.sources[1].listeners.size, 1);
	f.sources[1].ended();
	assert.equal(f.deleted, 1);
});
test('F17 explicit restart resets accumulated progress and retains pitch', async () => {
	const f = await fixture({ offset: 3, pitchScale: 2 });
	f.node.start();
	f.setTime(12);
	f.node.pause();
	f.setTime(100);
	f.node.pause(false);
	f.setTime(102);
	f.node.pause();
	f.node.restart();
	near(f.node.pauseTime, 0);
	near(f.sources.at(-1).starts[0].offset, 3);
	near(f.sources.at(-1).playbackRate.value, 2);
	f.setTime(104);
	f.node.pause();
	near(f.node.pauseTime, 4);
});
test('F17 natural loop restart preserves looping and original offset', async () => {
	const f = await fixture({ offset: 3, pitchScale: 0.5, loopMode: 'forward' });
	f.node.start();
	f.setTime(20);
	f.sources[0].ended();
	assert.equal(f.deleted, 0);
	assert.equal(f.sources.length, 2);
	near(f.sources[1].starts[0].offset, 3);
	near(f.sources[1].playbackRate.value, 0.5);
	f.setTime(22);
	f.node.pause();
	near(f.node.pauseTime, 1);
});
test('F17 the public sample pause bridge uses the same retained cursor', async () => {
	const f = await fixture();
	f.node.start();
	f.bridge.godot_audio_sample_set_pause('playback', 1);
	f.setTime(100);
	f.bridge.godot_audio_sample_set_pause('playback', 0);
	f.setTime(102);
	f.bridge.godot_audio_sample_set_pause('playback', 1);
	near(f.node.pauseTime, 2);
});
test('F17 default position starts at zero and unpause before pause is inert', async () => {
	const f = await fixture();
	near(f.node.getPlaybackPosition(), 0);
	f.node.pause(false);
	assert.equal(f.sources[0].starts.length, 0);
	f.node.start();
	assert.equal(f.sources[0].starts.length, 1);
});
