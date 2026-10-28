#!/usr/bin/env python3
"""Build Godot's pinned Mesa NIR source subset for the Windows D3D12 backend."""

import argparse
import ast
import hashlib
import json
import os
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import urllib.request
from pathlib import Path, PurePosixPath


def digest(path):
    checksum = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def is_linked(path):
    return path.is_symlink() or (
        os.name == "nt" and path.exists() and bool(path.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT)
    )


def rename_directory(source, destination):
    # A newly created DLL/library tree can briefly be held by Windows scanning.
    for attempt in range(5):
        try:
            source.rename(destination)
            return
        except PermissionError:
            if attempt == 4:
                raise
            time.sleep(0.1 * 2**attempt)


def extract(archive, destination, selected=None):
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as source:
        for member in source:
            parts = PurePosixPath(member.name).parts[1:]
            if not parts or not member.isfile():
                continue
            relative = PurePosixPath(*parts).as_posix()
            if selected is not None and relative not in selected and not relative.startswith("src/compiler/spirv/"):
                continue
            target = destination.joinpath(*parts)
            if not target.resolve().is_relative_to(destination.resolve()):
                raise ValueError("Archive path outside source directory")
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.extractfile(member) as data, target.open("wb") as output:
                shutil.copyfileobj(data, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", required=True, type=Path)
    parser.add_argument("--arch", default="x86_64", choices=["x86_64", "arm64", "x86_32"])
    parser.add_argument("--toolchain", default="msvc", choices=["msvc", "gcc", "llvm"])
    parser.add_argument("--mingw-prefix", default="")
    parser.add_argument("--download-cache", type=Path)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    script_dir = Path(__file__).resolve().parent
    manifest_file = script_dir / "mesa_nir_sources.json"
    manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
    patch = script_dir / "mesa_nir_26.2.5.patch"
    if digest(patch) != manifest["patch_sha256"]:
        raise ValueError("Mesa integration patch checksum mismatch")
    if is_linked(args.destination):
        raise ValueError("Refusing to replace a linked dependency directory")
    destination = args.destination.resolve()
    cache = args.download_cache or destination.parent / "source_cache"
    cache.mkdir(parents=True, exist_ok=True)
    archives = {}
    for name, url, expected in [
        ("mesa-26.2.5.tar.xz", manifest["mesa_url"], manifest["mesa_sha256"]),
        ("godot-nir-" + manifest["wrapper_commit"] + ".tar.gz", manifest["wrapper_url"], manifest["wrapper_sha256"]),
    ]:
        target = cache / name
        if not target.exists():
            temporary = target.with_suffix(target.suffix + ".download")
            with urllib.request.urlopen(url, timeout=120) as source, temporary.open("wb") as output:
                shutil.copyfileobj(source, output)
            if digest(temporary) != expected:
                raise ValueError("Downloaded archive checksum mismatch: " + name)
            temporary.replace(target)
        if digest(target) != expected:
            raise ValueError("Cached archive checksum mismatch: " + name)
        archives["mesa" if name.startswith("mesa-") else "wrapper"] = target
    key = digest(manifest_file)[:16]
    work = (
        args.work_dir or destination.parent / ".mesa-nir-build" / (key + "-" + args.toolchain + "-" + args.arch)
    ).resolve()
    work.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment["PYTHONUTF8"] = "1"
    if args.mingw_prefix:
        environment["PATH"] = str(Path(args.mingw_prefix) / "bin") + os.pathsep + environment.get("PATH", "")
    python_deps = work / "python-deps"
    if not (python_deps / "mako").exists() or not (python_deps / "yaml").exists():
        subprocess.run(
            [
                sys.executable,
                "-m",
                "pip",
                "install",
                "--target",
                str(python_deps),
                "Mako==1.4.3",
                "MarkupSafe==3.0.4",
                "PyYAML==6.0.3",
            ],
            check=True,
        )
    environment["PYTHONPATH"] = str(python_deps) + os.pathsep + environment.get("PYTHONPATH", "")
    source = work / "mesa"
    subset = work / "godot-mesa"
    ready = work / "sources-ready.json"
    if ready.exists() and json.loads(ready.read_text())["manifest_sha256"] != digest(manifest_file):
        raise ValueError("Work directory belongs to a different Mesa integration")
    if not ready.exists():
        extract(archives["wrapper"], work)
        extract(archives["mesa"], source, set(manifest["copy_sources"]) | {"VERSION", "bin/git_sha1_gen.py"})
        for relative in manifest["copy_sources"]:
            target = subset / relative
            if not target.resolve().is_relative_to(subset.resolve()):
                raise ValueError("Source manifest path outside Mesa subset")
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / relative, target)
        shutil.copyfile(source / "VERSION", subset / "VERSION.info")
        (subset / "bin").mkdir(parents=True, exist_ok=True)
        initial_steps = [
            ("bin", ["git_sha1_gen.py", "--output", str(subset / "bin/git_sha1.h")]),
            (
                "src/compiler/spirv",
                [
                    "spirv_info_gen.py",
                    "--json",
                    "spirv.core.grammar.json",
                    "--out-h",
                    str(subset / "src/compiler/spirv/spirv_info.h"),
                    "--out-c",
                    str(subset / "src/compiler/spirv/spirv_info.c"),
                ],
            ),
            (
                "src/compiler/spirv",
                [
                    "vtn_gather_types_c.py",
                    "spirv.core.grammar.json",
                    str(subset / "src/compiler/spirv/vtn_gather_types.c"),
                ],
            ),
        ]
        for directory, command in initial_steps:
            subprocess.run([sys.executable, *command], cwd=source / directory, env=environment, check=True)
        # Prevent Git from discovering the engine checkout above this build folder.
        environment["GIT_CEILING_DIRECTORIES"] = str(work.parent)
        subprocess.run(["git", "apply", "--check", str(patch)], cwd=work, env=environment, check=True)
        subprocess.run(["git", "apply", str(patch)], cwd=work, env=environment, check=True)
        ready.write_text(json.dumps({"manifest_sha256": digest(manifest_file)}) + "\n")
    generated = work / "generated-ready.json"
    if not generated.exists():
        tree = ast.parse((work / "SConstruct").read_text(encoding="utf-8"))
        steps = next(
            ast.literal_eval(node.value)
            for node in tree.body
            if isinstance(node, ast.Assign)
            and any(isinstance(target, ast.Name) and target.id == "custom_build_steps" for target in node.targets)
        )
        for directory, command, _ in steps:
            output_dir = subset / "generated" / directory
            output_dir.mkdir(parents=True, exist_ok=True)
            arguments = [arg.replace("%s", str(output_dir)) for arg in shlex.split(command)]
            if ">" in arguments:
                redirect = arguments.index(">")
                with Path(arguments[redirect + 1]).open("wb") as output:
                    subprocess.run(
                        [sys.executable, *arguments[:redirect]],
                        cwd=subset / directory,
                        env=environment,
                        stdout=output,
                        check=True,
                    )
            else:
                subprocess.run([sys.executable, *arguments], cwd=subset / directory, env=environment, check=True)
        generated.write_text(json.dumps({"manifest_sha256": digest(manifest_file)}) + "\n")
    subprocess.run(
        [
            sys.executable,
            "-m",
            "SCons",
            "-Q",
            "platform=windows",
            "arch=" + args.arch,
            "use_mingw=" + ("no" if args.toolchain == "msvc" else "yes"),
            "use_llvm=" + ("yes" if args.toolchain == "llvm" else "no"),
        ],
        cwd=work,
        env=environment,
        check=True,
    )
    output = work / "bin" / ("libNIR.windows." + args.arch + (".lib" if args.toolchain == "msvc" else ".a"))
    if not output.is_file():
        raise FileNotFoundError(output)
    files = {}
    for path in subset.rglob("*"):
        if path.is_file() and path.suffix not in [".obj", ".o", ".pyc"]:
            files[Path("godot-mesa") / path.relative_to(subset)] = path
    files[Path("bin") / output.name] = output
    receipt = {
        "version": manifest["mesa_version"],
        "manifest_sha256": digest(manifest_file),
        "library_sha256": digest(output),
        "arch": args.arch,
        "toolchain": args.toolchain,
    }
    previous_receipt = destination / "mesa-source-build.json"
    if (
        previous_receipt.is_file()
        and json.loads(previous_receipt.read_text()) == receipt
        and all(
            (destination / relative).is_file() and digest(destination / relative) == digest(path)
            for relative, path in files.items()
        )
    ):
        print("Mesa NIR package already matches; preserved library and header timestamps.")
        return
    # Build the entire replacement first. Do not leave headers from an older Mesa
    # release mixed with the new library, and retain the previous package for recovery.
    destination.parent.mkdir(parents=True, exist_ok=True)
    if is_linked(destination):
        raise ValueError("Refusing to replace a linked dependency directory")
    staging = destination.with_name(destination.name + ".staging-" + str(time.time_ns()))
    staging.mkdir()
    for relative, path in files.items():
        target = staging / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
    (staging / "mesa-source-build.json").write_text(json.dumps(receipt, indent=2) + "\n")
    backup = None
    if destination.exists():
        backup = destination.with_name(destination.name + ".previous-" + str(time.time_ns()))
        if backup.resolve().parent != destination.resolve().parent:
            raise ValueError("Backup path outside dependency parent")
        rename_directory(destination, backup)
    try:
        rename_directory(staging, destination)
    except OSError:
        if backup is not None:
            rename_directory(backup, destination)
        raise
    print("Installed Mesa NIR", manifest["mesa_version"], "in", destination)


if __name__ == "__main__":
    main()
