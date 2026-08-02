#!/usr/bin/env python3
"""Generate the compact, seekable CJKF bitmap font used by the ESP32 UI.

The runtime deliberately does not use LVGL's binfont decoder.  CJKF has a
small fixed header, a sorted fixed-size index, and independently seekable A2
glyph bitmaps, making malformed assets easy to reject without a boot crash.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


MAGIC = b"CJKF"
VERSION = 1
HEADER = struct.Struct("<4sHHIIIIII")
ENTRY = struct.Struct("<IIHBBBbbB")


def gb2312_characters() -> set[str]:
    chars: set[str] = set()
    for high in range(0xA1, 0xF8):
        for low in range(0xA1, 0xFF):
            try:
                text = bytes((high, low)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if len(text) == 1 and not text.isspace():
                chars.add(text)
    return chars


def pack_a2(image: Image.Image) -> bytes:
    """Pack an 8-bit alpha image as LVGL A2, four MSB-first pixels/byte."""
    pixels = image.load()
    out = bytearray()
    for y in range(image.height):
        for x0 in range(0, image.width, 4):
            value = 0
            for i in range(4):
                x = x0 + i
                alpha = pixels[x, y] if x < image.width else 0
                level = min(3, (alpha + 42) // 85)
                value |= level << (6 - i * 2)
            out.append(value)
    return bytes(out)


def render(font: ImageFont.FreeTypeFont, char: str) -> tuple[bytes, tuple[int, ...]]:
    left, top, right, bottom = font.getbbox(char, anchor="ls")
    width = max(0, right - left)
    height = max(0, bottom - top)
    advance = max(1, round(font.getlength(char)))
    if width == 0 or height == 0:
        return b"", (advance, 0, 0, 0, 0)
    if width > 255 or height > 255 or not (-128 <= left <= 127) or not (-128 <= -bottom <= 127):
        raise ValueError(f"glyph metrics out of range for U+{ord(char):04X}")
    image = Image.new("L", (width, height), 0)
    # getbbox() above is relative to the left baseline ("ls"). Keep the same
    # anchor while rasterizing; using Pillow's default top-left anchor here
    # would place the glyph outside this tightly cropped image and emit an
    # all-transparent bitmap.
    ImageDraw.Draw(image).text((-left, -top), char, font=font, fill=255, anchor="ls")
    return pack_a2(image), (advance, width, height, left, -bottom)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", type=Path, required=True, help="SourceHan/Noto TTF or OTF")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size", type=int, default=14)
    args = parser.parse_args()

    font = ImageFont.truetype(str(args.font), args.size)
    static_ui = "当前状态点击屏幕或说唤醒词开始对话按下通过本地触发调试待机中聆听识别思考说话联网已连接断开错误请稍候用户助手心情开心难过惊讶宠物盒子你好吗天气笑话故事音乐音量设置返回菜单录音播放测试小智"
    punctuation = "，。！？：；、（）【】《》“”‘’…—·￥℃"
    chars = gb2312_characters() | set(static_ui + punctuation)
    codepoints = sorted(ord(c) for c in chars if ord(c) >= 0x80)

    bitmaps = bytearray()
    entries = bytearray()
    data_offset = HEADER.size + len(codepoints) * ENTRY.size
    max_bitmap = 0
    for cp in codepoints:
        bitmap, metrics = render(font, chr(cp))
        advance, width, height, ofs_x, ofs_y = metrics
        offset = data_offset + len(bitmaps)
        entries += ENTRY.pack(cp, offset, len(bitmap), advance, width, height, ofs_x, ofs_y, 0)
        bitmaps += bitmap
        max_bitmap = max(max_bitmap, len(bitmap))

    line_height = args.size + 2
    base_line = 2
    header = HEADER.pack(
        MAGIC, VERSION, HEADER.size, len(codepoints), HEADER.size, data_offset,
        line_height, base_line, max_bitmap
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header + entries + bitmaps)
    print(
        f"wrote {args.output}: {len(codepoints)} glyphs, "
        f"{args.output.stat().st_size} bytes, max bitmap {max_bitmap} bytes"
    )


if __name__ == "__main__":
    main()
