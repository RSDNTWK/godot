"""Opt-in reuse of static libraries after their loose objects have been removed.

Records originate from a successful archive action. A known compatible metadata
revision may be retagged only after full archive and dependency validation. An
absent or invalid record leaves the normal SCons graph in place; arbitrary
prebuilt libraries cannot be adopted merely because they exist.
"""

import contextlib
import hashlib
import json
import os
import shlex
import sys
import tempfile
from pathlib import Path


_VERSION = 3
_libraries = []
_OBJECT_SUFFIXES = {".o", ".obj", ".os"}
_ARCHIVE_SUFFIXES = {".a", ".lib"}
# Same v3 fingerprints, before directory-target handling and metadata retagging.
# This audited revision already includes the uncacheable generated-input guard.
_COMPATIBLE_IMPLEMENTATIONS = {"2e6ccfbef09114f7f7f450b938a3c8ac4d3a3aff0e1e13d72ee768383664a90d"}


class CannotReuse(Exception):
    pass


def _digest(value):
    if isinstance(value, str):
        value = value.encode("utf-8")
    return hashlib.sha256(value).hexdigest()


def _json_digest(value):
    return _digest(json.dumps(value, sort_keys=True, separators=(",", ":")))


def _identity(path):
    # File spellings are aliases on Windows; preserve case on POSIX filesystems.
    return os.path.normcase(str(path))


def _objects(node):
    pending = list(node.children())
    visited = set()
    while pending:
        child = pending.pop()
        if child in visited:
            continue
        visited.add(child)
        if child.has_builder() and hasattr(child, "get_abspath"):
            if Path(child.get_abspath()).suffix in _OBJECT_SUFFIXES:
                yield child
        pending.extend(child.children())


