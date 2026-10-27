# Number conversion compatibility wrapper

Godot's existing `grisu2::to_chars()` interface now uses stable Dragonbox 1.1.3
for both IEEE binary32 and binary64 shortest round-trip digit generation.
The formatter retains the previous Godot fixed/scientific thresholds, integer
spelling, signed-zero behavior and length-bounded memory operations.

The original MIT-licensed formatting helpers and their license are retained.
Dragonbox source and both upstream license options are in `../dragonbox/`.
The old Grisu2-specific patch is superseded by this explicit compatibility wrapper.
