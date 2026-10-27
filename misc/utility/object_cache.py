"""Plan loose-object removal while preserving inputs needed outside archives.

This module does not delete files or change SCons' dependency decisions.
"""

import json
import os
from pathlib import Path

from misc.utility import library_cache


OBJECT_SUFFIXES = {".o", ".obj", ".os"}
ARCHIVE_SUFFIXES = {".a", ".lib"}


def _path(node):
    return os.path.normcase(node.get_abspath())


def plan(libraries, link_targets):
    """Return graph-derived candidates and exceptions, without build actions.

    Only actual sources of a verified reusable archive are eligible. Explicit
    archive dependencies are not necessarily members. Walk final-link inputs
    through generated/combined objects, stopping at static-library boundaries.
    An object shared between an archive and a direct link must remain loose.
    """
    if not link_targets:
        raise ValueError("No final-link targets were captured; remove nothing")
    direct = set()
    pending = list(link_targets)
    visited = set()
    while pending:
        node = pending.pop()
        if node in visited:
            continue
        visited.add(node)
        if hasattr(node, "get_abspath"):
            suffix = Path(node.get_abspath()).suffix
            if suffix in ARCHIVE_SUFFIXES:
                continue
            if suffix in OBJECT_SUFFIXES:
                direct.add(_path(node))
        # Ignore() suppresses dependency traversal, not the command's source
        # list. A linker input must still be kept even if deliberately ignored.
        pending.extend(getattr(node, "sources", ()))
        pending.extend(node.children())

    snapshot = library_cache.Snapshot()
    covered = set()
    archives = []
    for _, node in libraries:
        row = {"path": node.get_abspath(), "verified_reusable": False}
        try:
            record = json.loads(library_cache._record_path(node).read_text(encoding="utf-8"))
            reusable = (
                library_cache.record_compatible(record, snapshot)
                and record.get("signature") == snapshot.node(node)
            )
            if reusable:
                row["pending_generated_checks"] = len(snapshot.generated_checks(node, record.get("generated")))
                with Path(node.get_abspath()).open("rb") as stream:
                    reusable = stream.read(8) == b"!<arch>\n"
            row["verified_reusable"] = reusable
            if reusable:
                # Do not use node.children(): Depends(obj) alone does not put
                # that object's bytes into an archive.
                members = {
                    _path(source)
                    for source in node.sources
                    if hasattr(source, "get_abspath") and Path(source.get_abspath()).suffix in OBJECT_SUFFIXES
                }
                row["object_sources"] = sorted(members)
                covered.update(members)
            else:
                row["reason"] = "Archive or recorded inputs do not match"
        except (OSError, ValueError, TypeError, AttributeError, library_cache.CannotReuse) as error:
            row["reason"] = str(error)
        archives.append(row)
    return {
        "dry_run_graph_only": True,
        "link_targets": sorted(node.get_abspath() for node in link_targets),
        "direct_link_objects": sorted(direct),
        "removable_archived_objects": sorted(covered - direct),
        "archives": archives,
    }
