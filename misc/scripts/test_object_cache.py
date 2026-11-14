"""Real SCons/linker checks for retaining direct-link object exceptions."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[2]
SCONSTRUCT = '''\
import json
import os
import sys
from pathlib import Path
sys.path.insert(0, {root!r})
import methods
from misc.utility import library_cache, object_cache
env = Environment(ENV=dict(os.environ))
env['library_reuse'] = True
env.Decider('content')
env['CCCOMSTR'] = 'TEST_COMPILE $SOURCE'
env['SHCCCOMSTR'] = 'TEST_COMPILE $SOURCE'
env['ARCOMSTR'] = 'TEST_ARCHIVE $TARGET'
env['LINKCOMSTR'] = 'TEST_LINK $TARGET'
env['SHLINKCOMSTR'] = 'TEST_LINK $TARGET'
env.AddMethod(methods.add_library, 'add_library')
shared = env.Object('shared.c')
library = env.add_library('fixture', [env.Object('value.c'), shared])
if ARGUMENTS.get('depends_extra', 'no') == 'yes':
    env.Depends(library, env.Object('extra.c'))
program = env.Program('check', [env.Object('main.c'), shared], LIBS=library)
if ARGUMENTS.get('ignore_shared', 'no') == 'yes':
    env.Ignore(program, shared)
plugin = env.SharedLibrary('plugin', env.SharedObject('plugin.c'))
Default(program, plugin)
if ARGUMENTS.get('plan', 'no') == 'yes':
    result = object_cache.plan(library_cache._libraries, list(program) + list(plugin))
    Path('object-plan.json').write_text(json.dumps(result))
    Exit(0)
library_cache.finalize(env)
'''


class DirectObjectRetentionTests(unittest.TestCase):
    def setUp(self):
        self.root = ROOT / 'bin' / ('object-retention-test-' + uuid.uuid4().hex)
        self.root.mkdir()
        assert self.root.resolve().parent == (ROOT / 'bin').resolve() and not self.root.is_symlink()
        self.addCleanup(shutil.rmtree, self.root)
        (self.root / 'SConstruct').write_text(SCONSTRUCT.format(root=str(ROOT)))
        for name, source in {
            'value.c': 'int value(void) { return 1; }\n',
            'shared.c': 'int shared(void) { return 2; }\n',
            'main.h': '#define OFFSET 0\n',
            'main.c': '#include "main.h"\nint value(void); int shared(void); int main(void) { return value()+shared()+OFFSET; }\n',
            'plugin.c': 'int plugin_value(void) { return 9; }\n',
        }.items():
            (self.root / name).write_text(source)
        self.scons()
        self.program = self.root / ('check.exe' if os.name == 'nt' else 'check')

    def scons(self, *args):
        result = subprocess.run([sys.executable, '-m', 'SCons', '-Q', '-j2', *args], cwd=self.root,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def purge_archived_only(self):
        output = self.scons('plan=yes', '-n')
        self.assertNotIn('TEST_COMPILE', output)
        plan = json.loads((self.root / 'object-plan.json').read_text())
        removed = [Path(p) for p in plan['removable_archived_objects']]
        retained = [Path(p) for p in plan['direct_link_objects']]
        self.assertEqual([p.stem for p in removed], ['value'])
        self.assertEqual({p.stem for p in retained}, {'main', 'shared', 'plugin'})
        self.assertTrue(all(p.is_file() for p in retained))
        for path in removed:
            self.assertTrue(path.resolve().is_relative_to(self.root.resolve()))
            path.unlink()
        return retained

    def test_fresh_link_keeps_direct_and_shared_objects(self):
        retained = self.purge_archived_only()
        before = {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in retained}
        self.program.unlink()  # Require a real link, not an up-to-date shortcut.
        output = self.scons()
        self.assertIn('Reusing 1 verified static libraries', output)
        self.assertIn('TEST_LINK', output)
        self.assertNotIn('TEST_COMPILE', output)
        self.assertNotIn('TEST_ARCHIVE', output)
        self.assertEqual(before, {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in retained})
        self.assertEqual(subprocess.run([str(self.program)], cwd=self.root).returncode, 3)

    def test_changed_direct_header_still_recompiles(self):
        self.purge_archived_only()
        header = self.root / 'main.h'
        before = header.stat()
        header.write_text('#define OFFSET 4\n')
        os.utime(header, ns=(before.st_atime_ns, before.st_mtime_ns))
        output = self.scons()
        self.assertIn('TEST_COMPILE main.c', output)
        self.assertNotIn('TEST_COMPILE shared.c', output)
        self.assertNotIn('TEST_COMPILE value.c', output)
        self.assertNotIn('TEST_ARCHIVE', output)
        self.assertEqual(subprocess.run([str(self.program)], cwd=self.root).returncode, 7)

    def test_unverified_archive_does_not_allow_object_removal(self):
        (self.root / 'value.c').write_text('int value(void) { return 8; }\n')
        self.scons('plan=yes', '-n')
        plan = json.loads((self.root / 'object-plan.json').read_text())
        self.assertEqual(plan['removable_archived_objects'], [])
        self.assertFalse(plan['archives'][0]['verified_reusable'])

    def test_archive_dependency_is_not_mistaken_for_a_member(self):
        (self.root / 'extra.c').write_text('int extra(void) { return 6; }\n')
        self.scons('depends_extra=yes')
        self.scons('depends_extra=yes', 'plan=yes', '-n')
        plan = json.loads((self.root / 'object-plan.json').read_text())
        self.assertEqual([Path(p).stem for p in plan['removable_archived_objects']], ['value'])
        self.assertTrue(plan['archives'][0]['verified_reusable'])

    def test_ignored_dependency_is_still_a_direct_link_source(self):
        self.scons('ignore_shared=yes', 'plan=yes', '-n')
        plan = json.loads((self.root / 'object-plan.json').read_text())
        self.assertIn('shared', {Path(p).stem for p in plan['direct_link_objects']})
        self.assertNotIn('shared', {Path(p).stem for p in plan['removable_archived_objects']})


if __name__ == '__main__':
    unittest.main()
