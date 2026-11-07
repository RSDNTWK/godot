# Native HTTPS media transport

This internal module serves native media workers and URL resolution. It accepts
HTTPS on port 443, validates all DNS answers as public unicast, and pins a vetted
address while preserving the original hostname for certificate verification/SNI.
Redirects and range reopens repeat validation. FFmpeg secondary-resource opens
use this same transport, including cross-origin requests; local/file and other
protocols cannot be opened by the native network demuxing path.

Transport buffering is bounded to HTTP chunks. There are no disk downloads or
persistent media caches. User-Agent, Referer, Accept, and Origin headers can be
forwarded. YouTube API client/version/visitor metadata is restricted to the
original origin and stripped on cross-origin redirects. JSON POST bodies cannot
cross origins. Authentication headers are not accepted.
LAN/private, loopback, link-local, multicast, documentation and transition address
ranges are excluded by the default policy. There is no private-network override.
Encrypted HLS using FFmpeg crypto wrapper URLs is currently unsupported.

Every connection/read has a timeout and checks cancellation. DNS lookup uses the
queued resolver. All blocking loops belong on decoder/resolver worker threads.
This module is excluded from Web, unthreaded builds, and non-target platforms.
