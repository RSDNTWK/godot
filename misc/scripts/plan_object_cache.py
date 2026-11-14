"""Export an object-retention plan for the supplied ordinary SCons options.

Example: python misc/scripts/plan_object_cache.py --manifest bin/objects.json --
         platform=windows target=template_release arch=x86_64 library_reuse=yes

This loads the build graph and exits before SCons executes any build actions.
"""

import argparse
import json
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("scons_options", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    destination = args.manifest.resolve()
    if not destination.is_relative_to(root / "bin"):
        parser.error("Manifest must be inside this checkout's bin directory")
    options = args.scons_options
    if options[:1] == ["--"]:
        options = options[1:]
    if "library_reuse=yes" not in options:
        parser.error("Supply library_reuse=yes and the exact normal build options")
    sys.path.insert(0, str(root))
    import SCons.Builder
    import SCons.Script
    from misc.utility import library_cache, object_cache

    links = []

    original_call = SCons.Builder.BuilderBase.__call__

    def capture(builder, env, *positional, **keywords):
        targets = original_call(builder, env, *positional, **keywords)
        if builder.get_name(env) in ("Program", "SharedLibrary"):
            links.extend(targets)
        return targets

    # SConstruct explicitly reloads methods.py, so hook the SCons builders in
    # this read-only process rather than replacing those temporary helpers.
    SCons.Builder.BuilderBase.__call__ = capture

    def export(env):
        result = object_cache.plan(library_cache.selected_libraries(env), links)
        result["scons_options"] = options
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print("Object retention graph exported; no compile/link actions executed.")
        env.Exit(0)

    library_cache.finalize = export
    # -n is an additional safeguard; export() exits during graph construction.
    sys.argv = ["scons", "-n", *options]
    SCons.Script.main()
    if not destination.is_file():
        raise RuntimeError("No graph manifest was produced; remove nothing")


if __name__ == "__main__":
    main()
