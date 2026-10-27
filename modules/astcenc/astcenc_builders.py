#!/usr/bin/env python

from pathlib import Path


def make_isolated_source(target, source, env):
    source_name = source[0].name
    text = (
        '// Generated ASTC implementation wrapper; do not edit.\n'
        '#include "astcenc_godot.h"\n\n'
        'namespace godot_astcenc {\n'
        f'#include "{source_name}"\n'
        '} // namespace godot_astcenc\n'
    )
    Path(str(target[0])).write_bytes(text.encode("utf-8"))
