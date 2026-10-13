"""CLI integration tests using generated images and a small, known fax pattern."""

import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("pdg_test_module", ROOT / "pdg_converter.py")
converter = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = converter
spec.loader.exec_module(converter)


def make_pdg(payload: bytes, width=13, height=9) -> bytes:
    header = bytearray(140)
    header[:4] = b"HH\x02\x00"
    struct.pack_into("<I", header, 8, 12)
    struct.pack_into("<HH", header, 16, width, height)
    struct.pack_into("<II", header, 24, len(header), len(payload))
    return bytes(header) + payload


def code_for(tree, value, prefix=""):
    if not isinstance(tree, list):
        return prefix if tree == value else None
    for bit, child in enumerate(tree):
        result = code_for(child, value, prefix + str(bit))
        if result is not None:
            return result
    return None


def fax_fixture():
    # Each 13-pixel row is black(3), white(5), black(5), white(0).
    horizontal = code_for(converter.MODE, "h")
    row = (horizontal + code_for(converter.BLACK, 3) + code_for(converter.WHITE, 5)
           + horizontal + code_for(converter.BLACK, 5) + code_for(converter.WHITE, 0))
    bits = row * 9
    bits += "0" * (-len(bits) % 8)
    payload = int(bits, 2).to_bytes(len(bits) // 8, "big")
    image = Image.new("RGB", (13, 9), "black")
    for y in range(9):
        for x in range(3, 8):
            image.putpixel((x, y), (255, 255, 255))
    return make_pdg(payload), image


def png_fixture():
    image = Image.new("RGB", (13, 9), (17, 82, 191))
    image.putpixel((0, 0), (255, 0, 0))
    buffer = io.BytesIO()
    image.save(buffer, format="PNG")
    return buffer.getvalue(), image


class ConverterCases:
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="pdg_test_")
        self.addCleanup(self.temporary.cleanup)
        base = Path(self.temporary.name)
        self.input = base / "输入 空格"
        self.output = base / "输出 空格"
        self.input.mkdir()

    def run_converter(self, *options, expected=0):
        result = subprocess.run(
            [*self.command, str(self.input), str(self.output), *options],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            timeout=60,
        )
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def assert_pixels(self, path, expected):
        with Image.open(path) as image:
            self.assertEqual(image.size, expected.size)
            self.assertEqual(image.convert("RGB").tobytes(), expected.tobytes())

    def test_fax_pixels_and_forced_formats(self):
        data, expected = fax_fixture()
        (self.input / "page.pdg").write_bytes(data)
        for format_name, extension in [("png", "png"), ("tiff", "tiff"), ("bmp", "bmp")]:
            with self.subTest(format=format_name):
                self.run_converter("--format", format_name)
                self.assert_pixels(self.output / f"page.{extension}", expected)
        self.run_converter("--format", "jpg")
        with Image.open(self.output / "page.jpg") as image:
            self.assertEqual(image.size, expected.size)

    def test_real_format_extraction_and_padding(self):
        png, expected = png_fixture()
        (self.input / "embedded.pdg").write_bytes(make_pdg(png + b"padding"))
        (self.input / "direct.pdg").write_bytes(png)
        self.run_converter()
        for name in ("embedded.png", "direct.png"):
            self.assertEqual((self.output / name).read_bytes(), png)
            self.assert_pixels(self.output / name, expected)

    def test_recursive_and_single_directory(self):
        nested = self.input / "book"
        nested.mkdir()
        png, _ = png_fixture()
        (nested / "page.PDG").write_bytes(png)
        self.run_converter("--no-recursive")
        self.assertFalse((self.output / "book/page.png").exists())
        self.run_converter()
        self.assertEqual((self.output / "book/page.png").read_bytes(), png)

    def test_skip_and_overwrite(self):
        png, _ = png_fixture()
        (self.input / "page.pdg").write_bytes(png)
        self.output.mkdir()
        target = self.output / "page.png"
        target.write_bytes(b"keep existing file")
        self.run_converter()
        self.assertEqual(target.read_bytes(), b"keep existing file")
        self.run_converter("--overwrite")
        self.assertEqual(target.read_bytes(), png)

    def test_summary_and_report_options(self):
        png, _ = png_fixture()
        (self.input / "page.pdg").write_bytes(png)
        for summary in (False, True):
            for reports in (False, True):
                with self.subTest(summary=summary, reports=reports):
                    self.output = Path(self.temporary.name) / f"options_{summary}_{reports}"
                    result = self.run_converter(
                        "--summary" if summary else "--no-summary",
                        "--reports" if reports else "--no-reports",
                    )
                    self.assertIn("[1/1]", result.stdout)
                    self.assertEqual(bool(list(self.output.glob("*.json"))), reports)
                    self.assertEqual(bool(list(self.output.glob("*.csv"))), reports)
                    ending = "Finished:" in result.stdout or "处理结束：" in result.stdout
                    self.assertEqual(ending, summary)
                    if reports:
                        report = json.loads(next(self.output.glob("*.json")).read_text(encoding="utf-8"))
                        self.assertEqual(report["summary"]["success"], 1)

    def test_bad_file_does_not_stop_good_file(self):
        png, _ = png_fixture()
        (self.input / "bad.pdg").write_bytes(b"HH\x02")
        (self.input / "good.pdg").write_bytes(png)
        result = self.run_converter(expected=2)
        self.assertIn("bad.pdg", result.stderr)
        self.assertEqual((self.output / "good.png").read_bytes(), png)

    def test_multiframe_auto_preserves_animation(self):
        frames = [Image.new("RGB", (13, 9), color) for color in ("red", "blue")]
        buffer = io.BytesIO()
        frames[0].save(buffer, format="GIF", save_all=True, append_images=frames[1:],
                       duration=100, loop=0)
        data = buffer.getvalue()
        (self.input / "animation.pdg").write_bytes(make_pdg(data))
        self.run_converter()
        self.assertEqual((self.output / "animation.gif").read_bytes(), data)
        with Image.open(self.output / "animation.gif") as image:
            self.assertEqual(image.n_frames, 2)
        self.run_converter("--format", "png", expected=2)
        self.assertFalse((self.output / "animation.png").exists())

    def test_invalid_input_and_same_directory(self):
        self.input = Path(self.temporary.name) / "missing"
        self.run_converter(expected=1)
        self.input.mkdir()
        self.output = self.input
        self.run_converter(expected=1)

    def test_default_paths_are_beside_program(self):
        base = Path(self.temporary.name) / "portable"
        default_input = base / "input"
        default_input.mkdir(parents=True)
        png, _ = png_fixture()
        (default_input / "page.pdg").write_bytes(png)
        unrelated_cwd = Path(self.temporary.name) / "different_cwd"
        unrelated_cwd.mkdir()
        if len(self.command) == 1:
            program = base / "converter.exe"
            shutil.copy2(self.command[0], program)
            command = [str(program)]
        else:
            program = base / "converter.py"
            shutil.copy2(self.command[1], program)
            command = [self.command[0], str(program)]
        result = subprocess.run(command, cwd=unrelated_cwd, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((base / "converted_images/page.png").read_bytes(), png)
        self.assertFalse(list((base / "converted_images").glob("*.json")))


class PythonConverterTests(ConverterCases, unittest.TestCase):
    command = [sys.executable, str(ROOT / "pdg_converter.py")]


class NativeConverterTests(ConverterCases, unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = Path(os.environ.get("PDG_CONVERTER_EXE", ROOT / "pdg_converter.exe"))
        if sys.platform != "win32" or not path.is_file():
            raise unittest.SkipTest("Windows C converter EXE has not been built")
        cls.command = [str(path.resolve())]

    def test_python_folder_call_example(self):
        png, _ = png_fixture()
        for name in ("book_a", "book_b"):
            folder = self.input / name
            folder.mkdir()
            (folder / "page.pdg").write_bytes(png)
        result = subprocess.run(
            [sys.executable, str(ROOT / "examples/call_c_from_python.py"),
             "--exe", self.command[0], "--input", str(self.input), "--output", str(self.output)],
            capture_output=True, text=True, encoding="utf-8", timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(result.stdout.count("[1/1]"), 2)
        for name in ("book_a", "book_b"):
            self.assertEqual((self.output / name / "page.png").read_bytes(), png)


if __name__ == "__main__":
    unittest.main()
