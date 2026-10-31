"""Run: WOW_PROJECT_ROOT="/path/to/wow project" python3 -B src/cmake/tests/test_install_path_spaces.py."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]


class InstallPathSafetyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        project = os.environ.get("WOW_PROJECT_ROOT")
        if not project:
            raise RuntimeError("Set WOW_PROJECT_ROOT to the WoW project root")
        project_root = Path(project).resolve(strict=True)
        subprocess.run([str(project_root / "scripts/wow-build-host-guard")], check=True)
        cls.work = project_root / "work"
        if not cls.work.is_dir():
            raise RuntimeError("WoW project work directory is missing")

    def test_only_selected_libraries_are_removed(self):
        for relative, marker in (
            ('src/server/scripts/CMakeLists.txt', 'if(DISABLED_SCRIPT_MODULE_PROJECTS)'),
            ('modules/CMakeLists.txt', 'if (DISABLED_SCRIPT_MODULE_PROJECTS)'),
        ):
            with self.subTest(source=relative), tempfile.TemporaryDirectory(prefix="cmake-path-fixture-", dir=self.work) as directory:
                root = Path(directory)
                source, build = root / 'fixture', root / 'build'
                source.mkdir()
                sentinel = root / 'install'
                sentinel.write_text('untouched')
                libraries = root / 'install prefix with spaces'
                libraries.mkdir()
                first, second, sibling = (libraries / name for name in
                                          ('first.so', 'second.so', 'keep.so'))
                for path in (first, second, sibling):
                    path.write_text('fixture')
                missing = libraries / 'missing.so'

                text = (ROOT / relative).read_text()
                start = text.index(marker)
                end = text.index('\nendif()', start) + len('\nendif()')
                selected = ';'.join(map(str, (first, second, missing)))
                (source / 'CMakeLists.txt').write_text(
                    'cmake_minimum_required(VERSION 3.20)\nproject(PathSafety NONE)\n'
                    f'set(DISABLED_SCRIPT_MODULE_PROJECTS "{selected}")\n'
                    + text[start:end] + '\n'
                )
                subprocess.run(['cmake', '-S', str(source), '-B', str(build),
                                f'-DCMAKE_INSTALL_PREFIX={root / "output"}'],
                               check=True, stdout=subprocess.DEVNULL)
                subprocess.run(['cmake', '--install', str(build)],
                               check=True, stdout=subprocess.DEVNULL)
                self.assertFalse(first.exists())
                self.assertFalse(second.exists())
                self.assertFalse(missing.exists())
                self.assertEqual(sentinel.read_text(), 'untouched')
                self.assertEqual(sibling.read_text(), 'fixture')


if __name__ == '__main__':
    unittest.main()
