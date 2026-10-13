#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PDG Converter：支持命令行，也可修改配置区后在 IDE 中直接运行。

依赖：Pillow（在 IDE 的 Python 环境中运行 python -m pip install Pillow）。
本批 HH v2 / 0x02、0x11 黑白页已通过 222 页逐像素验证。
CCITT 标准码树来源为 pdfminer.six，许可见 licenses/pdfminer_CCITT_LICENSE.txt。
无需运行 Pdg2Pic.exe，也不依赖 Ghidra / pdfminer。
"""

from pathlib import Path

# ==================== 用户配置：主要修改这里 ====================
INPUT_DIR = Path(__file__).resolve().parent / "input"
OUTPUT_DIR = Path(__file__).resolve().parent / "converted_images"

RECURSIVE = True             # True：包括子文件夹，输出保留相对目录结构
OUTPUT_FORMAT = "auto"      # auto / png / tiff / bmp / jpg / webp
OVERWRITE_EXISTING = False   # False：跳过已有输出；True：覆盖
SHOW_SUMMARY = False        # 最后的成功、失败、跳过与耗时汇总
WRITE_REPORTS = False       # CSV / JSON 报告；逐文件进度始终显示
PNG_DPI = (300, 300)         # 传真页没有独立 DPI 来源；沿用已验证样本的 300 DPI
JPEG_QUALITY = 95           # 仅强制输出 JPG 时使用；auto 保留 JPEG 原始压缩数据
MAX_FILE_BYTES = 256 * 1024 * 1024
MAX_IMAGE_PIXELS = 100_000_000
# ===============================================================

import argparse
import csv
import hashlib
import io
import json
import os
import re
import shutil
import struct
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from datetime import datetime, timedelta, timezone
from typing import Optional

try:
    from PIL import Image
except ImportError as exc:
    raise SystemExit("缺少 Pillow。请在 IDE 使用的 Python 环境中安装：python -m pip install Pillow") from exc


class ConversionError(ValueError):
    """文件损坏或无法识别。"""


class UnsupportedPdgError(ConversionError):
    """识别到了 PDG，但其版本、图层或编码尚未实现。"""


@dataclass
class ConvertedImage:
    format: str
    width: Optional[int]
    height: Optional[int]
    method: str
    encoding: str = ""
    frames: int = 1
    original_bytes: Optional[bytes] = None
    bitmap: Optional[Image.Image] = None


@dataclass
class FileResult:
    input: str
    output: str = ""
    status: str = ""
    format: str = ""
    width: Optional[int] = None
    height: Optional[int] = None
    frames: int = 1
    encoding: str = ""
    method: str = ""
    seconds: float = 0.0
    error: str = ""


# 标准 CCITT T.4/T.6 码树；运行时无需 pdfminer。
MODE = [[[[[[[[[[[[None, [[[[[[[[[[[[None, 'e'], None], None], None], None], None], None], None], None], None], None], None]], None], None], None], None], [[['x1', 'x2'], ['x3', 'x4']], [['x5', 'x6'], ['x7', 'u']]]], [-3, 3]], [-2, 2]], 'p'], 'h'], [-1, 1]], 0]
WHITE = [[[[[[[[None, [[[1792, [1984, 2048]], [[2112, 2176], [2240, 2304]]], [[1856, 1920], [[2368, 2432], [2496, 2560]]]]], [29, 30]], [[45, 46], 22]], [[23, [47, 48]], 13]], [[[20, [33, 34]], [[35, 36], [37, 38]]], [[19, [31, 32]], 1]]], [[[12, [[53, 54], 26]], [[[39, 40], [41, 42]], [[43, 44], 21]]], [[[28, [61, 62]], [[63, 0], [320, 384]]], 10]]], [[[11, [[27, [59, 60]], [[[1472, 1536], [1600, 1728]], 18]]], [[[24, [49, 50]], [[51, 52], 25]], [[[55, 56], [57, 58]], 192]]], [[[1664, [[448, 512], [[704, 768], 640]]], [[[576, [832, 896]], [[960, 1024], [1088, 1152]]], [[[1216, 1280], [1344, 1408]], 256]]], 2]]], [[[3, [128, 8]], [[9, [16, 17]], 4]], [[5, [[14, 15], 64]], [6, 7]]]]
BLACK = [[[[[[[[None, [[[1792, [1984, 2048]], [[2112, 2176], [2240, 2304]]], [[1856, 1920], [[2368, 2432], [2496, 2560]]]]], [[[18, [[52, [640, 704]], [[768, 832], 55]]], [[[56, [1280, 1344]], [[1408, 1472], 59]], [[60, [1536, 1600]], 24]]], [[[25, [[1664, 1728], 320]], [[384, 448], [[512, 576], 53]]], [[[54, [896, 960]], [[1024, 1088], [1152, 1216]]], 64]]]], [[13, [[[23, [50, 51]], [[44, 45], [46, 47]]], [[[57, 58], [61, 256]], 16]]], [[[17, [[48, 49], [62, 63]]], [[[30, 31], [32, 33]], [[40, 41], 22]]], 14]]], [[10, 11], [[[15, [[[128, 192], [26, 27]], [[28, 29], 19]]], [[[20, [34, 35]], [[36, 37], [38, 39]]], [[21, [42, 43]], 0]]], 12]]], [[9, 8], 7]], [6, 5]], [1, 4]], [3, 2]]


def check_dimensions(width: int, height: int) -> None:
    if width <= 0 or height <= 0:
        raise ConversionError(f"无效图像尺寸：{width} × {height}")
    if width * height > MAX_IMAGE_PIXELS:
        raise ConversionError(f"图像超过 MAX_IMAGE_PIXELS：{width} × {height}")


def signature_format(data: bytes) -> Optional[str]:
    """只在载荷起点判断真实图像签名，不在任意位置搜索。"""
    if data.startswith(b"\xff\xd8\xff"):
        return "JPEG"
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return "PNG"
    if data.startswith((b"II*\x00", b"MM\x00*", b"II+\x00", b"MM\x00+")):
        return "TIFF"
    if data.startswith(b"BM"):
        return "BMP"
    if data.startswith((b"GIF87a", b"GIF89a")):
        return "GIF"
    if len(data) >= 12 and data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "WEBP"
    if data.startswith(b"\x00\x00\x00\x0cjP  \r\n\x87\n"):
        return "JPEG2000"
    if data.startswith(b"\xffO\xffQ"):
        return "J2K"
    if data.startswith(b"AT&TFORM"):
        return "DJVU"
    return None


def jpeg_end(data: bytes) -> int:
    """按 JPEG marker 读取到真正的 EOI，保留 APP/EXIF，忽略 PDG 尾部填充。"""
    position = 2
    while position < len(data):
        if data[position] != 0xFF:
            raise ConversionError("JPEG 标记结构损坏")
        while position < len(data) and data[position] == 0xFF:
            position += 1
        if position >= len(data):
            break
        marker = data[position]
        position += 1
        if marker == 0xD9:
            return position
        if marker == 0x01 or 0xD0 <= marker <= 0xD8:
            continue
        if marker == 0 or position + 2 > len(data):
            raise ConversionError("JPEG 标记或长度损坏")
        size = int.from_bytes(data[position:position + 2], "big")
        if size < 2 or position + size > len(data):
            raise ConversionError("JPEG 数据截断")
        position += size
        if marker == 0xDA:  # 扫描数据中的 FF00 和 restart marker 不结束图像
            while True:
                position = data.find(b"\xff", position)
                if position < 0:
                    raise ConversionError("JPEG 缺少结束标记")
                next_position = position + 1
                while next_position < len(data) and data[next_position] == 0xFF:
                    next_position += 1
                if next_position >= len(data):
                    raise ConversionError("JPEG 扫描数据截断")
                next_marker = data[next_position]
                if next_marker == 0 or 0xD0 <= next_marker <= 0xD7:
                    position = next_position + 1
                    continue
                break
    raise ConversionError("JPEG 缺少 EOI 标记")


def trim_container(data: bytes, format_name: str) -> bytes:
    """仅对长度明确的容器移除 PDG 填充；TIFF/GIF/JP2 保留完整载荷。"""
    if format_name == "JPEG":
        return data[:jpeg_end(data)]
    if format_name == "PNG":
        position = 8
        while position + 12 <= len(data):
            size = int.from_bytes(data[position:position + 4], "big")
            end = position + size + 12
            if end > len(data):
                raise ConversionError("PNG 数据截断")
            if data[position + 4:position + 8] == b"IEND":
                if size != 0:
                    raise ConversionError("PNG IEND 长度无效")
                return data[:end]
            position = end
        raise ConversionError("PNG 缺少 IEND")
    if format_name in {"BMP", "WEBP", "DJVU"}:
        if format_name == "BMP":
            if len(data) < 14:
                raise ConversionError("BMP 文件头截断")
            end = int.from_bytes(data[2:6], "little")
            minimum = 14
        elif format_name == "WEBP":
            end = 8 + int.from_bytes(data[4:8], "little")
            minimum = 12
        else:
            if len(data) < 16 or data[12:16] not in {b"DJVU", b"DJVM"}:
                raise UnsupportedPdgError("DjVu FORM 类型不是独立图像或文档")
            end = 12 + int.from_bytes(data[8:12], "big")
            minimum = 16
        if not minimum <= end <= len(data):
            raise ConversionError(f"{format_name} 声明的文件长度无效")
        return data[:end]
    return data


def standard_image(data: bytes, encoding: str = "") -> Optional[ConvertedImage]:
    format_name = signature_format(data)
    if format_name is None:
        return None
    data = trim_container(data, format_name)
    if format_name == "DJVU":
        # 只提取 DjVu 容器，不将其误称为已经解码的 PNG。
        return ConvertedImage(format_name, None, None, "提取原始 DjVu 容器", encoding,
                              original_bytes=data)
    try:
        with Image.open(io.BytesIO(data)) as image:
            check_dimensions(*image.size)
            width, height = image.size
            frames = getattr(image, "n_frames", 1)
            image.verify()
        # JPEG 的 verify() 不解码扫描数据；实际读取像素才能发现截断或损坏。
        with Image.open(io.BytesIO(data)) as image:
            for frame in range(frames):
                image.seek(frame)
                check_dimensions(*image.size)
                image.load()
    except ConversionError:
        raise
    except (OSError, ValueError, SyntaxError, Image.DecompressionBombError) as exc:
        raise ConversionError(f"{format_name} 图像校验失败：{exc}") from exc
    return ConvertedImage(format_name, width, height, "提取原始图像载荷", encoding,
                          frames, original_bytes=data)


def xor_prefix(data: bytes, key: bytes, count: int) -> bytes:
    result = bytearray(data)
    for index in range(count):
        result[index] ^= key[index % len(key)]
    return bytes(result)


def decrypt_blocks(data: bytes, key: bytes) -> bytes:
    """Pdg2Pic 的 16 字节 / 16 轮 TEA 类逆变换；不完整尾块保持不变。"""
    k0, k1, k2, k3 = struct.unpack("<4I", key)
    mask = 0xFFFFFFFF
    result = bytearray(data)
    for position in range(0, len(data) - 15, 16):
        v0, v1, v2, v3 = struct.unpack_from("<4I", result, position)
        total = 0xE3779B90
        for _ in range(16):
            v3 = (v3 - (((v0 >> 5) + k1) ^ ((v0 << 4) + k2) ^ (v0 + total))) & mask
            v2 = (v2 - (((v3 << 4) + k0) ^ ((v3 >> 5) + k3) ^ (v3 + total))) & mask
            v1 = (v1 - (((v2 >> 5) + k3) ^ ((v2 << 4) + k2) ^ (v2 + total))) & mask
            v0 = (v0 - (((v1 << 4) + k0) ^ ((v1 >> 5) + k1) ^ (v1 + total))) & mask
            total = (total - 0x9E3779B9) & mask
        struct.pack_into("<4I", result, position, v0, v1, v2, v3)
    return bytes(result)


def restore_sparse(data: bytes, width: int, height: int, parameter: int) -> bytes:
    length = len(data)
    a = width & 255
    d = (width - height) & 255
    s = (width * height) & 255
    b = (length >> 8) & 255
    q = (b * width) & 255
    t = (width | height) & 255
    n = (width + height + length) & 255
    e = (s * a + parameter) & 255
    first = (e + q * b + t * s - n * d + 250) & 511
    second = (t * t + n * d * s - e * q + 250) & 511
    i, j = sorted((first, second))
    if i == j:
        return data
    step = 19 if j - i == 17 else 17
    if max(i + step, j + step) >= length:
        raise ConversionError("载荷太短，无法恢复类型 0x11 扰码")
    result = bytearray(data)
    old_first = result[i]
    result[i] = result[j] ^ result[j + step]
    result[j] = old_first ^ result[i + step]
    return bytes(result)


def restore_payload(header: bytes, payload: bytes, kind: int, width: int, height: int) -> bytes:
    if kind == 0:
        return payload
    if kind == 1:
        return xor_prefix(payload, b"3.141592", len(payload) // 8 * 8)
    if kind == 17:
        if struct.unpack_from("<I", header, 8)[0] != 12:
            raise UnsupportedPdgError("类型 0x11 的元数据布局不是本次解析的偏移 12")
        parameter = struct.unpack_from("<H", header, 0x5E)[0]
        return restore_sparse(payload, width, height, parameter)
    if kind not in {2, 3, 4, 5}:
        raise UnsupportedPdgError(f"暂未实现 PDG 编码类型 0x{kind:02X}")
    material = bytearray(header[0x40:0x70])
    if kind > 2:
        material[0x66 - 0x40] = 0  # 主入口在计算 MD5 前清零这个头部字节
    key = hashlib.md5(bytes(material), usedforsecurity=False).digest()
    if kind == 3:
        key = xor_prefix(key, b"SUPERSTAR4PDG2.0", 8)
    elif kind == 4:
        key = xor_prefix(key, b"SSREADER", 8)
    elif kind == 5:
        key = xor_prefix(key, b"e#fgF%3*", 8)
    return decrypt_blocks(payload, key)


class BitReader:
    def __init__(self, data: bytes):
        self.data = data
        self.position = 0

    def read(self, tree):
        node = tree
        while isinstance(node, list):
            position = self.position
            if position >= len(self.data) * 8:
                raise ConversionError("传真压缩数据截断")
            self.position += 1
            node = node[(self.data[position // 8] >> (7 - position % 8)) & 1]
        if node is None:
            raise ConversionError(f"传真码无效，bit 偏移 {self.position}")
        return node

    def run_length(self, white: bool) -> int:
        length = 0
        while True:
            value = self.read(WHITE if white else BLACK)
            if not isinstance(value, int):
                raise ConversionError("无效传真游程")
            length += value
            if value < 64:
                return length
            if length > 131_072:
                raise ConversionError("传真游程异常过长")


def decode_fax(payload: bytes, width: int, height: int) -> Image.Image:
    """还原黑色初始状态的二维传真流，返回 1 bit 图像。"""
    check_dimensions(width, height)
    reader = BitReader(payload)
    previous = [0, width, width, width, width]
    raster = bytearray()
    padding = (-width) % 8
    row_bytes = (width + 7) // 8
    for row in range(height):
        changes = []
        reference_index = position = 0
        white = False
        operations = 0
        while position < width:
            operations += 1
            if operations > 4 * width + 1024:
                raise ConversionError(f"第 {row + 1} 行传真模式异常过多")
            mode = reader.read(MODE)
            try:
                if mode == "p":
                    reference_index += 1
                    position = previous[reference_index]
                    reference_index += 1
                elif mode == "h":
                    position += reader.run_length(white)
                    changes.append(position)
                    position += reader.run_length(not white)
                    changes.append(position)
                    while reference_index < len(previous) and previous[reference_index] <= position:
                        reference_index += 2
                elif isinstance(mode, int):
                    position = max(0, previous[reference_index] + mode)
                    reference_index += 1
                    changes.append(position)
                    white = not white
                else:
                    raise UnsupportedPdgError(f"第 {row + 1} 行包含未实现的传真扩展模式：{mode}")
            except IndexError as exc:
                raise ConversionError(f"第 {row + 1} 行的传真参考变化点越界") from exc
        bits = 0
        for index in range(0, len(changes) - 1, 2):
            start = min(width, max(0, changes[index]))
            end = min(width, max(0, changes[index + 1]))
            if end > start:
                bits |= ((1 << (end - start)) - 1) << (width - end)
        raster.extend((bits << padding).to_bytes(row_bytes, "big"))
        previous = changes + [position] * 4
    return Image.frombytes("1", (width, height), bytes(raster))


def read_pdg(source: Path) -> ConvertedImage:
    if source.stat().st_size > MAX_FILE_BYTES:
        raise ConversionError("文件超过 MAX_FILE_BYTES")
    data = source.read_bytes()
    # 一些 .pdg 文件实际上就是完整 JPEG 等图像。
    direct = standard_image(data)
    if direct is not None:
        return direct
    if not data.startswith(b"HH"):
        raise UnsupportedPdgError("既不是 HH PDG，也不是可识别的完整图像")
    if len(data) < 3:
        raise ConversionError("PDG 文件头截断")
    if data[2] == 1:
        return read_legacy_pdg(data)
    if data[2] != 2:
        raise UnsupportedPdgError(f"暂未实现 PDG 版本 {data[2]}")
    if len(data) < 128:
        raise ConversionError("PDG v2 文件头截断")
    if data[0x38]:
        raise UnsupportedPdgError("文件使用了尚未实现的外层变换（头部 0x38）")
    if data[0x15]:
        raise UnsupportedPdgError("文件含附加图层，暂不支持完整合成")
    width, height = struct.unpack_from("<HH", data, 0x10)
    offset, length = struct.unpack_from("<II", data, 0x18)
    if offset < 128 or length == 0 or offset + length > len(data):
        raise ConversionError(f"PDG 载荷范围无效：offset={offset}, length={length}")
    payload = data[offset:offset + length]
    kind = data[0x0F]
    encoding = f"HH v2 / 0x{kind:02X}"
    # 已经具有有效图像结构的明文载荷无需再次扰码恢复，例如类型 5 的 DjVu。
    if signature_format(payload):
        try:
            image = standard_image(payload, encoding)
            if image is not None:
                return image
        except ConversionError:
            pass
    payload = restore_payload(data, payload, kind, width, height)
    image = standard_image(payload, encoding)
    if image is not None:
        return image
    if data[0x0E] != 0:
        raise UnsupportedPdgError(f"无法识别头部格式 0x{data[0x0E]:02X} 的图像载荷")
    if kind == 4:
        raise UnsupportedPdgError("类型 0x04 的载荷未识别为标准图像，停止而不猜测为传真流")
    bitmap = decode_fax(payload, width, height)
    return ConvertedImage("FAX", width, height, "恢复 PDG 编码并解码黑白传真流", encoding,
                          bitmap=bitmap)


def read_legacy_pdg(data: bytes) -> ConvertedImage:
    """旧版 HH 01：尺寸在 +3/+5，载荷从 +0x38 起，类型 4/5 删除插入字节。

    来自原程序的旧版分支；目前只有构造样本验证，没有真实 v1 样本。
    """
    if len(data) < 56:
        raise ConversionError("PDG v1 文件头截断")
    width, height = struct.unpack_from("<HH", data, 3)
    kind = struct.unpack_from("<I", data, 7)[0]
    payload = data[56:]
    if kind not in {0, 4, 5}:
        raise UnsupportedPdgError(f"暂未验证的 PDG v1 类型：{kind}")
    if kind in {4, 5}:
        if len(data) < 189:
            raise ConversionError("PDG v1 类型 4/5 数据太短")
        count = 1 if kind == 4 else 2
        payload = payload[:128] + payload[128 + count:]
    encoding = f"HH v1 / {kind}"
    image = standard_image(payload, encoding)
    if image is not None:
        return image
    bitmap = decode_fax(payload, width, height)
    return ConvertedImage("FAX", width, height, "解码旧版 PDG 黑白传真流", encoding,
                          bitmap=bitmap)


FORMAT_EXTENSIONS = {"JPEG": ".jpg", "PNG": ".png", "TIFF": ".tiff", "BMP": ".bmp",
                     "GIF": ".gif", "WEBP": ".webp", "JPEG2000": ".jp2", "J2K": ".j2k",
                     "DJVU": ".djvu"}
FORCED_FORMATS = {"png": "PNG", "jpg": "JPEG", "jpeg": "JPEG", "tif": "TIFF",
                  "tiff": "TIFF", "bmp": "BMP", "webp": "WEBP"}


def chosen_format(image: ConvertedImage) -> str:
    mode = OUTPUT_FORMAT.strip().lower()
    if mode == "auto":
        return "PNG" if image.format == "FAX" else image.format
    if mode not in FORCED_FORMATS:
        raise ValueError(f"无效 OUTPUT_FORMAT：{OUTPUT_FORMAT}")
    target = FORCED_FORMATS[mode]
    if image.format == "DJVU":
        raise UnsupportedPdgError("DjVu 目前可在 auto 模式原样提取，暂未接入 DjVu 光栅解码器")
    if image.frames > 1 and target != image.format:
        raise UnsupportedPdgError("载荷包含多帧；请使用 auto 保留原始格式，避免丢失页面或动画")
    return target


def image_bytes(image: ConvertedImage, target_format: str) -> bytes:
    if image.original_bytes is not None and target_format == image.format:
        return image.original_bytes
    if image.bitmap is not None:
        bitmap = image.bitmap
    else:
        with Image.open(io.BytesIO(image.original_bytes)) as opened:
            opened.load()
            bitmap = opened.copy()
    options = {}
    if target_format == "JPEG":
        bitmap = bitmap.convert("RGB")
        options.update(quality=JPEG_QUALITY, subsampling=0)
    elif target_format == "WEBP":
        bitmap = bitmap.convert("RGB" if "A" not in bitmap.getbands() else "RGBA")
        options["lossless"] = True
    elif target_format == "TIFF":
        options["compression"] = "group4" if bitmap.mode == "1" else "tiff_deflate"
    if image.format == "FAX":
        if target_format == "PNG":
            # 与原样本相同的 1 位黑白索引图；像素内容不变。
            bitmap = bitmap.convert("L").point(lambda value: 1 if value else 0, mode="P")
            bitmap.putpalette([0, 0, 0, 255, 255, 255])
            options["bits"] = 1
        if PNG_DPI is not None:
            options["dpi"] = PNG_DPI
    buffer = io.BytesIO()
    bitmap.save(buffer, format=target_format, **options)
    return buffer.getvalue()


def write_atomically(target: Path, data: bytes) -> None:
    """先写同目录临时文件；成功后提交，默认不替换已有文件。"""
    target.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=".pdg_", suffix=".tmp", dir=target.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(data)
        if OVERWRITE_EXISTING:
            os.replace(temporary, target)
        else:
            try:
                os.link(temporary, target)
            except FileExistsError:
                raise
            except OSError:
                # FAT / 部分网络文件系统不支持硬链接，用独占创建防止覆盖。
                created = False
                try:
                    with target.open("xb") as destination, temporary.open("rb") as source:
                        created = True
                        shutil.copyfileobj(source, destination)
                except Exception:
                    if created:
                        target.unlink(missing_ok=True)
                    raise
    finally:
        temporary.unlink(missing_ok=True)


def natural_sort_key(path: Path):
    return [(1, int(piece)) if piece.isdigit() else (0, piece.casefold())
            for piece in re.split(r"(\d+)", path.as_posix())]


def find_inputs(input_dir: Path, output_dir: Path):
    if input_dir == output_dir:
        raise ValueError("输入与输出文件夹不能相同")
    candidates = input_dir.rglob("*") if RECURSIVE else input_dir.iterdir()
    paths = []
    for path in candidates:
        if not path.is_file() or path.suffix.lower() != ".pdg":
            continue
        if path.resolve().is_relative_to(output_dir):
            continue
        paths.append(path)
    return sorted(paths, key=lambda path: natural_sort_key(path.relative_to(input_dir)))


def convert_one(source: Path, input_dir: Path, output_dir: Path) -> FileResult:
    started = time.perf_counter()
    result = FileResult(input=str(source))
    try:
        image = read_pdg(source)
        target_format = chosen_format(image)
        relative = source.relative_to(input_dir)
        target = output_dir / relative.with_suffix(FORMAT_EXTENSIONS[target_format])
        result.output = str(target)
        result.format = target_format
        result.width, result.height = image.width, image.height
        result.frames, result.encoding = image.frames, image.encoding
        result.method = image.method
        if target.exists() and not OVERWRITE_EXISTING:
            result.status = "skipped"
        else:
            try:
                write_atomically(target, image_bytes(image, target_format))
                result.status = "success"
            except FileExistsError:
                result.status = "skipped"
    except Exception as exc:
        # 批处理中的单个异常留在报告里，随后继续下一个文件。
        result.status = "failed"
        result.error = f"{type(exc).__name__}: {exc}"
    result.seconds = round(time.perf_counter() - started, 3)
    return result


def save_reports(output_dir: Path, results, elapsed: float, input_dir: Path):
    report_time = datetime.now(timezone(timedelta(hours=8)))
    stamp = report_time.strftime("%Y%m%d_%H%M%S_%f")
    report_json = output_dir / f"conversion_report_{stamp}.json"
    report_csv = output_dir / f"conversion_report_{stamp}.csv"
    summary = {status: sum(row.status == status for row in results)
               for status in ("success", "skipped", "failed")}
    document = {"generated_at": report_time.isoformat(),
                "input_dir": str(input_dir), "output_dir": str(output_dir),
                "output_format": OUTPUT_FORMAT, "recursive": RECURSIVE,
                "elapsed_seconds": round(elapsed, 3), "total": len(results),
                "summary": summary, "files": [asdict(row) for row in results]}
    with report_json.open("x", encoding="utf-8") as handle:
        json.dump(document, handle, ensure_ascii=False, indent=2)
    with report_csv.open("x", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(FileResult.__dataclass_fields__))
        writer.writeheader()
        writer.writerows(asdict(row) for row in results)
    return summary, report_json, report_csv


def run_batch():
    input_dir = Path(INPUT_DIR).expanduser().resolve()
    output_dir = Path(OUTPUT_DIR).expanduser().resolve()
    if not input_dir.is_dir():
        raise ValueError(f"输入文件夹不存在：{input_dir}")
    if OUTPUT_FORMAT.strip().lower() not in {"auto", *FORCED_FORMATS}:
        raise ValueError(f"无效 OUTPUT_FORMAT：{OUTPUT_FORMAT}")
    sources = find_inputs(input_dir, output_dir)
    if not sources:
        print(f"没有找到 PDG 文件：{input_dir}")
        return []
    output_dir.mkdir(parents=True, exist_ok=True)
    print(f"输入：{input_dir}\n输出：{output_dir}\n文件数：{len(sources)}；输出模式：{OUTPUT_FORMAT}", flush=True)
    results = []
    started = time.perf_counter()
    labels = {"success": "成功", "skipped": "跳过", "failed": "失败"}
    try:
        for index, source in enumerate(sources, 1):
            result = convert_one(source, input_dir, output_dir)
            results.append(result)
            detail = result.error or str(Path(result.output).relative_to(output_dir))
            print(f"[{index}/{len(sources)}] {labels[result.status]}  "
                  f"{source.relative_to(input_dir)} → {detail}",
                  file=sys.stderr if result.status == "failed" else sys.stdout, flush=True)
    except KeyboardInterrupt:
        print("\n已中止。", file=sys.stderr, flush=True)
        raise
    elapsed = time.perf_counter() - started
    summary = {status: sum(row.status == status for row in results)
               for status in ("success", "skipped", "failed")}
    if WRITE_REPORTS:
        _, report_json, report_csv = save_reports(output_dir, results, elapsed, input_dir)
    if SHOW_SUMMARY:
        print(f"\n处理结束：成功 {summary['success']}，跳过 {summary['skipped']}，"
              f"失败 {summary['failed']}；耗时 {elapsed:.3f} 秒")
        if WRITE_REPORTS:
            print(f"JSON 报告：{report_json}\nCSV 报告：{report_csv}")
    return results


def main(argv=None):
    """命令行选项覆盖代码顶部配置；不带参数时适合在 IDE 中直接运行。"""
    global INPUT_DIR, OUTPUT_DIR, RECURSIVE, OVERWRITE_EXISTING
    global OUTPUT_FORMAT, SHOW_SUMMARY, WRITE_REPORTS

    # Windows 重定向时默认编码可能是 GBK；统一日志编码便于其他程序读取。
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")

    parser = argparse.ArgumentParser(description="PDG Converter (Python)")
    parser.add_argument("input_dir", nargs="?", type=Path, help="输入 PDG 目录")
    parser.add_argument("output_dir", nargs="?", type=Path, help="输出图像目录")
    parser.add_argument("--format", choices=["auto", *FORCED_FORMATS], default=None)
    parser.add_argument("--overwrite", action="store_true", default=None)
    parser.add_argument("--no-recursive", action="store_false", dest="recursive", default=None)
    summary = parser.add_mutually_exclusive_group()
    summary.add_argument("--summary", action="store_true", dest="summary")
    summary.add_argument("--no-summary", action="store_false", dest="summary")
    reports = parser.add_mutually_exclusive_group()
    reports.add_argument("--reports", action="store_true", dest="reports")
    reports.add_argument("--no-reports", action="store_false", dest="reports")
    parser.set_defaults(summary=None, reports=None)
    args = parser.parse_args(argv)
    if (args.input_dir is None) != (args.output_dir is None):
        parser.error("输入目录和输出目录必须同时提供")
    if args.input_dir is not None:
        INPUT_DIR, OUTPUT_DIR = args.input_dir, args.output_dir
    if args.format is not None:
        OUTPUT_FORMAT = args.format
    if args.overwrite is not None:
        OVERWRITE_EXISTING = args.overwrite
    if args.recursive is not None:
        RECURSIVE = args.recursive
    if args.summary is not None:
        SHOW_SUMMARY = args.summary
    if args.reports is not None:
        WRITE_REPORTS = args.reports
    try:
        results = run_batch()
    except KeyboardInterrupt:
        return 130
    except (ValueError, OSError) as exc:
        print(f"无法启动批量转换：{exc}", file=sys.stderr)
        return 1
    return 2 if any(row.status == "failed" for row in results) else 0


if __name__ == "__main__":
    raise SystemExit(main())
