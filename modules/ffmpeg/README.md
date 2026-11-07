# Native media streaming

On threaded Windows, Linux/BSD, and Android builds, `AudioStreamMedia.file` and
`VideoStreamMedia.file` accept direct HTTPS files or original YouTube URLs.
YouTube resolution requires `module_youtube_enabled=yes`; it embeds QuickJS-NG
and upstream EJS rather than a Python interpreter or external yt-dlp process.
These native modules are excluded from Web.

Opening, seeking, and decoding belong to playback workers. Network inputs use
Godot's certificate-verifying HTTPS transport through custom FFmpeg AVIO.
Redirects, byte-range reopens, and secondary media requests use the same public
destination validation. The network demuxing path cannot open local files or
arbitrary FFmpeg protocols. Buffers are bounded; media is not persisted to disk.

Interrupted known-length GETs and completed partial byte ranges can reopen at
the exact consumed byte within the original read deadline. Reopens validate
the returned range, total length and available ETag/Last-Modified identity;
zero-progress retries are bounded. POST requests and cancelled reads are not
replayed. Every reopen retains destination and certificate validation.

For original YouTube URLs, automatic selection chooses the highest supported
video rendition at or below 1920x1080 and 60 fps. Live HLS selection preserves
the selected rendition's default audio pairing. FFmpeg opens
those media playlists directly, avoiding the initial probe of every variant in
the YouTube master. The resolver bounds manifest reads and nesting; HTTPS
destination validation still covers the selected playlists and media requests.

YouTube's selected video-only and audio-only inputs share one playback clock.
Native streamed video follows source samples consumed by `VideoStreamPlayer`'s
audio mixer, with output latency accounted for. Consumed source samples are
mapped to their scheduled AudioServer output frames, including differences
between server mix blocks and driver buffer sizes. Interpolation is bounded by
real source consumption. Samples waiting in decode or resampler queues do not
advance that clock. Input opening/preparation time is
excluded. Remote seeks decode/discard preceding keyframes on the worker before
publishing completion, including seeks made while paused.
For manually paired direct inputs, use `VideoStreamMedia.audio_file` and
`audio_http_headers`. Embedded audio in ordinary local/downloaded media keeps
the existing playback path.

Unknown-duration live HLS inputs prepare both tracks at the later of their
initial source timestamps before publishing readiness. Earlier audio samples
and video frames are discarded during preroll; an initial gap over 30 seconds
is rejected. Only the selected HLS video rendition and its audio streams remain
active. Positive-duration EVENT playlists and changes of timestamp epoch at
discontinuities are not qualified. A selected video packet whose decode
timestamp retreats by more than one second reports `ERR_UNAVAILABLE` with a
sanitized timestamps/discontinuity message. Playback stops, stale PCM is cleared,
and readers are released. This rejects an unsupported reset; it does not
normalize the two clocks or provide general epoch reconciliation.

Remote video playback gives each selected video/audio demuxer
its own compressed-packet reader. Both readers start before readiness waits up
to 30 seconds for an initial six-second horizon per input. A full queue or a
terminal input with remaining packets also satisfies that input's prefill.
Each queue holds at most 1,024 packets and 8 MiB of charged packet storage,
including backing buffers and side data. These are queue bounds: one in-flight
allocation and FFmpeg's internal demuxer/IO buffers remain additional memory.
Readers pause at capacity and resume as decoding consumes packets. Cancellation
interrupts both capacity waits and active HTTPS reads before the reader joins
and its format closes. Remote paired audio uses the same read-ahead policy.
Standalone audio, ordinary local media, and Web keep their existing paths.

Streamed video decoding and RGBA conversion produce immutable CPU images on
the worker. A queue bounded to four images and 32 MiB transfers them to the
main frame tick for timestamp-based texture publication; one supported image
larger than the queue budget may occupy it alone, up to 64 MiB. GPU textures
remain on the frontend. A seek clears queued images and resets the generation.
Presentation tolerates small render-clock jitter while discarding genuinely
late frames. A prepared image is queued before readiness becomes observable;
opening, reopening and paused seeking can publish that image without advancing
paused media time. Texture publication remains on the main thread.

Sustained YouTube playback pressure can lower the quality ceiling through
720p, 480p, 360p, 240p and 144p. Startup, pause and seek receive a grace period;
attempts have a cooldown, and a failed lower-rendition opening restores the
previous selection once. Live handoff requires an advertised UTC timeline so
both inputs reopen at the same media position; otherwise the current decoder
is retained. This internal timeline does not enable public DVR controls when
the caller has left DVR disabled.

