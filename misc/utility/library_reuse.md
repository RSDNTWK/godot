# Reusing static libraries without loose objects

Add `library_reuse=yes` to the usual SCons arguments to enable verified static
library reuse. It defaults to `no`; existing build commands keep their normal
behavior. This applies to libraries built through `methods.add_library`.
FFmpeg's separately installed libraries already link as prebuilt inputs and do
not need this option.

A successful archive build writes a small adjacent `.reuse.json` record. On
later invocations, SCons checks each archive independently. A matching archive
can supply the final link even when its loose `.o` or `.obj` files are gone.
This also works with Godot's default directory build traversal.

Checks cover archive contents, source contents, the header dependencies found
by SCons, generated files and their declared inputs/actions, explicit
dependencies, expanded build actions, executable build tools, the build process
environment, and Python/SCons/cache implementation identities. Values are hashed;
records do not store source contents, encryption keys or expanded commands.
The raw `PATH` string is excluded: the resolved executable paths and their
contents detect changes to the selected build tools. This avoids recompiling
unchanged code when a launcher adds an unrelated shim directory. Other process
environment inputs, including include/library search settings, remain checked.
WSL's per-launch `WSL_INTEROP` transport address and `SHLVL` shell depth are also
excluded. Unknown/custom environment variables remain significant. File paths
use Windows' case-insensitive identity on Windows and retain case on POSIX.

Version 3 records separate a generated file's declared recipe from its bytes.
When returning to a configuration whose generated files were overwritten by a
different configuration, the archive can remain reusable if its recipe matches
and SCons will rebuild the generated files. A dependency guard checks their
actual bytes before allowing the cached archive to be used. The guard always
runs and cannot be retrieved from SCons' artifact cache. If regeneration differs
from the cached build, linking stops and the stale record is removed for a normal
rebuild on retry. Manually edited outputs without a scheduled regeneration
invalidate reuse. Deterministic generators with multiple file outputs, such as
the editor's translation header/source pairs, are supported as one output group.

Known compatible version 3 metadata revisions can be retagged after the full
archive, dependency and generated-output checks pass. This changes no library
or object bytes. Older formats and unknown implementation revisions refresh
records through the normal object dependency graph; otherwise-current objects
are not forced to compile merely because bookkeeping changed. Missing objects
still require compilation when their old-format archive cannot be validated.
Build scripts must declare their dependencies correctly, as with ordinary
SCons builds. These checks establish freshness, not the absence of code bugs or
ABI errors. The final linker still validates symbol resolution.

Changing one library does not invalidate every library passed to the final
linker. Changes to a shared header or build setting can invalidate several
libraries. Where objects remain, unchanged ones stay incremental. Where objects
have been deleted, rebuilding an invalidated archive requires recreating its
missing members. This is the storage/compilation tradeoff of retaining only an
archive.

Missing, corrupt or incompatible records fall back to the ordinary build graph.
Existing archives are never adopted simply because they exist. To deliberately
seed records with a fresh build, add `-B library_reuse=yes` to the correct full
build command. There is no need to do this merely to use the existing binaries.
Use the same configuration for later invocations. A build with changed source
inputs must complete before its new record is trusted. `scons -c
library_reuse=yes` also cleans the associated records.

No object files are deleted automatically. Retain objects used outside static
archives, and retain PDBs and other linker/debug inputs. Thin archives, object or
archive builders with multiple outputs, side effects, order-only dependencies,
unsupported node types and always-built inputs cannot be reused. Ninja,
`fast_unsafe` and interactive mode keep the normal build behavior.

Before a manual loose-object purge, export the exact configuration's graph with
`python misc/scripts/plan_object_cache.py --manifest bin/object-retention.json --`
followed by the usual SCons arguments, including `library_reuse=yes`. This exits
during graph construction without compiling, linking or deleting anything.
`direct_link_objects` lists the loose objects to keep for executables, console
wrappers and shared libraries, including generated and resource objects. An
object used both directly and in an archive is kept. Only actual object sources
of verified reusable archives appear in `removable_archived_objects`; an explicit
dependency alone is not proof of archive membership. Keep all other objects.
Retained objects stay subject to normal SCons source/header/options checks, so
unchanged inputs need no compilation while changed inputs still rebuild.

The platform reuse matrix uses this graph automatically before each purge and
checks retained-object hashes before and after deletion. It does not apply the
exception to an explicit `scons -c`, whose purpose remains a normal clean.

The adapter uses SCons File node APIs to convert verified archives into existing
inputs after all SCsubs have added dependencies. It does not change global
deciders or detach objects from their other consumers. It retains dependency
information until archive post-actions have recorded the successful build, which
adds memory and hashing overhead when enabled. Recheck the adapter when upgrading
SCons. The regression checks were run against SCons 4.11.0.
Explicit archive, program, alias and directory targets limit validation to their
reachable libraries; a normal default build continues to cover its full graph.

A full Windows x86_64 release-template build with MSVC 14.5 was also tested
after purging its loose objects while retaining the libraries. The initial
build compiled 2,532 units and recorded 63 archives; after another purge, the
same command reused all 63 archives, compiled 19 direct executable inputs,
linked successfully and passed the bounded headless smoke. Approximate wall
times were 21 minutes and 1 minute. The existing CRT/libdav1d linker warnings
were present in both passes. Other build profiles were not tested in this
full-build comparison. See `bin/library-reuse-full-build-20261020/RESULTS.txt`.

The version 3 regression work is recorded in
`bin/library-cache-validation-fix-20261020/RESULTS.txt`. Real ENet archives were
checked in all 14 supported configurations: both Windows editors, Windows/Linux
templates, four Web variants, and Android ARM32/ARM64 templates. Removing their
12 archive-only objects and repeating the builds produced zero compilation or
archive actions and preserved archive bytes. Fresh-link executable tests use
isolated compiler fixtures. The subsequent full-engine two-pass benchmark also passed all 14 profiles,
including both Android packaging passes and 36 desktop checks. All 10
Windows/Linux/Web reuse builds had zero source compilation/archive actions.
Android configuration switches rebuilt their 24 shared objects; an unchanged
Android fresh link had zero compilation/archiving. Timings and preserved
diagnostics are in `bin/platform-build-reuse-matrix-20261020/RESULTS.txt`.
These are SCons timings; one-time graph planning/purge work is separate.

Run `python misc/scripts/test_library_reuse.py` for real compilation, archive,
link and execution checks with GCC/MinGW. On Windows, set
`LIBRARY_REUSE_TEST_TOOL=default` to use the installed MSVC toolchain. Test
projects are created under `bin` and removed when each test finishes. For
unexpected ordinary rebuilds, append `--debug=explain` to the actual build command
to see which dependency or action SCons considers changed.
