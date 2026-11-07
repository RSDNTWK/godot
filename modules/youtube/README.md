# Native YouTube resolver

`YouTubeResolver` implements the public YouTube playback subset in C++. It uses
Godot HTTP/TLS and embedded QuickJS-NG with the unmodified upstream EJS solver.
There is no runtime Python dependency, yt-dlp executable, Deno process, media
download, or persistent media cache. Python remains a build-tool dependency.

The module is restricted to threaded Windows, Linux/BSD, and Android builds.
Web retains the application's browser/iframe path. No project synchronization,
permissions, revisions, or playback adapters are implemented here.

Both `AudioStreamMedia.file` and `VideoStreamMedia.file` accept an original
YouTube URL or a direct HTTPS media URL. YouTube extraction and A/V pairing are
automatic; the application does not invoke yt-dlp.

For applications that need metadata separately, the node's
`resolve(url, audio_only)` method returns immediately. Its `completed`
signal carries an Error and a dictionary of selected/all formats, required HTTP
headers, title, ID, duration, and live status. Pass the selected URL and headers
to `AudioStreamMedia` or `VideoStreamMedia`. HTTPS opening, byte-range seeking,
and decoding run on playback workers. Only bounded transport/PCM buffers are
retained. Network duration queries return zero until metadata is available.

Login/cookies, DRM, proof-of-origin token generation, SABR transport, playlist
enumeration, subtitles, and IAMF stream-group presentations are not ported.
Ordinary AAC/Opus alternatives are selected instead of unsupported IAMF tracks.
Separate adaptive audio/video URLs are returned as a pair; set
`VideoStreamMedia.audio_file` and `audio_http_headers` alongside its video URL
and headers. Video follows the consumed-audio clock and both inputs share controls.
Service-side changes or authentication requirements can prevent resolution or
playback. EJS solves signature/n challenges; it does not remove these requirements.
Active live broadcasts require a public HTTPS HLS manifest. Client responses
containing only unsupported adaptive live tracks are declined so normal client
fallback can continue; the current TVHTML5 response is followed by VISIONOS when
the latter supplies HLS. DASH-style live segment requests are not treated as
ordinary direct media files. HLS playback requires the target FFmpeg demuxer.

Before FFmpeg opens a YouTube live master, the resolver selects the lowest
compatible advertised-bandwidth H.264/AAC rendition and its default audio group.
Audio-only requests use the selected audio rendition directly when one is
provided. The resulting media-playlist URLs avoid probing every master variant
during decoder opening.

Manifest selection uses the same validated public HTTPS transport, including
redirects, and preserves signed URL query bytes. Each body is limited to 1 MiB,
each master to 128 variants and 128 rendition records, each branch to four
manifest levels, and the whole resolution to eight manifest fetches across
client fallbacks. The transport's existing redirect limit applies to each fetch.
Cycles, malformed or over-limit manifests, and masters without a supported
declared rendition fail explicitly. These bounds concern manifest metadata;
media continues to stream without a persistent cache.

`upstream.json` pins the yt-dlp release used as the extraction reference,
QuickJS release/commit, and the EJS asset hashes used by that yt-dlp release.
To update, review upstream's YouTube client profiles, player-response extraction,
signature/n handling, and EJS protocol. Import matching released EJS bundles and
verify their SHA3-512 hashes from yt-dlp's vendor metadata. Update the pin file and
run the parser, transport-policy, solver, and streaming tests. Do not auto-fetch
or replace executable scripts while a game is running.

Native streaming playback instances expose `ready`, `buffering_changed(bool)`,
`error(code, message)`, and `seek_completed(position)` signals. Read
`stream_ready`, `buffering`, and `stream_error` properties for current state.
`VideoStreamPlayer.get_stream_playback()` exposes the native playback instance;
audio players already expose their playback. Readiness means the inputs and
decoders are prepared, including the bounded live prefill; remote video seek completion follows decoding/discarding
preceding keyframes to reach the requested timeline position. Superseded
seek/stop generations do not deliver stale completion events.
The project remains responsible for its shared playback position and permissions.

The public VOD path has been exercised from original watch URLs on Windows,
Nokia XR20 (ARM64 over Wi-Fi/cellular and ARM32), and Linux x86-64 headless
software-decoding tests, using the native resolver and embedded solver without
an external extractor/runtime. Matching final source fingerprints also passed
eight Windows and seven Linux headless suites and 42 strict live checks per
ABI on a physical Nokia XR20. Original YouTube live URLs exercised native
resolution, paired playback, audio-only playback, controls and reopening;
independent host checks verified captured mixer PCM and changing player textures.
The FFmpeg wrapper prepares unknown-duration live inputs at a common source
timestamp before readiness and applies live PCM backpressure against presentable
video. Its selected live HLS inputs use separate compressed-packet readers so
segment fetches can overlap decoding and presentation. Readiness normally waits
for six seconds per input, with a 30-second wait limit and queue-capacity or
queued-terminal-data fallback. Each input queue is limited to 1,024 packets and
8 MiB, plus one in-flight allocation and FFmpeg's internal buffers. The wrapper
keeps accepted live audio within 100 ms plus source-time device latency of
presentable video and bounds outstanding audio production credit to one second.
Standalone audio, VOD, and Web retain their existing paths. Normal playback speed
is the runtime-qualified path; nondefault speeds,
positive-duration EVENT playlists and timestamp-epoch reconciliation remain
unqualified. A selected video packet's decode timestamp retreat over one second
stops playback with `ERR_UNAVAILABLE` and a sanitized cause; it does not stitch
different epochs. The device checks do not measure physical display/speaker
synchronization or establish an Android 150 ms lip-sync bound. Desktop device
output, physical shared-texture freeze during an in-flight pause, other devices
and long runs remain unqualified. An earlier intermittent Windows recovery/reopen
stall on the multi-period iReplay input remains causally unassigned; it is
preserved separately from the deterministic reset guard and final continuous
HLS qualification. See
`build/youtube-integration-20261106/live-playback/live-validation-summary.json`.
Twitch page-link resolution is not implemented by this YouTube-only module.