class Snapshot:
    def __init__(self):
        self.files = {}
        self.nodes = {}
        self.environments = {}
        self.visiting = set()
        self.generated = {}
        self.generated_nodes = {}
        self.references = {}

    def file(self, path):
        path = Path(path)
        key = _identity(path)
        if key not in self.files:
            with path.open("rb") as stream:
                digest = hashlib.sha256()
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            self.files[key] = digest.hexdigest()
        return self.files[key]

    def environment(self, env):
        from SCons import __version__ as scons_version

        key = id(env)
        if key in self.environments:
            return self.environments[key]
        # Build actions contain expanded flags; also check the actual compiler,
        # assembler, archiver and Python binaries, including in-place upgrades.
        tools = {_identity(Path(sys.executable).resolve()): self.file(sys.executable)}
        for name in ("CC", "CXX", "AS", "AR", "RANLIB"):
            command = env.subst("$" + name)
            if not command:
                continue
            # Quoted Windows paths must keep their backslashes. Check launchers
            # as well as compilers when more than one executable is named.
            words = shlex.split(command, posix=os.name != "nt")
            found = False
            for word in words:
                word = word.strip('"')
                resolved = env.WhereIs(word) if not word.startswith("-") else None
                if resolved:
                    tools[_identity(Path(resolved).resolve())] = self.file(resolved)
                    found = True
            if not found:
                raise CannotReuse("Cannot identify build tool: " + name)
        # PATH is a lookup mechanism, not a compiler option. The resolved paths
        # and contents above detect a different selected tool, while unrelated
        # launcher/shim directories must not invalidate every object. Preserve
        # the other environment inputs, including INCLUDE/LIB and custom flags.
        # WSL_INTEROP names a per-launch transport socket; SHLVL is shell depth.
        # Neither selects compiler flags or source inputs. Keep all other env
        # inputs, including unknown/custom variables and reproducibility flags.
        ignored = {"PATH", "WSL_INTEROP", "SHLVL"}
        process_env = {
            str(k).upper() if os.name == "nt" else str(k): str(v)
            for k, v in env.get("ENV", {}).items()
            if str(k).upper() not in ignored
        }
        value = _json_digest([tools, process_env, sys.version, scons_version])
        self.environments[key] = value
        return value

    def action(self, executor):
        # Recording cache metadata does not change the archive's compiled code.
        # Other user/toolchain pre-actions and post-actions remain significant.
        actions = [a for a in executor.get_action_list() if getattr(a, "execfunction", None) is not _record]
        return _digest(
            b"".join(
                a.get_contents(executor.get_all_targets(), executor.get_all_sources(), executor.get_build_env())
                for a in actions
            )
        )

    def node(self, node):
        from SCons.Node.FS import Entry, File
        from SCons.Node.Python import Value

        if isinstance(node, Entry):
            node = node.disambiguate()
        reused = getattr(node.attributes, "library_reuse_signature", None)
        if reused is not None:
            return reused
        if node in self.nodes:
            return self.nodes[node]
        if node in self.visiting:
            raise CannotReuse("Dependency cycle")
        self.visiting.add(node)
        try:
            references = set()
            if isinstance(node, Value):
                children = list(node.children())
                value = ["value", _digest(node.get_contents()), [self.node(c) for c in children]]
                for child in children:
                    references.update(self.references.get(child, ()))
            elif isinstance(node, File):
                path = node.get_abspath()
                derived = node.has_builder()
                # Missing loose objects are the sole missing-file exception.
                # Generated headers/sources must exist and match their record.
                object_file = derived and Path(path).suffix in _OBJECT_SUFFIXES
                value = ["file", _identity(path), None if object_file else self.file(path)]
                if derived:
                    executor = node.get_executor()
                    always_build = node.always_build and not getattr(node.attributes, "library_reuse_forced", False)
                    if always_build or node.side_effects or node.prerequisites:
                        raise CannotReuse("Always-built targets, side effects or order-only dependencies")
                    targets = [t.disambiguate() if isinstance(t, Entry) else t for t in executor.get_all_targets()]
                    generated = not object_file and Path(path).suffix not in _ARCHIVE_SUFFIXES
                    if len(targets) != 1 and (
                        not generated
                        or any(
                            not isinstance(t, File)
                            or Path(t.get_abspath()).suffix in _OBJECT_SUFFIXES | _ARCHIVE_SUFFIXES
                            or t.always_build
                            or t.side_effects
                            or t.prerequisites
                            for t in targets
                        )
                    ):
                        raise CannotReuse("Unsupported multi-target builder")
                    children = list(node.children())
                    details = [
                        self.action(executor),
                        self.environment(node.get_build_env()),
                        [self.node(child) for child in children],
                    ]
                    for child in children:
                        references.update(self.references.get(child, ()))
                    if generated:
                        # Generated bytes may currently belong to another build
                        # configuration. Their declared recipe is the stable
                        # identity; actual bytes are validated separately below.
                        recipe = _json_digest([details, [_identity(t.get_abspath()) for t in targets]])
                        value = ["generated", _identity(path), recipe]
                        for target in targets:
                            key = _identity(target.get_abspath())
                            self.generated[key] = {"inputs": recipe, "sha256": self.file(target.get_abspath())}
                            self.generated_nodes[key] = target
                            references.add(key)
                    else:
                        value += details
                elif node.depends:
                    value += [[self.node(child) for child in node.depends]]
                    for child in node.depends:
                        references.update(self.references.get(child, ()))
            else:
                raise CannotReuse("Unsupported dependency node: " + type(node).__name__)
            result = _json_digest(value)
            self.nodes[node] = result
            self.references[node] = references
            return result
        finally:
            self.visiting.remove(node)

    def generated_checks(self, node, expected):
        if not isinstance(expected, dict) or set(expected) != self.references[node]:
            raise CannotReuse("Generated dependency set changed")
        pending = {}
        for path, old in expected.items():
            current = self.generated[path]
            if not isinstance(old, dict) or old.get("inputs") != current["inputs"]:
                raise CannotReuse("Generated dependency recipe changed")
            if old.get("sha256") != current["sha256"]:
                if not self.generated_nodes[path].changed():
                    raise CannotReuse("Generated output changed without a scheduled rebuild: " + path)
                pending[path] = old["sha256"]
        return pending

    def object_signature(self, node, expected=None):
        logical = self.node(node)
        outputs = []
        for path in sorted(self.references[node]):
            current = self.generated[path]
            content = current["sha256"]
            old = (expected or {}).get(path, {})
            if old.get("inputs") == current["inputs"] and old.get("sha256") != content:
                if self.generated_nodes[path].changed():
                    content = old["sha256"]
            outputs.append([path, content])
        return _json_digest([logical, outputs])


