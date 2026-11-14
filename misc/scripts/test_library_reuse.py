"""Run real compiler/SCons regression checks in automatically removed projects.

Usage: python misc/scripts/test_library_reuse.py
Requires SCons and GCC (MinGW on Windows) on PATH. On Windows set
LIBRARY_REUSE_TEST_TOOL=default to check the installed MSVC toolchain instead.
"""

import json
import os
import shutil
import subprocess
import sys
import unittest
import uuid
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = os.environ.get("LIBRARY_REUSE_TEST_TOOL", "mingw" if os.name == "nt" else "default")
EMSCRIPTEN = os.environ.get("LIBRARY_REUSE_TEST_EMSCRIPTEN") == "yes"
PROGRAM_SUFFIX = ".js" if EMSCRIPTEN else ".exe" if os.name == "nt" else ""

SCONSTRUCT = '''\
import os
import sys
sys.path.insert(0, {root!r})
import methods
from misc.utility import library_cache

env = Environment(tools=[{tool!r}], ENV=dict(os.environ))
if os.environ.get('LIBRARY_REUSE_TEST_EMSCRIPTEN') == 'yes':
    env.Replace(CC='emcc', CXX='em++', AR='emar', RANLIB='emranlib', LINK='emcc', PROGSUFFIX='.js')
    env.Append(LINKFLAGS=['-sENVIRONMENT=node'])
env['library_reuse'] = ARGUMENTS.get('reuse', 'yes') == 'yes'
env['verbose'] = True
env['CCCOMSTR'] = 'TEST_COMPILE $SOURCE'
env['ARCOMSTR'] = 'TEST_ARCHIVE $TARGET'
env['LINKCOMSTR'] = 'TEST_LINK $TARGET'
if ARGUMENTS.get('cache', 'no') == 'yes':
    env.CacheDir('scons-cache')
env.Append(CPPPATH=['.'], CPPDEFINES=['OFFSET=' + ARGUMENTS.get('offset', '0')])
env.Decider(os.environ.get('LIBRARY_REUSE_TEST_DECIDER', 'content'))
env.AddMethod(methods.add_library, 'add_library')
tag = ARGUMENTS.get('variant', '')
tag = tag + '-' if tag else ''
env.File(ARGUMENTS.get('header_alias', 'nested.h'))

def generate(target, source, env):
    with open(str(source[0])) as f:
        value = f.read().strip()
    with open(str(target[0]), 'w') as f:
        f.write('#define GENERATED ' + value + '\\n')
env.Command('generated.h', 'input.txt', Action(generate, 'TEST_GENERATE'))
sources = [env.Object(tag + 'value', 'value.c'), env.Object(tag + 'helper', 'helper.c')]
if ARGUMENTS.get('extra', 'no') == 'yes':
    sources.append(env.Object('extra.c'))
library = env.add_library(tag + 'fixture', sources)
# Added after registration, as in core/SCsub.
env.Depends(library, 'explicit.txt')
libraries = [library]
if ARGUMENTS.get('two_libs', 'no') == 'yes':
    libraries.append(env.add_library('unrelated', [env.Object('unrelated.c')]))
program = env.Program(tag + 'check', [env.Object(tag + 'main', 'main.c')], LIBS=libraries)
if ARGUMENTS.get('default', 'yes') == 'yes':
    Default(program)
library_cache.finalize(env)
'''


