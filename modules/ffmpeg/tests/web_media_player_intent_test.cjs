/**************************************************************************/
/*  web_media_player_intent_test.cjs                                               */
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
const source = fs.readFileSync(process.argv[2] || path.join(__dirname, '../web/library_godot_media_player.js'), 'utf8');

function setup() {
	const pending = [];
	const element = { paused: true, play() { this.paused = false; return new Promise((resolve, reject) => pending.push({resolve, reject})); }, pause() { this.paused = true; }, removeAttribute() {}, load() {} };
	const context = { LibraryManager: {library: {}}, autoAddDeps() {}, mergeInto: Object.assign,
		GodotRuntime: {parseString: x => x}, GodotConfig: {canvas: {style: {}}}, clearTimeout() {}, console };
	vm.createContext(context);
	vm.runInContext(source + '\nglobalThis.GodotMediaPlayer = GodotMediaPlayerLibrary.$GodotMediaPlayer;', context);
	const state = context.GodotMediaPlayer, api = context.LibraryManager.library;
	const r = { id: 1, element, events: [], wantPlay: false, playIntent: 0, portal: false, buffering: true, options: {} };
	state.players.set(1, r);
	return {r, pending, state, element, command: action => api.godot_web_media_command(1, action, 0, 0)};
}
const settle = async () => { await Promise.resolve(); await Promise.resolve(); };
const error = (name, message = name) => Object.assign(new Error(message), {name});
const checks = [];
async function check(name, test) {
	try { await test(); checks.push({name, passed: true}); }
	catch (error) { checks.push({name, passed: false, error: String(error.stack || error)}); }
}
(async () => {
	await check('Aborted old play cannot fail pause/resume replacement', async () => {
		const t = setup(); t.command('play'); t.command('pause'); t.command('play');
		t.pending[0].reject(error('AbortError')); t.pending[1].resolve(); await settle();
		assert.equal(t.r.events.length, 0); assert.equal(t.r.wantPlay, true);
	});
	await check('Superseded refusal is ignored while current autoplay refusal is reported', async () => {
		const t = setup(); t.command('play'); t.command('pause'); t.command('play');
		t.pending[0].reject(error('NotAllowedError', 'old refusal')); await settle();
		assert.equal(t.r.events.length, 0);
		t.pending[1].reject(error('NotAllowedError', 'current refusal')); await settle();
		assert.equal(t.r.events.length, 1); assert.equal(t.r.events[0].message, 'current refusal'); assert.equal(t.r.events[0].error, 4);
	});
	await check('Pause cancellation remains quiet', async () => {
		const t = setup(); t.command('play'); t.command('pause'); t.pending[0].reject(error('AbortError')); await settle(); assert.equal(t.r.events.length, 0);
	});
	await check('Current unexpected abort remains an error', async () => {
		const t = setup(); t.command('play'); t.pending[0].reject(error('AbortError')); await settle(); assert.equal(t.r.events.length, 1);
	});
	await check('Replacement element rejects stale callbacks', async () => {
		const t = setup(); t.command('play'); t.r.element = {}; t.pending[0].reject(error('NotAllowedError')); await settle(); assert.equal(t.r.events.length, 0);
	});
	await check('Closed playback rejects stale callbacks', async () => {
		const t = setup(); t.command('play'); t.state.close(1); t.pending[0].reject(error('NotAllowedError')); await settle(); assert.equal(t.r.events.length, 0);
	});
	await check('Older duplicate play cannot overwrite current failure', async () => {
		const t = setup(); t.command('play'); t.command('play'); t.pending[1].reject(error('NotAllowedError', 'current refusal')); await settle();
		t.pending[0].reject(error('AbortError', 'old cancellation')); await settle(); assert.equal(t.r.events.length, 1); assert.equal(t.r.lastError, 'current refusal');
	});
	const report = {passed: checks.every(x => x.passed), scope: 'Controlled promise ordering on actual engine bridge; browser exports require separate qualification', checks};
	console.log(JSON.stringify(report, null, 2)); process.exitCode = report.passed ? 0 : 1;
})().catch(error => { console.error(error); process.exitCode = 1; });
