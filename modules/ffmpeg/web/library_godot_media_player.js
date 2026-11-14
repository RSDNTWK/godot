/**************************************************************************/
/*  library_godot_media_player.js                                              */
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

/* Browser backends for the engine media controller. Native transport is separate. */
const GodotMediaPlayerLibrary = {
	$GodotMediaPlayer__deps: ['$GodotRuntime', '$GodotConfig', '$GodotMedia', '$GodotOS'],
	$GodotMediaPlayer__postset: 'GodotOS.atexit(function(resolve, reject) { try { GodotMediaPlayer.freeFrameReader(); resolve(); } catch (error) { reject(error); } });',
	$GodotMediaPlayer: {
		next: 1,
		players: new Map(),
		scripts: new Map(),
		backdrop: null,
		canvasStyle: null,
		// A single page-lifetime scratch context/buffer bounds allocation churn
		// across all media players. It never accesses the main Godot GL context.
		frameReader: null,
		frameReaderDisabled: '',
		frameReaderFatal: false,
		frameReaderErrorCode: 1,
		releaseFrameReaderStorage: function () {
			const reader = GodotMediaPlayer.frameReader;
			if (!reader) return;
			reader.pixels = null; reader.dirty = false;
			const framebuffer = reader.framebuffer, texture = reader.texture;
			reader.framebuffer = null; reader.texture = null;
			let failed = false;
			try { if (framebuffer) reader.gl.deleteFramebuffer(framebuffer); } catch (_) { failed = true; }
			try { if (texture) reader.gl.deleteTexture(texture); } catch (_) { failed = true; }
			if (failed) { try { GodotRuntime.error('Browser video scratch storage cleanup failed.'); } catch (_) { } }
		},
		reportFrameReaderFailure: function (r) {
			r.frameTransfer = 'webgl_reader_error'; r.frameTransferReason = GodotMediaPlayer.frameReaderDisabled;
			GodotMediaPlayer.error(r, 'Browser video frame reader failed: ' + GodotMediaPlayer.frameReaderDisabled, GodotMediaPlayer.frameReaderErrorCode);
		},
		failFrameReader: function (reason, outOfMemory) {
			GodotMediaPlayer.frameReaderDisabled = reason;
			GodotMediaPlayer.frameReaderFatal = true;
			if (outOfMemory) GodotMediaPlayer.frameReaderErrorCode = 6;
			GodotMediaPlayer.releaseFrameReaderStorage();
			for (const player of GodotMediaPlayer.players.values()) {
				if (player.video && player.element && !player.portal && !player.opaque) GodotMediaPlayer.reportFrameReaderFailure(player);
			}
			return -2;
		},
		frameReaderContextLost: function () {
			const reader = GodotMediaPlayer.frameReader;
			if (reader && reader.canvas === this) {
				GodotMediaPlayer.failFrameReader('private_context_lost', false);
			}
		},
		freeFrameReader: function () {
			const reader = GodotMediaPlayer.frameReader;
			GodotMediaPlayer.frameReader = null;
			GodotMediaPlayer.frameReaderDisabled = 'shutdown';
			GodotMediaPlayer.frameReaderFatal = true;
			if (!reader) return;
			reader.canvas.removeEventListener('webglcontextlost', GodotMediaPlayer.frameReaderContextLost);
			reader.pixels = null;
			let failure = null;
			const attempt = function (operation) { try { operation(); } catch (error) { if (!failure) failure = error; } };
			if (reader.framebuffer) attempt(function () { reader.gl.deleteFramebuffer(reader.framebuffer); });
			if (reader.texture) attempt(function () { reader.gl.deleteTexture(reader.texture); });
			attempt(function () { const extension = reader.gl.getExtension('WEBGL_lose_context'); if (extension) extension.loseContext(); });
			reader.canvas.width = 0; reader.canvas.height = 0;
			if (failure) throw failure;
		},
		clearFrameReaderIfIdle: function () {
			const reader = GodotMediaPlayer.frameReader;
			if (!reader || !reader.dirty) return;
			try {
				for (const player of GodotMediaPlayer.players.values()) {
					if (player.video && player.element && !player.portal && !player.opaque) return;
				}
				if (reader.pixels) reader.pixels.fill(0);
				if (!reader.gl.isContextLost() && reader.framebuffer) {
					reader.gl.bindFramebuffer(reader.gl.FRAMEBUFFER, reader.framebuffer);
					reader.gl.clearColor(0, 0, 0, 0);
					reader.gl.clear(reader.gl.COLOR_BUFFER_BIT);
				}
				reader.dirty = false;
			} catch (_) {
				GodotMediaPlayer.failFrameReader('scratch_idle_clear_exception', false);
				try { GodotRuntime.error('Browser video scratch idle clear failed.'); } catch (_) { }
			}
		},
		readFrameReusable: function (r, pointer, capacity, width, height) {
			const fallback = function (reason) {
				r.frameTransfer = 'cpu_canvas_readback'; r.frameTransferReason = reason; return -1;
			};
			const failed = function (reason, outOfMemory) {
				return GodotMediaPlayer.failFrameReader(reason, outOfMemory);
			};
			if (GodotMediaPlayer.frameReaderDisabled) {
				if (GodotMediaPlayer.frameReaderFatal) { GodotMediaPlayer.reportFrameReaderFailure(r); return -2; }
				return fallback(GodotMediaPlayer.frameReaderDisabled);
			}
			if (r.frameReaderUnsupported) return fallback(r.frameReaderUnsupported);
			if (r.element.seeking || r.element.videoWidth !== width || r.element.videoHeight !== height) return 0;
			let reader = GodotMediaPlayer.frameReader;
			if (!reader) {
				try {
					const canvas = document.createElement('canvas'); canvas.width = 1; canvas.height = 1;
					const gl = canvas.getContext('webgl2', {'alpha': false, 'depth': false, 'stencil': false, 'antialias': false, 'premultipliedAlpha': false});
					if (!gl || gl.canvas !== canvas || canvas === GodotConfig.canvas) {
						GodotMediaPlayer.frameReaderDisabled = 'private_webgl2_unavailable';
						return fallback(GodotMediaPlayer.frameReaderDisabled);
					}
					reader = {canvas, gl, texture: null, framebuffer: null, pixels: null, dirty: false};
					GodotMediaPlayer.frameReader = reader;
					canvas.addEventListener('webglcontextlost', GodotMediaPlayer.frameReaderContextLost);
					reader.texture = gl.createTexture(); reader.framebuffer = gl.createFramebuffer();
					if (!reader.texture || !reader.framebuffer) return failed('scratch_object_allocation', true);
					gl.bindTexture(gl.TEXTURE_2D, reader.texture);
					gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, 1920, 1080);
					gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
					gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
					gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
					gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
					gl.bindFramebuffer(gl.FRAMEBUFFER, reader.framebuffer);
					gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, reader.texture, 0);
					const allocationError = gl.getError();
					if (allocationError !== gl.NO_ERROR) return failed('scratch_storage_gl_' + allocationError, allocationError === gl.OUT_OF_MEMORY);
					if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) return failed('scratch_framebuffer_incomplete', false);
					gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
					gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
					gl.pixelStorei(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL, gl.BROWSER_DEFAULT_WEBGL);
					if ('unpackColorSpace' in gl) gl.unpackColorSpace = 'srgb';
					gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
					reader.pixels = new Uint8Array(1920 * 1080 * 4);
				} catch (error) {
					return failed(error instanceof RangeError ? 'scratch_pixel_allocation' : 'scratch_initialization_exception', error instanceof RangeError);
				}
			}
			const gl = reader.gl;
			if (gl.isContextLost()) return failed('private_context_lost', false);
			try {
				gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, gl.RGBA, gl.UNSIGNED_BYTE, r.element);
				reader.dirty = true;
				const uploadError = gl.getError();
				if (uploadError !== gl.NO_ERROR) return failed('video_upload_gl_' + uploadError, uploadError === gl.OUT_OF_MEMORY);
				gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, reader.pixels);
				const readError = gl.getError();
				if (readError !== gl.NO_ERROR || gl.isContextLost()) return failed('pixel_read_gl_' + readError, readError === gl.OUT_OF_MEMORY);
				HEAPU8.set(reader.pixels.subarray(0, capacity), pointer);
				r.frameTransfer = 'webgl_reusable_readback'; r.frameTransferReason = '';
				return 1;
			} catch (error) {
				// Keep existing origin/security fallback behavior. Unsupported DOM
				// upload may use the legacy path, but never masquerades as this fix.
				if (error && (error.name === 'SecurityError' || error.name === 'TypeError')) {
					r.frameReaderUnsupported = 'private_upload_' + error.name;
					return fallback(r.frameReaderUnsupported);
				}
				return failed('frame_transfer_exception', error instanceof RangeError);
			}
		},
		loadScript: function (url, ready) {
			if (ready()) return Promise.resolve();
			if (!GodotMediaPlayer.scripts.has(url)) {
				GodotMediaPlayer.scripts.set(url, new Promise(function (resolve, reject) {
					const script = document.createElement('script');
					script.src = url; script.async = true;
					script.onload = function () { resolve(); };
					script.onerror = function () { GodotMediaPlayer.scripts.delete(url); script.remove(); reject(new Error('Browser media runtime could not load.')); };
					document.head.appendChild(script);
				}));
			}
			return GodotMediaPlayer.scripts.get(url);
		},
		alive: function (r) { return GodotMediaPlayer.players.get(r.id) === r; },
		close: function (id) {
			const r = GodotMediaPlayer.players.get(id);
			if (!r) return;
			GodotMediaPlayer.players.delete(id); clearTimeout(r.timer); clearTimeout(r.ytWait);
			GodotMediaPlayer.clearFrameReaderIfIdle();
			if (r.hls) r.hls['destroy']();
			if (r.yt) r.yt['destroy']();
			if (r.element) { r.element.pause(); r.element.removeAttribute('src'); r.element.load(); }
			if (r.holder) r.holder.remove();
			if (![...GodotMediaPlayer.players.values()].some(function (player) { return !!player.holder; })) {
				if (GodotMediaPlayer.backdrop) GodotMediaPlayer.backdrop.remove();
				GodotMediaPlayer.backdrop = null;
				if (GodotMediaPlayer.canvasStyle) {
					GodotConfig.canvas.style.position = GodotMediaPlayer.canvasStyle.position;
					GodotConfig.canvas.style.zIndex = GodotMediaPlayer.canvasStyle.zIndex;
					GodotMediaPlayer.canvasStyle = null;
				}
			}
		},
		push: function (r, event) {
			if (GodotMediaPlayer.alive(r) && r.events.length < 8) r.events.push(event);
		},
		error: function (r, message, code) {
			const text = String(message).slice(0, 512);
			if (r.lastError !== text) GodotMediaPlayer.push(r, {'type': 'error', 'error': code || 25, 'message': text});
			r.lastError = text;
			r.buffering = false;
		},
		youtubeId: function (url) {
			const host = url.hostname.toLowerCase();
			if (!(host === 'youtu.be' || host === 'youtube.com' || host.endsWith('.youtube.com') || host === 'youtube-nocookie.com' || host.endsWith('.youtube-nocookie.com'))) return null;
			const parts = url.pathname.split('/').filter(Boolean);
			const id = host === 'youtu.be' ? parts[0] : url.searchParams.get('v') || (['embed', 'shorts', 'live'].includes(parts[0]) ? parts[1] : '');
			if (!/^[a-zA-Z0-9_-]{11}$/.test(id || '')) throw new Error('A YouTube video URL is required; playlist expansion is not provided.');
			return id;
		},
		mount: function (r, element) {
			const canvas = GodotConfig.canvas;
			const parent = canvas.parentNode;
			if (!GodotMediaPlayer.backdrop) {
				const background = document.createElement('div');
				background.style.cssText = 'position:fixed;pointer-events:none;z-index:0;background:#' + r.options['canvas_background'];
				parent.insertBefore(background, canvas);
				GodotMediaPlayer.backdrop = background;
				GodotMediaPlayer.canvasStyle = {position: canvas.style.position, zIndex: canvas.style.zIndex};
			}
			const holder = document.createElement('div');
			holder.style.cssText = 'position:fixed;overflow:hidden;pointer-events:none;background:black;display:none;z-index:0';
			element.style.cssText = 'display:block;width:100%;height:100%;border:0;object-fit:contain';
			holder.appendChild(element); parent.insertBefore(holder, canvas);
			r.holder = holder;
			// Keep ordinary Godot controls above the DOM fallback. A canvas shader
			// punches a transparent hole only at this player's draw order.
			if (getComputedStyle(canvas).position === 'static') canvas.style.position = 'relative';
			if (getComputedStyle(canvas).zIndex === 'auto') canvas.style.zIndex = '1';
		},
		play: function (r) {
			if (!GodotMediaPlayer.alive(r)) return;
			if (r.portal) { if (r.yt && r.apiReady) r.yt['playVideo'](); r.wantPlay = true; return; }
			const element = r.element;
			if (!element) return;
			const pending = element.play();
			if (pending && pending.catch) pending.catch(function (error) {
				if (GodotMediaPlayer.alive(r) && r.element === element && !(error.name === 'AbortError' && !r.wantPlay)) GodotMediaPlayer.error(r, error.message || 'Browser requires a playback gesture.', 4);
			});
		},
		direct: function (r, url) {
			const element = document.createElement(r.video ? 'video' : 'audio');
			r.element = element;
			element.preload = r.video ? 'auto' : 'metadata'; element.playsInline = true; element.volume = r.volume;
			if (!r.opaque) element.crossOrigin = 'anonymous';
			if (r.video) GodotMediaPlayer.mount(r, element);
			const active = function () { return GodotMediaPlayer.alive(r) && r.element === element; };
			element.addEventListener('loadedmetadata', function () { if (active()) r.metadataReady = true; });
			element.addEventListener('loadeddata', function () { if (active()) r.frameTime = -1; });
			element.addEventListener('canplay', function () { if (active()) r.buffering = false; });
			element.addEventListener('playing', function () { if (active()) { if (!r.wantPlay) element.pause(); r.buffering = false; r.lastError = ''; } });
			element.addEventListener('waiting', function () { if (active()) r.buffering = true; });
			element.addEventListener('stalled', function () { if (active() && element.readyState < 3) r.buffering = true; });
			element.addEventListener('ended', function () { if (active()) GodotMediaPlayer.push(r, {'type': 'finished'}); });
			element.addEventListener('seeked', function () {
				if (active()) r.frameTime = -1;
				if (!active() || !r.seeking || element.seeking) return;
				GodotMediaPlayer.push(r, {'type': 'seek', 'request': r.seeking.id, 'position': element.currentTime});
				r.seeking = null;
			});
			element.addEventListener('error', function () {
				if (!active()) return;
				// A readable fetch/texture is optional. Preserve ordinary browser
				// playback when the server permits media embedding but denies CORS.
				if (!r.opaque && r.allowFallback && !r.hls) {
					element.pause(); element.removeAttribute('src'); element.load();
					if (r.holder) { r.holder.remove(); r.holder = null; }
					r.opaque = true;
					GodotMediaPlayer.direct(r, url);
					return;
				}
				GodotMediaPlayer.error(r, 'Browser cannot play this source (network, embedding policy or codec).');
			});
			const isHLS = /\.m3u8(?:$|[?#])/i.test(url) || r.options['format'] === 'hls';
			if (isHLS) {
				GodotMediaPlayer.loadScript(GodotConfig.locate_file('godot.media.hls.js'), function () { return !!window['Hls']; }).then(function () {
					if (!active()) return;
					const Hls = window['Hls'];
					if (!Hls || !Hls['isSupported']()) {
						if (element.canPlayType('application/vnd.apple.mpegurl')) { element.src = url; element.load(); if (r.wantPlay) GodotMediaPlayer.play(r); return; }
						throw new Error('This browser has no supported HLS backend.');
					}
					const hls = new Hls({'enableWorker': true, 'maxBufferLength': 20, 'maxMaxBufferLength': 30, 'backBufferLength': 10, 'maxBufferSize': 32 * 1024 * 1024, 'capLevelToPlayerSize': true, 'maxLoadingDelay': 10});
					r.hls = hls;
					hls['on'](Hls['Events']['MANIFEST_PARSED'], function (_, data) {
						if (!active()) return;
						let cap = -1;
						for (let i = 0; i < data['levels'].length; i++) if ((data['levels'][i]['height'] || 0) <= 1080) cap = i;
						if (cap >= 0) hls['autoLevelCapping'] = cap;
					});
					hls['on'](Hls['Events']['LEVEL_LOADED'], function (_, data) {
						if (active()) { r.live = !!data['details']['live']; r.liveKnown = true; }
					});
					hls['on'](Hls['Events']['LEVEL_UPDATED'], function (_, data) {
						if (!active()) return;
						const details = data['details'], fragments = details['fragments'];
						if (details['live'] && fragments.length) {
							const first = fragments[0], last = fragments[fragments.length - 1];
							r.window = {'start': first['start'], 'end': last['start'] + last['duration']};
						} else r.window = null;
					});
					hls['on'](Hls['Events']['ERROR'], function (_, data) { if (active() && data['fatal']) GodotMediaPlayer.error(r, 'HLS playback failed: ' + data['details']); });
					hls['loadSource'](url); hls['attachMedia'](element);
					if (r.wantPlay) GodotMediaPlayer.play(r);
				}).catch(function (error) { if (active()) GodotMediaPlayer.error(r, error.message); });
			} else {
				element.src = url; element.load(); if (r.wantPlay) GodotMediaPlayer.play(r);
			}
		},
		youtube: function (r, id) {
			r.portal = true;
			r.youtubeId = id;
			const iframe = document.createElement('iframe');
			iframe.setAttribute('credentialless', '');
			iframe.referrerPolicy = 'strict-origin-when-cross-origin';
			iframe.allow = 'autoplay; encrypted-media; picture-in-picture';
			const showControls = r.options['youtube_show_controls'] !== false;
			iframe.src = 'https://www.youtube-nocookie.com/embed/' + id + '?enablejsapi=1&playsinline=1&origin=' + encodeURIComponent(location.origin)
				+ '&controls=' + (showControls ? '1' : '0') + '&disablekb=' + (showControls ? '0' : '1') + '&fs=' + (showControls ? '1' : '0');
			GodotMediaPlayer.mount(r, iframe);
			GodotMediaPlayer.loadScript('https://www.youtube.com/iframe_api', function () { return window['YT'] && window['YT']['Player']; }).then(function () {
				const deadline = performance.now() + 15000;
				const wait = function () {
					if (!GodotMediaPlayer.alive(r)) return;
					if (!(window['YT'] && window['YT']['Player'])) {
						if (performance.now() < deadline) { r.ytWait = setTimeout(wait, 50); return; }
						GodotMediaPlayer.error(r, 'YouTube iframe API did not initialize.'); return;
					}
					r.yt = new window['YT']['Player'](iframe, {'events': {
						'onReady': function () {
							if (!GodotMediaPlayer.alive(r)) return;
							r.apiReady = true; r.buffering = false;
							r.yt['setVolume'](r.volume * 100);
							if (r.wantPlay) GodotMediaPlayer.play(r);
						},
						'onStateChange': function (event) {
							if (!GodotMediaPlayer.alive(r)) return;
							if (event['data'] === 1 && !r.wantPlay) r.yt['pauseVideo']();
							r.buffering = event['data'] === 3;
							if (event['data'] === 0) GodotMediaPlayer.push(r, {'type': 'finished'});
						},
						'onError': function (event) { if (GodotMediaPlayer.alive(r)) GodotMediaPlayer.error(r, 'YouTube iframe error ' + event['data']); },
						'onAutoplayBlocked': function () { if (GodotMediaPlayer.alive(r)) GodotMediaPlayer.error(r, 'YouTube requires a playback gesture.', 4); },
					}});
				}; wait();
			}).catch(function (error) { if (GodotMediaPlayer.alive(r)) GodotMediaPlayer.error(r, error.message); });
		},
		status: function (r) {
			let position = 0, duration = 0, ranges = [], width = 0, height = 0, title = r.title;
			let canSeek = false, seekLoop = false, live = r.live, dvr = false;
			if (r.yt && r.apiReady) {
				position = r.yt['getCurrentTime']() || 0;
				// Cues briefly clear metadata for the same video. Preserve its known range.
				const reportedDuration = r.yt['getDuration']();
				if (Number.isFinite(reportedDuration) && reportedDuration > 0) r.youtubeDuration = reportedDuration;
				duration = r.youtubeDuration || 0;
				// The documented iframe API has no advertised DVR TimeRanges.
				// A finite playlist can explicitly identify its timeline. Unknown
				// timelines never claim an invented live rewind window.
				const data = (r.yt['getVideoData'] ? r.yt['getVideoData']() : {}) || {};
				title = GodotMedia.title(data['title'] || title);
				r.title = title;
				if (typeof data['isLive'] === 'boolean' && !r.options['timeline_mode']) { live = r.live = data['isLive']; r.liveKnown = true; }
				canSeek = r.liveKnown && !live && duration > 0;
				if (duration > 0 && !r.ready) { r.ready = true; clearTimeout(r.timer); }
				seekLoop = canSeek;
				if (canSeek) ranges.push({'start': 0, 'end': duration});
				width = 16; height = 9;
				const state = r.yt['getPlayerState']();
				const distance = r.seeking ? Math.abs(position - r.seeking.target) : Infinity;
				// Cued preparation reports its requested start time without decoding
				// or starting playback. Normal seeks retain their existing tolerance.
				const seek_complete = r.seeking && (r.seeking.cued
					? (state === 5 && distance < 0.001) || ([0, 1, 2].includes(state) && distance < 0.5)
					: state !== 3 && distance < 0.5);
				if (seek_complete) {
					GodotMediaPlayer.push(r, {'type': 'seek', 'request': r.seeking.id, 'position': position}); r.seeking = null;
				}
			} else if (r.element) {
				const media = r.element;
				position = media.currentTime || 0;
				duration = Number.isFinite(media.duration) ? media.duration : 0;
				if (!r.liveKnown && media.duration === Infinity) { live = r.live = true; r.liveKnown = true; }
				for (let i = 0; i < Math.min(media.seekable.length, 16); i++) {
					// MSE can retain already expired media in its local buffer. A live
					// loop must also fit the current server-advertised manifest window.
					const begin = Math.max(media.seekable.start(i), r.window ? r.window['start'] : -Infinity);
					const end = Math.min(media.seekable.end(i), r.window ? r.window['end'] : Infinity);
					if (end > begin) ranges.push({'start': begin, 'end': end});
				}
				canSeek = ranges.length > 0; seekLoop = canSeek; dvr = live && canSeek;
				if (r.metadataReady && (canSeek || media.readyState >= 2) && !r.ready) { r.ready = true; clearTimeout(r.timer); }
				width = media.videoWidth || 0; height = media.videoHeight || 0;
			}
			if (r.seeking && performance.now() > r.seeking.deadline) { GodotMediaPlayer.error(r, 'Browser seek did not complete.', 24); r.seeking = null; }
			if (r.video && (width > 1920 || height > 1080)) r.opaque = true;
			const events = r.events.splice(0, 8);
			title = GodotMedia.title(title);
			if (r.hls && r.hls['playingDate'] instanceof Date) {
				const origin = Math.round((r.hls['playingDate'].getTime() / 1000 - position) * 1000) / 1000;
				for (const range of ranges) range['timeline_origin_utc'] = origin;
			}
			return {'ready': r.ready, 'buffering': r.buffering, 'position': position, 'duration': duration, 'width': width, 'height': height, 'portal': r.video && (r.portal || r.opaque), 'ranges': ranges, 'events': events,
				'metadata': {'title': title, 'title_source': r.yt ? 'youtube_iframe' : 'url', 'source_type': r.portal ? 'youtube' : 'direct', 'duration': duration, 'is_live': live, 'live_status_known': r.liveKnown, 'opened': r.ready},
				'capabilities': {'backend': r.yt || r.portal ? 'youtube_iframe' : r.hls ? 'browser_hls' : 'browser_media', 'precise_loop': false, 'seek_loop': seekLoop, 'seek': canSeek, 'live_playback': live, 'dvr': dvr, 'pcm_decode': !r.portal && !r.opaque && !live, 'volume': true, 'godot_audio_bus': false, 'native_destination_validation': false, 'network_policy': 'browser', 'buffer_policy': r.hls ? 'bounded_hls_targets' : 'browser_managed', 'buffer_hard_limit': false, 'max_buffer_target_bytes': r.hls ? 33554432 : 0, 'video_frame_transfer': r.frameTransfer || 'pending', 'video_frame_transfer_reason': r.frameTransferReason || '', 'texture_rendering': !r.portal && !r.opaque, 'dom_rendering': r.portal || (r.video && r.opaque), 'dom_axis_aligned_only': r.portal || (r.video && r.opaque), 'dom_subviewport': false, 'live_status_known': r.liveKnown, 'iframe_dvr_ranges': false}};
		},
	},
	godot_web_media_open__proxy: 'sync',
	godot_web_media_open__sig: 'iii',
	godot_web_media_open: function (source, options) {
		if (GodotMediaPlayer.players.size >= 8) return 0;
		let allocated = 0;
		try {
			const text = GodotRuntime.parseString(source), url = new URL(text), config = JSON.parse(GodotRuntime.parseString(options));
			if (!['https:', 'http:'].includes(url.protocol) || url.username || url.password) return 0;
			const id = GodotMediaPlayer.next++;
			let filename = url.pathname.split('/').pop() || url.hostname;
			try { filename = decodeURIComponent(filename); } catch (error) { /* Metadata must not reject a playable URL. */ }
			const r = {id, source: text, options: config, ready: false, apiReady: false, buffering: true, events: [], video: !!config['video'], wantPlay: !!config['autoplay'], volume: config['volume'], allowFallback: config['allow_browser_fallback'] !== false, opaque: false, portal: false, element: null, yt: null, holder: null, seeking: null, hls: null, window: null, timer: null, ytWait: null, live: config['timeline_mode'] === 'live', liveKnown: ['live', 'finite'].includes(config['timeline_mode']), title: GodotMedia.title(filename), frameCanvas: null, frameContext: null, frameTime: -1};
			GodotMediaPlayer.players.set(id, r);
			allocated = id;
			r.timer = setTimeout(function () { if (!r.ready && GodotMediaPlayer.alive(r)) GodotMediaPlayer.error(r, 'Browser media opening timed out.', 24); }, Math.min(60000, Math.max(1, Number(config['timeout_msec']) || 30000)));
			const youtube = GodotMediaPlayer.youtubeId(url);
			if (youtube) GodotMediaPlayer.youtube(r, youtube); else GodotMediaPlayer.direct(r, text);
			return id;
		} catch (error) { if (allocated) GodotMediaPlayer.close(allocated); return 0; }
	},
	godot_web_media_poll__proxy: 'sync',
	godot_web_media_poll__sig: 'iiii',
	godot_web_media_poll: function (id, pointer, capacity) {
		const r = GodotMediaPlayer.players.get(id);
		if (!r) return 0;
		const json = JSON.stringify(GodotMediaPlayer.status(r));
		if (lengthBytesUTF8(json) >= capacity) return 0;
		GodotRuntime.stringToHeap(json, pointer, capacity); return 1;
	},
	godot_web_media_command__proxy: 'sync',
	godot_web_media_command__sig: 'viidi',
	godot_web_media_command: function (id, command, value, request) {
		const r = GodotMediaPlayer.players.get(id);
		if (!r) return;
		const action = GodotRuntime.parseString(command);
		try {
			if (action === 'play') {
				// A new play intent must report a fresh browser refusal after a pause.
				if (!r.wantPlay) r.lastError = '';
				r.wantPlay = true;
				GodotMediaPlayer.play(r);
			}
			else if (action === 'pause') { r.wantPlay = false; if (r.yt && r.apiReady) r.yt['pauseVideo'](); else if (r.element) r.element.pause(); }
			else if (action === 'volume') { r.volume = Math.min(1, Math.max(0, value)); if (r.yt && r.apiReady) r.yt['setVolume'](r.volume * 100); else if (r.element) r.element.volume = r.volume; }
			else if (action === 'seek') {
				r.seeking = {id: request, target: value, deadline: performance.now() + 15000};
				if (r.yt && r.ready) {
					const state = r.yt['getPlayerState']();
					if (!r.wantPlay && (state === -1 || state === 5)) {
						// seekTo during canceled startup can leave an unbuffered iframe
						// unstarted. Prepare the paused start time without forcing play.
						r.seeking.cued = true;
						r.yt['cueVideoById']({'videoId': r.youtubeId, 'startSeconds': value});
					} else {
						r.yt['seekTo'](value, true);
						if (r.wantPlay) GodotMediaPlayer.play(r);
					}
				}
				else if (r.element) {
					if (Math.abs(r.element.currentTime - value) < 0.001 && !r.element.seeking) { GodotMediaPlayer.push(r, {'type': 'seek', 'request': request, 'position': value}); r.seeking = null; }
					else r.element.currentTime = value;
					if (r.wantPlay && r.element.paused) GodotMediaPlayer.play(r);
				}
			}
		} catch (error) { GodotMediaPlayer.error(r, error.message); }
	},
	godot_web_media_rect__proxy: 'sync',
	godot_web_media_rect__sig: 'viddddi',
	godot_web_media_rect: function (id, x, y, width, height, visible) {
		const r = GodotMediaPlayer.players.get(id);
		if (!r || !r.holder) return;
		const canvas = GodotConfig.canvas, rect = canvas.getBoundingClientRect();
		const background = GodotMediaPlayer.backdrop;
		if (background) { background.style.left = rect.left + 'px'; background.style.top = rect.top + 'px'; background.style.width = rect.width + 'px'; background.style.height = rect.height + 'px'; }
		const portal = r.video && (r.portal || r.opaque);
		r.holder.style.display = visible && portal ? 'block' : 'none';
		r.holder.style.left = (rect.left + x * rect.width / canvas.width) + 'px';
		r.holder.style.top = (rect.top + y * rect.height / canvas.height) + 'px';
		r.holder.style.width = (width * rect.width / canvas.width) + 'px';
		r.holder.style.height = (height * rect.height / canvas.height) + 'px';
	},
	godot_web_media_frame__proxy: 'sync',
	godot_web_media_frame__sig: 'iiiiii',
	godot_web_media_frame: function (id, pointer, capacity, width, height) {
		const r = GodotMediaPlayer.players.get(id);
		if (!r || !r.element || !r.video || r.opaque || r.element.readyState < 2 ||
				!Number.isInteger(width) || !Number.isInteger(height) || width <= 0 || height <= 0 ||
				width > 1920 || height > 1080 || capacity !== width * height * 4) return 0;
		if (!Number.isInteger(pointer) || pointer < 0 || pointer + capacity > HEAPU8.length) {
			GodotMediaPlayer.error(r, 'Invalid video frame destination.', 31); return 0;
		}
		if (GodotMediaPlayer.frameReaderFatal) { GodotMediaPlayer.reportFrameReaderFailure(r); return 0; }
		if (r.frameTime === r.element.currentTime && r.frameWidth === width && r.frameHeight === height) return 0;
		try {
			const transfer = GodotMediaPlayer.readFrameReusable(r, pointer, capacity, width, height);
			if (transfer === -2 || transfer === 0) return 0;
			let data;
			if (transfer === 1) {
				data = HEAPU8.subarray(pointer, pointer + capacity);
			} else {
				if (!r.frameCanvas) { r.frameCanvas = document.createElement('canvas'); r.frameContext = r.frameCanvas.getContext('2d', {'willReadFrequently': true}); }
				r.frameCanvas.width = width; r.frameCanvas.height = height;
				r.frameContext.drawImage(r.element, 0, 0, width, height);
				data = r.frameContext.getImageData(0, 0, width, height).data;
			}
			// Retain the first-drawable-frame guard, including paused startup.
			if (r.frameTime < 0) {
				let drawable = false;
				for (let i = 3; i < data.length; i += 4) { if (data[i]) { drawable = true; break; } }
				if (!drawable) return 0;
			}
			if (transfer !== 1) HEAPU8.set(data, pointer);
			r.frameTime = r.element.currentTime; r.frameWidth = width; r.frameHeight = height;
			return 1;
		} catch (error) {
			r.opaque = true; GodotMediaPlayer.clearFrameReaderIfIdle(); return 0;
		}
	},
	godot_web_media_close__proxy: 'sync',
	godot_web_media_close__sig: 'vi',
	godot_web_media_close: function (id) {
		GodotMediaPlayer.close(id);
	},
};
autoAddDeps(GodotMediaPlayerLibrary, '$GodotMediaPlayer');
mergeInto(LibraryManager.library, GodotMediaPlayerLibrary);
