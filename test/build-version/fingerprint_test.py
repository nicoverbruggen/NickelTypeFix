import runpy
import shutil
import tempfile
import unittest
from pathlib import Path


fingerprint = runpy.run_path(
    str(Path(__file__).resolve().parents[2] / "tools/build_fingerprint.py")
)["fingerprint"]


class FingerprintTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "source"
        self.inputs = (
            "Makefile", "build.sh", "tools/build_fingerprint.py", "src/main.cc",
            "src/main.h", "NickelHook/nh.c", "NickelHook/NickelHook.h",
            "NickelHook/NickelHook.mk", "res/doc", "res/uninstall",
        )
        for name in self.inputs:
            self.write(name, name.encode())

    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def test_each_build_input_changes_the_fingerprint(self):
        before = fingerprint(self.root)
        self.assertRegex(before, r"^[0-9a-f]{12}$")
        for name in self.inputs:
            with self.subTest(name=name):
                self.write(name, name.encode() + b" changed")
                self.assertNotEqual(fingerprint(self.root), before)
                self.write(name, name.encode())
                self.assertEqual(fingerprint(self.root), before)

    def test_new_and_renamed_sources_are_included(self):
        before = fingerprint(self.root)
        self.write("src/new.cc", b"new source")
        added = fingerprint(self.root)
        self.assertNotEqual(added, before)
        (self.root / "src/new.cc").rename(self.root / "src/renamed.cc")
        self.assertNotEqual(fingerprint(self.root), added)

    def test_generated_files_and_unrelated_docs_are_ignored(self):
        before = fingerprint(self.root)
        for name in ("src/main.o", "src/libnickeltypefix.so", "src/form.moc",
                     "NickelHook/nhplugin.json", "KoboRoot.tgz", ".git/HEAD",
                     "README.md", "test/example.cc", "tmp/source.cc"):
            self.write(name, b"not a build input")
        self.assertEqual(fingerprint(self.root), before)

    def test_relocation_does_not_change_the_fingerprint(self):
        moved = Path(self.temp.name) / "moved"
        shutil.copytree(self.root, moved)
        self.assertEqual(fingerprint(self.root), fingerprint(moved))

    def test_missing_required_input_fails(self):
        (self.root / "Makefile").unlink()
        with self.assertRaises(FileNotFoundError):
            fingerprint(self.root)


if __name__ == "__main__":
    unittest.main()