Live paired playback limits accepted frontend PCM to a 100 ms horizon plus
source-time output latency beyond decoded/presentable video, including a queued
future frame during hardware recovery. Video starvation reports buffering
instead of allowing audio to outrun video. The newest usable frame is published
before another packet read. Live audio retains at most one second of source
production credit during stalls; rejected PCM remains queued for later
consumption. `VideoStreamPlayer` supplies
output latency scaled by effective playback speed through an optional internal
playback hook; the asynchronous wrapper retains that override across seeks and
uses the AudioServer latency when none is supplied. Runtime qualification uses
normal speed; nondefault speeds remain unqualified.

Windows prefers D3D11 decoding. If hardware decoding fails on a sliding live
input, software decoding resumes at the next independent frame without seeking
back or resetting paired audio. On Linux, the WSL lifetime guard marks only the
already-loaded system D3D12 library pair `RTLD_NODELETE`, so registered thread
cleanup callbacks retain executable code; decoder/device allocations are still
released. RGBA conversion uses aligned, padded FFmpeg frame storage before
copying packed rows into the Godot image.

Get the native playback instance from `VideoStreamPlayer.get_stream_playback()`
or the audio player's `get_stream_playback()`. It exposes:

| Signal | Meaning |
| --- | --- |
| `ready()` | Inputs and decoders are prepared, including bounded live prefill; emitted once per instance. |
| `buffering_changed(buffering)` | Opening, seeking, or decoder starvation changes buffering state. |
| `error(code, message)` | Resolution, transport, decoder, or seek failure. |
| `seek_completed(position)` | The latest seek is prepared; remote video preroll has reached its target. |

`stream_ready`, `buffering`, and `stream_error` expose current state. Readiness
and seek completion do not promise that a frame has reached the display or
speakers. Superseded seeks/stops suppress stale completion events. The project
can apply its shared position when ready and retain its own synchronization,
permissions, evidence revisions, playlist controls, and aspect fitting.

Use the video player node's normal controls and `stream_position` property; its
playback getter exposes the native readiness/buffering/seek/error signals.
`AudioStreamPlayer.seek()` replaces its playback instance in Godot. Reacquire
`get_stream_playback()` and connect the new instance's signals after player-level
audio seeks or restarts. A direct playback-instance seek retains that instance.

## FFmpeg SDK requirements

Use a matched set of LGPL FFmpeg libraries and headers. The validated Windows
SDK uses upstream FFmpeg `n9.0.2`, commit
`946fcce07b6dcd0331c8cc609192aeff5e1924f8`. Preserve the target's existing codec and
hardware-decoder options and add `--enable-demuxer=hls --enable-demuxer=mpegts`
for unencrypted HTTPS HLS, including live inputs. Apply
`patches/hls-custom-https-io.patch` so HLS recognizes HTTPS provided by custom
AVIO when no FFmpeg TLS backend is built. Then apply
`patches/hls-declared-epochs.patch`. Native real-time playback and finite PCM preparation opt into its
`toi_pdt_timeline` demuxer option. This maps unencrypted single-track H.264/AAC
fMP4 playlists with program-date timestamps onto a shared UTC clock. A declared
discontinuity drains the old demuxer before reopening with the next MAP; codec
extradata follows the new packets. Track layout, dimensions, time base and audio
rate/layout must remain compatible. Advertised UTC anchors and durations must
accurately describe the source. Missing/invalid UTC anchors, changes to other
codecs/layouts and UTC gaps over 30 seconds remain unsupported. Undeclared source
resets retain the explicit stop/error guard. Real-time audio trims overlapping
samples or pads bounded declared timeline gaps. Finite PCM preparation retains
all decoder samples across the same demuxer transitions, without clock trimming
or padding. Web transport is unchanged.

HLS also selects the AAC,
AC-3/E-AC-3 demuxers and AC-3 parser in FFmpeg. No FFmpeg TLS backend is required:
Godot owns the validated network connections. Keep GPL/nonfree disabled.

The Windows, Linux x86-64, Android ARM64, and Android ARM32 SDKs have been rebuilt
with these additions. The pre-DVR streaming baseline passed eight Windows
and seven Linux headless suites, covering original YouTube live URLs, a public
continuous AWS HLS control, decoded audio/video, pause/resume, cancellation,
reopening, delayed origins, and the unsupported-reset error contract. Windows
also passed injected hardware-to-software recovery. That recorded source
passed 42 strict live checks in each ABI on a physical Nokia XR20, with
independently decoded mixer PCM and changing player-texture captures.
These results do not measure physical display/speaker synchronization or an
Android 150 ms lip-sync bound. Desktop device output, physical frame freeze of
a shared texture during in-flight pause work, and broader device coverage remain
unqualified. An earlier intermittent Windows recovery/reopen stall on the
multi-period iReplay feed remains causally unassigned; its preserved evidence is
separate from the deterministic reset guard and final continuous-control tests.
See the recorded hashes and exact scope in
`build/youtube-integration-20261106/live-playback/live-validation-summary.json`.
Those artifacts describe the earlier baseline; YouTube DVR validation and its
source/binary fingerprints are recorded separately under
`build/youtube-integration-20261106/live-playback/youtube-dvr/`.
Twitch page-link resolution is not implemented.

