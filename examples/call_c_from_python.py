#!/usr/bin/env python3
"""按 PDG 文件夹调用 C 转换器；无需 Pillow，可在 IDE 中直接运行。"""

import argparse
from pathlib import Path
import subprocess
import sys


# 在 IDE 中运行时修改这里。C 程序应先编译到项目根目录。
PROJECT_DIR = Path(__file__).resolve().parents[1]
EXE_PATH = PROJECT_DIR / "pdg_converter.exe"
INPUT_ROOT = PROJECT_DIR / "input"
OUTPUT_ROOT = PROJECT_DIR / "converted_images"


def convert_folders(exe_path: Path, input_root: Path, output_root: Path) -> int:
    exe_path = exe_path.expanduser().resolve()
    input_root = input_root.expanduser().resolve()
    output_root = output_root.expanduser().resolve()
    if not exe_path.is_file():
        raise FileNotFoundError(f"请先编译 C 转换器：{exe_path}")
    if not input_root.is_dir():
        raise NotADirectoryError(input_root)
    if input_root == output_root:
        raise ValueError("输入与输出目录不能相同")

    folders = sorted({
        path.parent
        for path in input_root.rglob("*")
        if path.is_file() and path.suffix.lower() == ".pdg"
        and not path.resolve().is_relative_to(output_root)
    })
    if not folders:
        print(f"没有找到 PDG 文件：{input_root}")
        return 0

    exit_code = 0
    for folder in folders:
        output_dir = output_root / folder.relative_to(input_root)
        command = [str(exe_path), str(folder), str(output_dir), "--no-recursive"]
        print(f"转换目录：{folder}", flush=True)
        with subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        ) as process:
            for line in process.stdout:
                print(line, end="", flush=True)
            code = process.wait()
        if code == 1:
            return 1
        if code:
            exit_code = code
    return exit_code


def main(argv=None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=EXE_PATH)
    parser.add_argument("--input", type=Path, default=INPUT_ROOT)
    parser.add_argument("--output", type=Path, default=OUTPUT_ROOT)
    args = parser.parse_args(argv)
    try:
        return convert_folders(args.exe, args.input, args.output)
    except (ValueError, OSError) as exc:
        print(f"无法启动转换：{exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