def _record_path(node):
    return Path(node.get_abspath() + ".reuse.json")


def record_compatible(record, snapshot):
    return (
        isinstance(record, dict)
        and record.get("version") == _VERSION
        and isinstance(record.get("implementation"), str)
        and record.get("implementation") in {snapshot.file(__file__), *_COMPATIBLE_IMPLEMENTATIONS}
    )


def _retag_record(path, record, implementation):
    """Retag only a fully verified record from an audited compatible revision."""
    updated = dict(record, implementation=implementation)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(updated, stream, sort_keys=True)
            stream.write("\n")
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            with contextlib.suppress(OSError):
                temporary.unlink(missing_ok=True)


def _record(target, source, env):
    """Post-action: never records an archive whose build failed."""
    node = target[0]
    path = _record_path(node)
    temporary = None
    try:
        # Thin archives depend on loose member files and cannot save this space.
        with Path(node.get_abspath()).open("rb") as stream:
            if stream.read(8) != b"!<arch>\n":
                raise CannotReuse("Not a self-contained archive")
        snapshot = Snapshot()
        record = {"version": _VERSION, "implementation": snapshot.file(__file__), "signature": snapshot.node(node)}
        record["objects"] = {_identity(child.get_abspath()): snapshot.object_signature(child) for child in _objects(node)}
        record["generated"] = {key: snapshot.generated[key] for key in sorted(snapshot.references[node])}
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(record, stream, sort_keys=True)
            stream.write("\n")
        os.replace(temporary, path)
    except (OSError, ValueError, TypeError, AttributeError, CannotReuse) as error:
        # Failure to record is not a build failure. Remove an older record so it
        # cannot describe an archive that this invocation has just overwritten.
        with contextlib.suppress(OSError):
            path.unlink(missing_ok=True)
        if env.get("verbose", False):
            print("Library reuse record skipped for %s: %s" % (node, error))
    finally:
        if temporary is not None:
            with contextlib.suppress(OSError):
                temporary.unlink(missing_ok=True)
    return 0


def _validate_generated(target, source, env):
    data = json.loads(source[0].get_contents())
    snapshot = Snapshot()
    for path, expected in data["outputs"].items():
        if snapshot.file(path) != expected:
            # This generated recipe did not reproduce the bytes used to build
            # the cached archive. Fail before linking it, then allow a normal
            # rebuild on retry. Never overwrite or delete the archive itself.
            Path(data["record"]).unlink(missing_ok=True)
            raise RuntimeError("Generated output differs from the cached build; retry to rebuild: " + path)
    Path(target[0].get_abspath()).write_text("verified\n", encoding="utf-8")
    print("Verified regenerated inputs for cached archive.")
    return 0


def register(env, library):
    from SCons.Action import Action

    for node in library:
        env.Clean(node, str(_record_path(node)))
        env.Clean(node, node.get_abspath() + ".reuse-check")
        if env.get("ninja", False) or env.get("fast_unsafe", False) or env.GetOption("interactive"):
            continue
        _libraries.append((env, node))
        env.AddPostAction(node, Action(_record, None))


def selected_libraries(env):
    """Limit explicit target builds to the libraries in their dependency graph."""
    from SCons.Script import COMMAND_LINE_TARGETS

    if not COMMAND_LINE_TARGETS:
        return _libraries
    pending = env.arg2nodes(COMMAND_LINE_TARGETS, node_factory=env.fs.Entry)
    reachable = set()
    while pending:
        node = pending.pop()
        if node in reachable:
            continue
        reachable.add(node)
        pending.extend(getattr(node, "sources", ()))
        pending.extend(node.children())
    return [(library_env, node) for library_env, node in _libraries if node in reachable]


