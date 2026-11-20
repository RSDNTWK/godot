const Preloader = /** @constructor */ function () { // eslint-disable-line no-unused-vars
	const HEADER_TIMEOUT_MS = 30000;
	const STALL_TIMEOUT_MS = 30000;

	function withDeadline(operation, timeout, connection, message) {
		let timer;
		const expired = new Promise((resolve, reject) => {
			timer = setTimeout(() => {
				reject(new Error(message));
				connection.abort();
			}, timeout);
		});
		return Promise.race([operation, expired]).finally(() => clearTimeout(timer));
	}

	function getTrackedResponse(response, load_status, raw, connection, file) {
		if (!response.body) {
			throw new Error('Response body missing for ' + response.url);
		}
		const reader = response.body.getReader();
		return new Response(new ReadableStream({
			pull: function (controller) {
				return withDeadline(reader.read(), STALL_TIMEOUT_MS, connection, `Download stalled for '${file}'.`).then(function (result) {
					if (load_status.done) {
						return;
					}
					if (result.done) {
						load_status.done = true;
						controller.close();
					} else {
						controller.enqueue(result.value);
						load_status.loaded += result.value.length;
					}
				}, function (error) {
					if (raw) {
						load_status.done = true;
					}
					throw error;
				});
			},
			cancel: function (reason) {
				load_status.done = true;
				return reader.cancel(reason);
			},
		}), { headers: response.headers });
	}

	function loadFetch(file, tracker, fileSize, raw) {
		tracker[file] = {
			total: fileSize || 0,
			loaded: 0,
			done: false,
		};
		const connection = new AbortController();
		const request = fetch(file, { signal: connection.signal });
		return withDeadline(request, HEADER_TIMEOUT_MS, connection, `Timed out waiting for '${file}'.`).then(function (response) {
			if (!response.ok) {
				connection.abort();
				return Promise.reject(new Error(`Failed loading file '${file}'`));
			}
			const tr = getTrackedResponse(response, tracker[file], raw, connection, file);
			if (raw) {
				return Promise.resolve(tr);
			}
			return tr.arrayBuffer();
		});
	}

	function retry(func, attempts = 1) {
		function onerror(err) {
			if (attempts <= 1) {
				return Promise.reject(err);
			}
			return new Promise(function (resolve, reject) {
				setTimeout(function () {
					retry(func, attempts - 1).then(resolve).catch(reject);
				}, 1000);
			});
		}
		return func().catch(onerror);
	}

	const DOWNLOAD_ATTEMPTS_MAX = 4;
	const loadingFiles = {};
	const lastProgress = { loaded: 0, total: 0 };
	let progressFunc = null;

	const animateProgress = function () {
		let loaded = 0;
		let total = 0;
		let totalIsValid = true;
		let progressIsFinal = true;

		Object.keys(loadingFiles).forEach(function (file) {
			const stat = loadingFiles[file];
			if (!stat.done) {
				progressIsFinal = false;
			}
			if (!totalIsValid || stat.total === 0) {
				totalIsValid = false;
				total = 0;
			} else {
				total += stat.total;
			}
			loaded += stat.loaded;
		});
		if (loaded !== lastProgress.loaded || total !== lastProgress.total) {
			lastProgress.loaded = loaded;
			lastProgress.total = total;
			if (typeof progressFunc === 'function') {
				progressFunc(loaded, total);
			}
		}
		if (!progressIsFinal) {
			requestAnimationFrame(animateProgress);
		}
	};

	this.animateProgress = animateProgress;

	this.setProgressFunc = function (callback) {
		progressFunc = callback;
	};

	this.loadPromise = function (file, fileSize, raw = false) {
		return retry(loadFetch.bind(null, file, loadingFiles, fileSize, raw), DOWNLOAD_ATTEMPTS_MAX).catch(function (error) {
			loadingFiles[file].done = true;
			throw error;
		});
	};

	this.preloadedFiles = [];
	this.preload = function (pathOrBuffer, destPath, fileSize) {
		let buffer = null;
		if (typeof pathOrBuffer === 'string') {
			const me = this;
			return this.loadPromise(pathOrBuffer, fileSize).then(function (buf) {
				me.preloadedFiles.push({
					path: destPath || pathOrBuffer,
					buffer: buf,
				});
				return Promise.resolve();
			});
		} else if (pathOrBuffer instanceof ArrayBuffer) {
			buffer = new Uint8Array(pathOrBuffer);
		} else if (ArrayBuffer.isView(pathOrBuffer)) {
			buffer = new Uint8Array(pathOrBuffer.buffer);
		}
		if (buffer) {
			this.preloadedFiles.push({
				path: destPath,
				buffer: pathOrBuffer,
			});
			return Promise.resolve();
		}
		return Promise.reject(new Error('Invalid object for preloading'));
	};
};
