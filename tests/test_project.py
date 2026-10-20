"""Small, game-free checks for extraction, migrated regressions and synthetic audio."""
import contextlib
import io
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import extract
import check_lut3d
import check_model_consumed
import check_title_quarter


class Section:
    def __init__(self, data, declared_size=None):
        self.data = data
        self.data_size = len(data) if declared_size is None else declared_size

    def read(self, offset, size):
        return self.data[offset:offset + size]


def bmp(path, black_quarter=False):
    width = height = 8
    pixels = b"".join(bytes((0, 0, 0, 255) if black_quarter and x < 4 and y < 4 else (180, 100, 80, 255))
                      for y in range(height) for x in range(width))
    path.write_bytes(struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54)
                     + struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 32, 0, len(pixels), 0, 0, 0, 0) + pixels)


class ProjectTests(unittest.TestCase):
    def test_build_paths_are_inside_checkout(self):
        import cc
        import recomp
        self.assertEqual(Path(cc.RUNTIME), ROOT / "runtime")
        self.assertEqual(Path(recomp.ROOT), ROOT)
        self.assertEqual(Path(recomp.TREE), ROOT)
        self.assertIn(0x71091796F0, recomp.hle_wraps())

    def test_extraction_uses_one_input_and_portable_config(self):
        program = SimpleNamespace(content_type=0, title_id=0x1000, f=io.BytesIO(),
                                  section=lambda kind: Section(b"abc" if kind == 1 else b"synthetic RomFS"))
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            root = Path(directory)
            with patch.object(extract, "nsp_ncas", return_value=[("program", program)]), \
                 patch.object(extract, "pfs0_entries", return_value=[("main", 0, 1), ("sdk", 1, 1), ("subsdk0", 2, 1)]):
                extract.extract("synthetic.nsp", "unused-keys", root)
            self.assertEqual((root / "exefs_base/main").read_bytes(), b"a")
            self.assertEqual((root / "romfs/romfs.bin").read_bytes(), b"synthetic RomFS")
            self.assertEqual((root / "dump/modules/romfs.cfg").read_text(), "romfs/romfs.bin\n0\n")
            self.assertTrue(program.f.closed)
            with self.assertRaises(FileExistsError):
                extract.extract("unused.nsp", "unused-keys", root)

    def test_extraction_rejects_update_and_unsafe_paths(self):
        for title, names in ((0x1800, ["main"]), (0x1000, ["../main", "sdk", "subsdk0"]),
                             (0x1000, ["main", "sdk", "sdk", "subsdk0"])):
            with self.subTest(title=title, names=names), tempfile.TemporaryDirectory() as directory:
                program = SimpleNamespace(content_type=0, title_id=title, f=io.BytesIO(), section=lambda _: Section(b"abc"))
                with patch.object(extract, "nsp_ncas", return_value=[("program", program)]), \
                     patch.object(extract, "pfs0_entries", return_value=[(name, 0, 1) for name in names]):
                    with self.assertRaises(ValueError):
                        extract.extract("synthetic.nsp", "unused-keys", directory)
                self.assertFalse((Path(directory) / "exefs_base").exists())
                self.assertTrue(program.f.closed)

    def test_romfs_short_read_is_not_published(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "romfs.bin"
            with self.assertRaises(OSError):
                extract.write_romfs(Section(b"short", declared_size=10), path)
            self.assertFalse(path.exists())
            self.assertTrue(path.with_name("romfs.bin.partial").exists())
            with self.assertRaises(ValueError):
                extract.write_romfs(Section(b""), path)

    def test_romfs_never_overwrites_existing_data(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "romfs.bin"
            path.write_bytes(b"keep")
            with self.assertRaises(FileExistsError):
                extract.write_romfs(Section(b"replacement"), path)
            self.assertEqual(path.read_bytes(), b"keep")

    def test_lut_check_rejects_collapsed_blue_axis(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            path = Path(directory) / "lut_2x2x2.txt"
            for collapsed, expected in ((False, True), (True, False)):
                path.write_text("# 2x2x2\n" + "".join(f"{x*255} {y*255} {0 if collapsed else z*255} 255\n"
                                                      for z in range(2) for y in range(2) for x in range(2)))
                self.assertEqual(check_lut3d.check(str(path)), expected)

    def test_title_check_rejects_black_quarter(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            path = Path(directory) / "title.bmp"
            bmp(path)
            self.assertTrue(check_title_quarter.check(str(path)))
            bmp(path, black_quarter=True)
            self.assertFalse(check_title_quarter.check(str(path)))

    def test_model_check_rejects_missing_alias_and_surface(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            root = Path(directory)
            log = root / "err.txt"
            lines = [f"[t26] tex va 0x{i+1:x} -> guest 0x{guest} | TIC 8x8 fmt 0x8 type 1 | surface guest 0x{guest} 8x8 fmt 0x8 depth 0 | match\n"
                     for i, guest in enumerate(check_model_consumed.MODEL_GUESTS)]
            for guest in check_model_consumed.MODEL_GUESTS:
                bmp(root / f"surf_000_{guest}_synthetic.bmp")
            with patch.object(sys, "argv", ["check", str(log), str(root)]):
                log.write_text("".join(lines))
                self.assertEqual(check_model_consumed.main(), 0)
                log.write_text("".join(lines[1:]))
                self.assertEqual(check_model_consumed.main(), 1)
                log.write_text("".join(lines))
                (root / f"surf_000_{check_model_consumed.MODEL_GUESTS[0]}_synthetic.bmp").unlink()
                self.assertEqual(check_model_consumed.main(), 1)

    @unittest.skipUnless(os.name == "nt" and os.environ.get("INCLUDE"), "Requires a VS x64 developer environment")
    def test_audio_vectors_in_c_without_game_or_device(self):
        import cc
        with tempfile.TemporaryDirectory(prefix="xc2-audio-") as directory:
            obj = str(Path(directory) / "audio.obj")
            exe = str(Path(directory) / "audio.exe")
            flags = cc.CFLAGS[:]
            try:
                cc.CFLAGS.append("/Gy")
                cc.compile_one(str(ROOT / "tests/audio_vectors.c"), obj)
                cc.link([obj], exe, ["/OPT:REF"])
            finally:
                cc.CFLAGS[:] = flags
            result = subprocess.run([exe], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("vectors: PASS (0 failure(s))", result.stderr)


if __name__ == "__main__":
    unittest.main()
