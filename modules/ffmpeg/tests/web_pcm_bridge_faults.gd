# Browser-only integration checks. Call run() with a decoder helper returning
# {"error": Error, "result": Dictionary}; no production globals remain installed.
extends RefCounted

static func run(decode: Callable) -> Array[Dictionary]:
	var checks: Array[Dictionary] = []
	var bridge: Object = Engine.get_singleton("JavaScriptBridge")
	for entry in [["worker", FAILED], ["copy", ERR_OUT_OF_MEMORY], ["input", FAILED], ["start", ERR_CANT_CREATE]]:
		var code := """
(() => {
	const mode = FAULT_MODE;
	const state = { worker: window.Worker, slice: Uint8Array.prototype.slice, seen: false, terminated: 0 };
	window.__pcmBridgeFault = state;
	window.Worker = class extends state.worker {
		constructor(url, options) { super(url, options); this.pcmFaultTarget = String(url).includes('.media.worker.js'); }
		postMessage(message, transfer) {
			if (this.pcmFaultTarget && ((mode === 'start' && message.type === 'start') || (mode === 'input' && message.type === 'input'))) {
				state.seen = true;
				throw new Error('Injected PCM bridge submission failure.');
			}
			const result = super.postMessage(message, transfer);
			if (this.pcmFaultTarget && mode === 'worker' && message.type === 'start') {
				state.seen = true;
				queueMicrotask(() => this.dispatchEvent(new ErrorEvent('error', { message: 'Injected decoder worker failure.' })));
			}
			return result;
		}
		terminate() { if (this.pcmFaultTarget) state.terminated++; return super.terminate(); }
	};
	if (mode === 'copy') Uint8Array.prototype.slice = function (begin, end) {
		if (this.byteLength >= 8388608 && end - begin === 262144) {
			state.seen = true;
			Uint8Array.prototype.slice = state.slice;
			throw new RangeError('Injected encoded input copy allocation failure.');
		}
		return state.slice.call(this, begin, end);
	};
})();
""".replace("FAULT_MODE", JSON.stringify(entry[0]))
		bridge.call("eval", code, true)
		var reply: Dictionary = await decode.call("res://fixtures/tone-wav.wav.fixture")
		var detail: Dictionary = JSON.parse_string(bridge.call("eval", """
(() => {
	const state = window.__pcmBridgeFault;
	window.Worker = state.worker;
	Uint8Array.prototype.slice = state.slice;
	delete window.__pcmBridgeFault;
	return JSON.stringify({ seen: state.seen, terminated: state.terminated });
})();
""", true))
		checks.append({"check": "bridge %s: classified error" % entry[0], "passed": int(reply.error) == int(entry[1]), "detail": reply})
		checks.append({"check": "bridge %s: no partial stream" % entry[0], "passed": not reply.result.has("stream")})
		checks.append({"check": "bridge %s: fault observed and worker released" % entry[0], "passed": bool(detail.seen) and int(detail.terminated) > 0, "detail": detail})
	return checks