def finalize(env):
    """Validate the complete graph, including Depends added after add_library.

    SCons has no public operation for converting a derived File to an existing
    input. Keep that small graph adapter here; do not change global deciders or
    the graph of any object shared by another target. Validation finishes for
    every library before any node is converted.
    """
    if not env.get("library_reuse", False):
        return
    if env.GetOption("clean") or env.GetOption("help") or env.GetOption("interactive"):
        return
    if env.get("ninja", False) or env.get("fast_unsafe", False):
        return
    # SCons normally releases completed objects' executors. Archive post-actions
    # still need their build commands and dependencies to describe this build.
    libraries = selected_libraries(env)
    pending = [node for _, node in libraries]
    retained = set()
    while pending:
        node = pending.pop()
        if node in retained:
            continue
        retained.add(node)
        node.attributes.keep_targetinfo = True
        if node.has_builder():
            # An unreferenced sibling of a generated header/source shares its
            # executor. Keep the whole group available for archive recording.
            pending.extend(node.get_executor().get_all_targets())
        pending.extend(node.children())
    snapshot = Snapshot()
    reusable = []
    for library_env, node in libraries:
        record = None
        try:
            record = json.loads(_record_path(node).read_text(encoding="utf-8"))
            compatible = record_compatible(record, snapshot)
            if compatible and record.get("signature") == snapshot.node(node):
                pending_checks = snapshot.generated_checks(node, record.get("generated"))
                if record.get("implementation") != snapshot.file(__file__):
                    # Read-only cache metadata must not prevent valid reuse.
                    with contextlib.suppress(OSError):
                        _retag_record(_record_path(node), record, snapshot.file(__file__))
                reusable.append((library_env, node, pending_checks))
                continue
        except (OSError, ValueError, TypeError, AttributeError, CannotReuse):
            pass
        if isinstance(record, dict):
            if not record_compatible(record, snapshot):
                # An upgraded bookkeeping format requires a fresh archive
                # record, not recompilation of otherwise current objects.
                # Normal SCons dependency checks still build any stale/missing
                # objects before this forced archive action records the result.
                library_env.AlwaysBuild(node)
                node.attributes.library_reuse_forced = True
                continue
            # Content checks must not be defeated by preserved timestamps. Only
            # changed objects are forced; unaffected objects remain incremental.
            library_env.AlwaysBuild(node)
            node.attributes.library_reuse_forced = True
            old_objects = record.get("objects", {})
            if not isinstance(old_objects, dict):
                old_objects = {}
            for child in _objects(node):
                try:
                    unchanged = old_objects.get(_identity(child.get_abspath())) == snapshot.object_signature(
                        child, record.get("generated")
                    )
                except (OSError, ValueError, TypeError, AttributeError, CannotReuse):
                    unchanged = False
                if not unchanged:
                    library_env.AlwaysBuild(child)
                    child.attributes.library_reuse_forced = True
    for library_env, node, pending_checks in reusable:
        for child in _objects(node):
            # Godot defaults to building the directory tree. An object must not
            # be rebuilt just because that traversal finds it. Explicit object
            # targets and other consumers still work.
            env.Ignore(child.dir, child)
        node.attributes.library_reuse_signature = snapshot.node(node)
        node.builder_set(None)
        node.sources = []
        node.sources_set = set()
        node.depends = []
        node.depends_set = set()
        node.implicit = []
        node.implicit_set = set()
        node.clear_memoized_values()
        if pending_checks:
            from SCons.Action import Action

            data = json.dumps({"record": str(_record_path(node)), "outputs": pending_checks}, sort_keys=True)
            guard = library_env.Command(
                node.get_abspath() + ".reuse-check",
                [library_env.Value(data)] + [snapshot.generated_nodes[p] for p in pending_checks],
                Action(_validate_generated, "Checking generated inputs for cached library $TARGET ..."),
            )
            library_env.AlwaysBuild(guard)
            # A previous successful stamp cannot replace a fresh byte check.
            library_env.NoCache(guard)
            library_env.Depends(node, guard)
    if reusable:
        print("Reusing %d verified static libraries." % len(reusable))