## YouTube DVR

Set `live_dvr_enabled = true` on `VideoStreamMedia` or `AudioStreamMedia`
before instantiating playback. DVR is opt-in and applies only when YouTube
advertises DVR and the selected media playlists provide a continuous UTC
timeline. Playback begins near live. `get_dvr_window()` on the playback (or
`VideoStreamPlayer`) exposes `available`, `start`, `end`, `live_position`, and
`position` in seconds on that instance's fixed timeline. UTC origin, window
endpoints, and current position are also returned for translating between
instances. The range is observed from playlist reloads, not extrapolated; it
may be stale while paused or buffered behind live.

Use normal playback-instance seeking or `VideoStreamPlayer.stream_position`
to rewind/seek forward, and `seek_to_live()` to request the current safe live
edge. Old/future positions clamp to the freshly fetched available window.
`AudioStreamPlayer.seek()` creates a new playback instance; seek its existing
playback directly to retain that instance's DVR origin. Commands cancel the old
generation and reopen both validated inputs on the worker. A bounded manifest
view selects preceding segments, then both decoders preroll to a shared target;
FFmpeg does not seek a live demuxer owned by a packet reader. Subsequent rolling
reloads retain the same sequence floor, allowing playback to continue beyond
the initially observed edge. Signed URLs are unchanged. No media cache or
complete-file download is introduced.

DVR is unavailable for ambiguous timestamp epochs/discontinuities, encrypted
HLS, non-rolling EVENT/VOD manifests, byte-ranged segments, partial segments,
or missing program-date timestamps.
Ordinary live playback remains available when the initial playlist cannot
provide DVR. HTTPS destination checks still cover every manifest, redirect,
initialization input, and media segment. Web playback is unchanged.

Offline selection/bounds tests use `--test-case=*[YouTubeDVR]*`. For network
rewind tests set `GODOT_YOUTUBE_DVR_URL` to an active DVR-enabled YouTube URL
and run `--test-case=*YouTubeDVRPlayback*,*YouTubeDVRAudio* --no-skip`.

DASH requires an appropriate SDK and is not part
of this validation. FFmpeg `crypto+https` HLS inputs are rejected by the native
transport. YouTube login, DRM, proof-of-origin token generation, and SABR are
not implemented; service restrictions can prevent playback even if extraction
returned a URL. Authentication requirements are reported through the error
signal.

## Validation

The 2026-11-07 recovery follow-up fixes deterministic asynchronous EOF/tail
retention, source/output-clock mapping, frame presentation and HTTP range
resumption. Windows and Linux each pass 1,588 offline assertions, 112 terminal
assertions and 60 paired-HLS hardware-recovery assertions. The original
historical stall lacked a state snapshot; its exact cause remains unproven.

Both desktop platforms completed a direct HTTPS film through natural EOF and
900 source seconds of separate 1080p60 HLS video/audio. Those endurance binaries
include the clock, pacing and range fixes, but precede the final prepared-image
publication adjustment. Final binaries passed focused recovery regressions,
short direct-stream controls, and Android ARM32/ARM64 each passed 21 checks
with independently decoded PCM and changing captured pictures. These results
do not establish sustained 60 fps on every Android device.

Original YouTube URL requalification is currently blocked by the service's
bot-confirmation response, also reproduced with the previously passing engine,
upstream yt-dlp and Windows Sandbox. Direct-media controls do not replace that
extraction test or qualify public DVR. Earlier original-URL, DVR and adaptive
handoff results retain their recorded binary scope. External 60 fps recordings
measure recorded end-to-end timing; the camera is uncalibrated and speaker
distance unknown, preventing absolute physical timing qualification.

Exact hashes, scope and final recording measurements are recorded in
`build/youtube-integration-20261106/live-playback/final-validation-summary.json`.
Detailed logs and the continuation checkpoint are under the adjacent
`recovery-stall/`, `long-duration/`, and `physical-sync/` directories. Twitch
page-link support and the separate engine/dependency audit remain on hold.