@unittest.skipUnless(
    TOOL == "default" and os.name == "nt" or shutil.which("gcc") and shutil.which("ar"), "GCC and ar are required"
)
class LibraryReuseTests(unittest.TestCase):
    def setUp(self):
        self.root = ROOT / "bin" / ("library-reuse-test-" + uuid.uuid4().hex)
        self.root.mkdir()
        if self.root.resolve().parent != (ROOT / "bin").resolve() or self.root.is_symlink():
            raise RuntimeError("Test directory escaped the workspace")
        self.addCleanup(shutil.rmtree, self.root)
        self.write("SConstruct", SCONSTRUCT.format(root=str(ROOT), tool=TOOL))
        self.write(
            "value.c",
            '#include "value.h"\n#include "generated.h"\nint value(void) { return VALUE + GENERATED + OFFSET; }\n',
        )
        self.write("value.h", '#include "nested.h"\n#define VALUE NESTED\n')
        self.write("nested.h", "#define NESTED 3\n")
        self.write("helper.c", "int helper(void) { return 0; }\n")
        self.write("main.c", "int value(void); int helper(void); int main(void) { return value() + helper(); }\n")
        self.write("extra.c", "int extra(void) { return 1; }\n")
        self.write("unrelated.c", "int unrelated(void) { return 0; }\n")
        self.write("input.txt", "2\n")
        self.write("explicit.txt", "original\n")
        output = self.run_scons()
        self.library = next(path for path in self.root.iterdir() if path.suffix in (".a", ".lib"))
        self.record = Path(str(self.library) + ".reuse.json")
        self.assertTrue(self.record.is_file(), "Successful archive must create a reuse record\n" + output)

    def write(self, name, content):
        (self.root / name).write_text(content, encoding="utf-8")

    def run_scons(self, *arguments, success=True, environment=None):
        result = subprocess.run(
            [sys.executable, "-m", "SCons", "-Q", "-j2", *arguments],
            cwd=self.root,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result.stdout

    def remove_objects(self):
        for path in self.root.iterdir():
            if path.suffix in (".o", ".obj"):
                path.unlink()

    def check_program(self, code):
        self.check_variant_program('', code)

    def check_variant_program(self, variant, code):
        executable = self.root / ((variant + '-' if variant else '') + 'check' + PROGRAM_SUFFIX)
        command = ['node', str(executable)] if EMSCRIPTEN else [str(executable)]
        self.assertEqual(subprocess.run(command, cwd=self.root).returncode, code)

    def assert_rebuilt(self, output):
        self.assertIn("TEST_COMPILE value.c", output)
        self.assertIn("TEST_ARCHIVE", output)

    def test_reuse_after_deleting_objects_and_executable(self):
        self.remove_objects()
        (self.root / ('check' + PROGRAM_SUFFIX)).unlink()
        before = self.library.stat().st_mtime_ns
        output = self.run_scons()
        self.assertIn("Reusing 1 verified static libraries", output)
        self.assertNotIn("TEST_COMPILE value.c", output)
        self.assertNotIn("TEST_COMPILE helper.c", output)
        self.assertIn("TEST_LINK", output)
        self.assertFalse(any((self.root / ("value" + suffix)).exists() for suffix in (".o", ".obj")))
        self.assertEqual(self.library.stat().st_mtime_ns, before)
        self.check_program(5)
        # The old SCons build information must remain usable on further runs.
        self.assertNotIn("TEST_ARCHIVE", self.run_scons())

    def test_source_edit(self):
        self.remove_objects()
        self.write("value.c", "int value(void) { return 9; }\n")
        self.assert_rebuilt(self.run_scons())
        self.check_program(9)

    def test_unrelated_path_entry_preserves_library(self):
        extra = self.root / "unrelated-tools"
        extra.mkdir()
        environment = dict(os.environ)
        environment["PATH"] = str(extra) + os.pathsep + environment["PATH"]
        self.remove_objects()
        output = self.run_scons(environment=environment)
        self.assertIn("Reusing 1 verified static libraries", output)
        self.assertNotIn("TEST_COMPILE value.c", output)
        self.assertNotIn("TEST_ARCHIVE", output)
        self.check_program(5)

    @unittest.skipUnless(os.name == "nt", "Windows case-insensitive file identity")
    def test_windows_header_alias_preserves_library(self):
        self.remove_objects()
        output = self.run_scons("header_alias=NESTED.H")
        self.assertIn("Reusing 1 verified static libraries", output)
        self.assertNotIn("TEST_COMPILE value.c", output)
        self.assertNotIn("TEST_ARCHIVE", output)
        self.check_program(5)

    def test_wsl_transport_and_shell_depth_preserve_library(self):
        self.remove_objects()
        environment = dict(os.environ)
        environment["WSL_INTEROP"] = "/run/WSL/12345_interop"
        environment["SHLVL"] = "37"
        output = self.run_scons(environment=environment)
        self.assertIn('Reusing 1 verified static libraries', output)
        self.assertNotIn('TEST_COMPILE value.c', output)
        self.assertNotIn('TEST_ARCHIVE', output)
        self.check_program(5)

    def test_generated_output_from_another_configuration(self):
        self.run_scons("variant=a")
        archive = next(p for p in self.root.iterdir() if "a-fixture" in p.name and p.suffix in (".a", ".lib"))
        before = archive.read_bytes()
        self.write('input.txt', '6\n')
        self.run_scons('variant=b')
        self.write('input.txt', '2\n')
        for p in self.root.glob('a-*'):
            if p.suffix in (".o", ".obj") and not p.stem.endswith("main"):
                p.unlink()
        output = self.run_scons('variant=a')
        self.assertIn('Reusing 1 verified static libraries', output)
        self.assertNotIn('TEST_COMPILE value.c', output)
        self.assertNotIn('TEST_COMPILE helper.c', output)
        self.assertEqual(archive.read_bytes(), before)
        self.assertIn("#define GENERATED 2", (self.root / "generated.h").read_text())
        self.check_variant_program('a', 5)

    def test_custom_build_environment_still_invalidates_library(self):
        self.remove_objects()
        environment = dict(os.environ)
        environment['LIBRARY_REUSE_CUSTOM_BUILD_INPUT'] = 'changed'
        self.assert_rebuilt(self.run_scons(environment=environment))

    def test_manually_changed_generated_header_rebuilds(self):
        self.remove_objects()
        self.write('generated.h', '#define GENERATED 9\n')
        self.assert_rebuilt(self.run_scons())
        self.check_program(12)

    def test_multi_output_generator_preserves_library(self):
        path = self.root / "SConstruct"
        source = path.read_text()
        source = source.replace("env.Command('generated.h',", "env.Command(['generated.h', 'generated-extra.h'],")
        source = source.replace(
            "env.Command(['generated.h',",
            "    with open(str(target[1]), 'w') as f:\n"
            "        f.write('#define EXTRA 4\\n')\n\nenv.Command(['generated.h',",
        )
        path.write_text(source)
        self.run_scons()
        self.assertTrue(self.record.is_file())
        self.remove_objects()
        output = self.run_scons()
        self.assertIn("Reusing 1 verified static libraries", output)
        self.assertNotIn("TEST_ARCHIVE", output)
        self.assertNotIn("TEST_COMPILE value.c", output)
        self.check_program(5)

    def test_generator_mismatch_blocks_link_and_invalidates_record(self):
        path = self.root / "SConstruct"
        source = path.read_text().replace(
            "    with open(str(target[0]), 'w') as f:",
            "    if os.path.isfile('untracked-output.txt'):\n"
            "        with open('untracked-output.txt') as f:\n"
            "            value = f.read().strip()\n    with open(str(target[0]), 'w') as f:",
        )
        path.write_text(source)
        self.run_scons('variant=a')
        self.write("input.txt", "6\n")
        self.run_scons('variant=b')
        self.write("input.txt", "2\n")
        self.write("untracked-output.txt", "8\n")
        for p in self.root.glob('a-*'):
            if p.suffix in (".o", ".obj") and not p.stem.endswith("main"):
                p.unlink()
        program = self.root / ("a-check" + PROGRAM_SUFFIX)
        program.unlink()
        record = next(p for p in self.root.glob("*a-fixture*.reuse.json"))
        output = self.run_scons("variant=a", success=False)
        self.assertIn("Generated output differs from the cached build", output)
        self.assertNotIn("TEST_LINK", output)
        self.assertFalse(record.exists())
        self.assertFalse(program.exists())
        self.run_scons('variant=a')
        self.check_variant_program('a', 11)

    def test_generated_validation_runs_with_scons_cache(self):
        self.run_scons('variant=a', 'cache=yes')
        for _iteration in range(2):
            self.write('input.txt', '6\n')
            self.run_scons('variant=b', 'cache=yes')
            self.write('input.txt', '2\n')
            for p in self.root.glob('a-*'):
                if p.suffix in ('.o', '.obj') and not p.stem.endswith('main'):
                    p.unlink()
            output = self.run_scons('variant=a', 'cache=yes')
            self.assertIn('Verified regenerated inputs for cached archive.', output)
            self.assertNotIn('TEST_ARCHIVE', output)
            self.assertNotIn('TEST_COMPILE value.c', output)
            self.check_variant_program('a', 5)

    def test_explicit_archive_target_only_validates_selected_library(self):
        self.run_scons('two_libs=yes')
        self.remove_objects()
        output = self.run_scons('two_libs=yes', self.library.name)
        self.assertIn('Reusing 1 verified static libraries', output)
        self.assertNotIn('Reusing 2', output)
        self.assertNotIn('TEST_COMPILE', output)
        self.assertNotIn('TEST_ARCHIVE', output)

    def test_explicit_program_target_keeps_all_linked_libraries(self):
        self.run_scons('two_libs=yes')
        self.remove_objects()
        program = "check" + PROGRAM_SUFFIX
        (self.root / program).unlink()
        output = self.run_scons("two_libs=yes", program)
        self.assertIn('Reusing 2 verified static libraries', output)
        self.assertNotIn('TEST_COMPILE value.c', output)
        self.assertNotIn('TEST_COMPILE unrelated.c', output)
        self.assertNotIn('TEST_ARCHIVE', output)
        self.assertIn('TEST_LINK', output)
        self.check_program(5)

    def test_explicit_directory_target_preserves_library(self):
        self.remove_objects()
        output = self.run_scons("default=no", ".")
        self.assertIn('Reusing 1 verified static libraries', output)
        self.assertNotIn('TEST_COMPILE value.c', output)
        self.assertNotIn('TEST_ARCHIVE', output)
        self.check_program(5)

    def test_known_compatible_metadata_upgrade_preserves_archive(self):
        import hashlib

        record = json.loads(self.record.read_text())
        record["implementation"] = "2e6ccfbef09114f7f7f450b938a3c8ac4d3a3aff0e1e13d72ee768383664a90d"
        self.record.write_text(json.dumps(record))
        before = self.library.read_bytes()
        self.remove_objects()
        output = self.run_scons()
        self.assertIn('Reusing 1 verified static libraries', output)
        self.assertNotIn('TEST_COMPILE value.c', output)
        self.assertNotIn('TEST_ARCHIVE', output)
        self.assertEqual(before, self.library.read_bytes())
        self.assertEqual(
            json.loads(self.record.read_text())["implementation"],
            hashlib.sha256((ROOT / "misc/utility/library_cache.py").read_bytes()).hexdigest(),
        )

    def test_malformed_implementation_identity_falls_back(self):
        record = json.loads(self.record.read_text())
        record["implementation"] = []
        self.record.write_text(json.dumps(record))
        self.remove_objects()
        self.assert_rebuilt(self.run_scons())
        self.check_program(5)

    @unittest.skipIf(os.name == "nt" and TOOL == "default", "GNU archiver path-switch fixture")
    def test_path_selecting_different_tool_invalidates_library(self):
        extra = self.root / "different-tools"
        extra.mkdir()
        archiver = Path(shutil.which("ar"))
        shutil.copy2(archiver, extra / archiver.name)
        environment = dict(os.environ)
        environment["PATH"] = str(extra) + os.pathsep + environment["PATH"]
        output = self.run_scons(environment=environment)
        self.assertIn("TEST_ARCHIVE", output)
        self.assertNotIn("Reusing 1", output)
        self.check_program(5)

    def test_record_upgrade_preserves_current_objects(self):
        record = json.loads(self.record.read_text(encoding="utf-8"))
        record["version"] = 1
        self.record.write_text(json.dumps(record), encoding="utf-8")
        output = self.run_scons()
        self.assertIn("TEST_ARCHIVE", output)
        self.assertNotIn("TEST_COMPILE", output)
        self.assertEqual(json.loads(self.record.read_text())["version"], 3)

    def test_implementation_update_preserves_current_objects(self):
        record = json.loads(self.record.read_text(encoding="utf-8"))
        record["implementation"] = "old implementation"
        self.record.write_text(json.dumps(record), encoding="utf-8")
        output = self.run_scons()
        self.assertIn("TEST_ARCHIVE", output)
        self.assertNotIn("TEST_COMPILE", output)

    def test_connected_library_does_not_rebuild(self):
        self.run_scons("two_libs=yes")
        other = next(
            path for path in self.root.iterdir() if "unrelated" in path.name and path.suffix in (".a", ".lib")
        )
        before = other.stat().st_mtime_ns
        self.remove_objects()
        self.write("value.c", "int unrelated(void); int value(void) { return unrelated() + 9; }\n")
        output = self.run_scons("two_libs=yes")
        self.assert_rebuilt(output)
        self.assertNotIn("TEST_COMPILE unrelated.c", output)
        self.assertFalse((self.root / "unrelated.o").exists())
        self.assertFalse((self.root / "unrelated.obj").exists())
        self.assertEqual(other.stat().st_mtime_ns, before)
        self.check_program(9)

    def test_default_directory_build(self):
        self.remove_objects()
        output = self.run_scons("default=no")
        self.assertIn("Reusing 1 verified static libraries", output)
        self.assertNotIn("TEST_COMPILE value.c", output)
        self.assertNotIn("TEST_COMPILE helper.c", output)
        self.check_program(5)

    def test_nested_header_edit_with_preserved_timestamp(self):
        self.remove_objects()
        path = self.root / "nested.h"
        before = path.stat()
        self.write("nested.h", "#define NESTED 7\n")
        os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.assert_rebuilt(self.run_scons())
        self.check_program(9)

    def test_header_edit_preserves_unaffected_object_and_refreshes_record(self):
        path = self.root / "nested.h"
        before = path.stat()
        self.write("nested.h", "#define NESTED 7\n")
        os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns))
        output = self.run_scons()
        self.assert_rebuilt(output)
        self.assertNotIn("TEST_COMPILE helper.c", output)
        self.check_program(9)
        self.remove_objects()
        output = self.run_scons()
        self.assertIn("Reusing 1 verified static libraries", output)
        self.assertNotIn("TEST_COMPILE value.c", output)

    def test_generated_header_input_edit(self):
        self.remove_objects()
        self.write("input.txt", "6\n")
        output = self.run_scons()
        self.assert_rebuilt(output)
        self.assertIn("TEST_GENERATE", output)
        self.check_program(9)

    def test_missing_generated_header(self):
        self.remove_objects()
        (self.root / "generated.h").unlink()
        self.assert_rebuilt(self.run_scons())
        self.check_program(5)

    def test_compiler_options(self):
        self.remove_objects()
        self.assert_rebuilt(self.run_scons("offset=4"))
        self.check_program(9)

    def test_explicit_dependency_added_after_registration(self):
        self.remove_objects()
        self.write("explicit.txt", "changed\n")
        self.assert_rebuilt(self.run_scons())

    def test_added_archive_member(self):
        self.remove_objects()
        self.assert_rebuilt(self.run_scons("extra=yes"))
        self.assertIn(b"extra.o", self.library.read_bytes())

    def test_missing_record(self):
        self.remove_objects()
        self.record.unlink()
        output = self.run_scons()
        self.assertIn("TEST_COMPILE value.c", output)
        self.assertNotIn("Reusing", output)

    def test_malformed_record(self):
        self.remove_objects()
        self.record.write_text("{broken", encoding="utf-8")
        output = self.run_scons()
        self.assertIn("TEST_COMPILE value.c", output)
        self.assertNotIn("Reusing", output)

    def test_old_record_version(self):
        self.remove_objects()
        record = json.loads(self.record.read_text(encoding="utf-8"))
        record["version"] = -1
        self.record.write_text(json.dumps(record), encoding="utf-8")
        output = self.run_scons()
        self.assertIn("TEST_COMPILE value.c", output)
        self.assertNotIn("Reusing", output)

    def test_archive_changed(self):
        self.remove_objects()
        with self.library.open("ab") as stream:
            stream.write(b"changed")
        self.assert_rebuilt(self.run_scons())

    def test_normal_mode(self):
        self.remove_objects()
        self.assert_rebuilt(self.run_scons("reuse=no"))
        self.check_program(5)

    def test_failed_build_does_not_refresh_record(self):
        before = self.record.read_bytes()
        self.write("value.c", "not valid C !\n")
        self.run_scons(success=False)
        self.assertEqual(self.record.read_bytes(), before)

    def test_clean_removes_archive_and_record(self):
        self.run_scons("-c")
        self.assertFalse(self.library.exists())
        self.assertFalse(self.record.exists())


if __name__ == "__main__":
    unittest.main()