Offline parser, destination-policy, upstream challenge-fixture, and error-event
tests are included. `--test-case=*[PacketReadAhead]*` exercises packet order,
queue limits, blocked-reader cancellation, and PCM recovery after packet
starvation without network access. Network cases are skipped by default. To opt in, set
`GODOT_MEDIA_TEST_URL` to a public HTTPS range-capable MP4,
`GODOT_YOUTUBE_LIVE_URL` to a public YouTube watch URL, and
`GODOT_YOUTUBE_TEST_PLAYER` to the upstream solver test's player script. Run
`--headless --test --test-case=*YouTube*,*MediaStream*,*NativeStreaming* --no-skip`.
Use `GODOT_FFMPEG_FORCE_SOFTWARE=1` for reproducible decoder comparisons.
For live cases, set `GODOT_MEDIA_LIVE_TEST_URL` to a public live HLS or original
YouTube live URL and run `--test-case=*LiveStreaming* --no-skip`. Set
`GODOT_MEDIA_LIVE_TEST_AUDIO_URL` when testing explicitly paired inputs.
Network tests can fail independently because of CDN availability or YouTube
service restrictions. They are not substitutes for device playback tests.

## Advisory playback metadata

The native playback exposes `get_stream_metadata()` and
`metadata_changed(metadata)`; `VideoStreamPlayer` delegates the getter.
YouTube titles reuse the existing extraction. Direct inputs read container
title tags during the existing open, with a decoded filename fallback.
There is no separate title request and no automatic resource renaming.
Titles are plain text, bounded to 256 characters with ASCII control characters
removed. Snapshots include original `source`, `source_type`, `title_source`,
duration, live state, and `opened`. Preliminary resolution has `opened=false`;
successful readiness atomically publishes opened metadata.

`playback_instance_id` and `generation` identify the emitting native playback
and opening. Deferred notifications from superseded generations are dropped.
The application must also capture/check its own queue-entry ID, since the same
URL may be queued twice. Metadata is advisory: playlist names and user-entered
evidence titles remain authoritative. Queued items can keep a temporary title
until playback opens; this API does not provide pre-submission queue lookup or
share caches between separately created resolver/playback instances.

## Unified media controller and Web preparation

`MediaStreamPlayer` is an opt-in Control with the same commands/events on
native and Web targets. It wraps the existing native resources or browser
media/YouTube iframe backends. It does not migrate existing project callers.
Every event includes the controller's `playback_instance_id` and opening
`generation`; seek completions also include `seek_request_id`. Applications
retain their own queue/evidence identities, authoritative titles and clocks.

`open(url, {"prepare_pcm": true})` is for bounded finite audio playlists.
Other sources stream normally. Precise PCM loops and approximate seek loops
are distinct capabilities. Capability and seekable-range changes are events.
An expired loop is disabled once, retains its requested boundaries, and leaves
playback running. Source changes suppress retired callbacks.

On both Web variants, `AudioStreamPCMDecoder` runs FFmpeg in a dedicated
non-threaded WASM worker, independent of MediaHTTP and SharedArrayBuffer.
The output cap is 128 MiB; transient encoded input is capped at 64 MiB.
Each worker's WASM heap is capped at 512 MiB, with at most four active workers.
These are component limits, not a promise about total browser memory. Remote
preparation requires a finite Content-Length, CORS and direct HTTPS without
redirects. The decoder denies secondary requests and never returns partial
WAVs on failure, timeout or cancellation.

Web playback retains browser-permitted HTTP/HTTPS links rather than rewriting
them. Its networking, redirects and mixed-content rules belong to the browser;
it does not claim native destination/DNS pinning. CORS-denied media may still
play through an ordinary media element. Direct browser media buffers are
browser-managed; HLS.js 1.7.4 has explicit buffer targets, not a hard cap on
all browser-owned memory. Browser media/iframe gain is independent of Godot
audio buses/effects. The capability dictionary reports these distinctions.

CORS-readable video can render as a Godot texture. DOM-only video/iframes are
composed beneath the canvas with later Godot controls above them. The current
DOM adapter is qualified for axis-aligned main-viewport controls; arbitrary
transforms, SubViewports and DOM fullscreen composition are not qualified.
YouTube uses the iframe API on Web, with seek-based finite loops and no
invented DVR ranges, extracted URLs or PCM access. Native extraction is
unchanged. Keep existing project fallbacks until the actual required layouts
and source capabilities are qualified.

The standard Web exporter bundles `.media.worker.js`, `.media.decoder.js`
and `.media.hls.js`. Deploy all three alongside the ordinary engine assets.
The standalone decoder always uses `ffmpeg_dir/nothreads`, even when the main
engine uses the threaded SDK. HLS.js is pinned under `thirdparty/hlsjs` with its
upstream license and SHA256 provenance.
