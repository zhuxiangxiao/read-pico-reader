#!/usr/bin/env python3
"""Generate authentic test fixtures for WeRead E2E host tests."""

import io
import json
import os
import struct
import tarfile
import zlib

FIX_DIR = os.path.dirname(os.path.abspath(__file__))


def _zipcrypto_encrypt(data: bytes, pwd: bytes, crc: int) -> bytes:
    """Traditional PKWARE ZipCrypto encryption."""
    table = []
    for n in range(256):
        c = n
        for _ in range(8):
            c = (0xEDB88320 ^ (c >> 1)) if (c & 1) else (c >> 1)
        table.append(c)

    k0, k1, k2 = 0x12345678, 0x23456789, 0x34567890

    def update(b: int) -> None:
        nonlocal k0, k1, k2
        k0 = ((k0 >> 8) ^ table[(k0 ^ b) & 0xFF]) & 0xFFFFFFFF
        k1 = ((k1 + (k0 & 0xFF)) * 134775813 + 1) & 0xFFFFFFFF
        k2 = ((k2 >> 8) ^ table[(k2 ^ (k1 >> 24)) & 0xFF]) & 0xFFFFFFFF

    def key_byte() -> int:
        t = (k2 | 2) & 0xFFFF
        return ((t * (t ^ 1)) >> 8) & 0xFF

    for ch in pwd:
        update(ch)

    hdr = bytes([0] * 10 + [(crc >> 16) & 0xFF, (crc >> 24) & 0xFF])
    out = bytearray()
    for b in hdr + data:
        kb = key_byte()
        update(b)
        out.append(b ^ kb)
    return bytes(out)


def _make_zipcrypto_zip(entries: dict[str, bytes], pwd: bytes, corrupt_crc: bool = False) -> bytes:
    """Create a valid ZIP buffer with ZipCrypto encryption."""
    local_parts = bytearray()
    central_parts = bytearray()
    offset = 0

    for name, plain in entries.items():
        name_b = name.encode("utf-8")
        crc = zlib.crc32(plain) & 0xFFFFFFFF
        if corrupt_crc:
            # Preserve byte 3 (MSB) so ZipCrypto password check succeeds,
            # allowing extraction to reach EOF and trigger CRC-32 mismatch (BadZipFile).
            crc_for_header = (crc ^ 0x00ADBEEF) & 0xFFFFFFFF
        else:
            crc_for_header = crc

        enc = _zipcrypto_encrypt(plain, pwd, crc)
        flag_bits = 0x0001  # Bit 0: encrypted

        # Local file header
        lh = struct.pack(
            "<IHHHHHIIIHH",
            0x04034B50,  # Signature
            20,          # Version needed
            flag_bits,   # Flags
            0,           # Compression (Stored)
            0,           # Mod time
            0,           # Mod date
            crc_for_header,
            len(enc),    # Compressed size
            len(plain),  # Uncompressed size
            len(name_b), # File name len
            0            # Extra field len
        ) + name_b

        # Central directory header
        cd = struct.pack(
            "<IHHHHHHIIIHHHHHII",
            0x02014B50,  # Signature
            20,          # Version made by
            20,          # Version needed
            flag_bits,
            0,
            0,
            0,
            crc_for_header,
            len(enc),
            len(plain),
            len(name_b),
            0, 0, 0, 0, 0,
            offset
        ) + name_b

        offset += len(lh) + len(enc)
        local_parts.extend(lh)
        local_parts.extend(enc)
        central_parts.extend(cd)

    eocd = struct.pack(
        "<IHHHHIIH",
        0x06054B50,
        0, 0,
        len(entries), len(entries),
        len(central_parts),
        offset,
        0
    )
    return bytes(local_parts) + bytes(central_parts) + eocd


def make_epub_fixture(output_path: str, pwd: bytes) -> None:
    """Chapter 0 and Chapter 1 sample EPUB zip."""
    info_json = {
        "bookId": "test_book_101",
        "bookVersion": 100,
        "chapters": [
            {
                "chapterUid": 1,
                "chapterIdx": 1,
                "title": "第一章 起源",
                "files": ["Text/chapter1_part1.xhtml", "Text/chapter1_part2.xhtml"]
            }
        ]
    }
    entries = {
        "info.txt": json.dumps(info_json, ensure_ascii=False, indent=2).encode("utf-8"),
        "Styles/main.css": b"body { font-size: 16px; line-height: 1.6; }\nh1 { font-size: 24px; }\np { text-indent: 2em; }",
        "Images/cover.jpg": b"\xFF\xD8\xFF\xE0\x00\x10JFIF\x00\x01\x01\x01\x00H\x00H\x00\x00\xFF\xDB\x00C\x00mock_jpeg_data\xFF\xD9",
        "Text/chapter1_part1.xhtml": (
            b"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            b"<!DOCTYPE html>\n"
            b"<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
            b"<head><title>\xe7\xac\xac\xe4\xb8\x80\xe7\xab\xa0</title><link rel=\"stylesheet\" href=\"../Styles/main.css\"/></head>\n"
            b"<body>\n<h1>\xe7\xac\xac\xe4\xb8\x80\xe7\xab\xa0 \xe8\xb5\xb7\xe6\xba\x90</h1>\n"
            b"<p>\xe5\xbe\xae\xe9\xa3\x8e\xe5\x90\xb9\xe8\xbf\x87\xe8\x8d\x92\xe5\x8e\x9f\xef\xbc\x8c\xe6\x97\xa7\xe4\xb8\x96\xe7\x95\x8c\xe7\x9a\x84\xe9\x92\x9f\xe5\xa3\xb0\xe5\x9c\xa8\xe8\xbf\x9c\xe6\x96\xb9\xe5\x9b\x9e\xe8\x8d\xa1\xe3\x80\x82</p>\n"
            b"</body>\n</html>"
        ),
        "Text/chapter1_part2.xhtml": (
            b"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            b"<!DOCTYPE html>\n"
            b"<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
            b"<head><title>\xe7\xac\xac\xe4\xb8\x80\xe7\xab\xa0 \xe7\xbb\xad</title><link rel=\"stylesheet\" href=\"../Styles/main.css\"/></head>\n"
            b"<body>\n"
            b"<p>\xe6\x8e\xa2\xe7\xb4\xa2\xe8\x80\x85\xe8\xb8\x8f\xe4\xb8\x8a\xe4\xba\x86\xe6\x96\xb0\xe7\x9a\x84\xe5\xbe\x81\xe7\xa8\x8b\xef\xbc\x8c\xe5\x89\x8d\xe6\x96\xb9\xe6\x98\xaf\xe6\x9c\xaa\xe7\x9f\xa5\xe7\x9a\x84\xe5\xba\x87\xe6\x8a\xa4\xe6\x89\x80\xe3\x80\x82</p>\n"
            b"</body>\n</html>"
        ),
    }
    blob = _make_zipcrypto_zip(entries, pwd)
    with open(output_path, "wb") as f:
        f.write(blob)
    print(f"Generated {output_path} ({len(blob)} bytes)")


def make_tar_fixture(output_path: str) -> None:
    """Chapter 1 sample TXT USTAR tar archive."""
    info_json = {
        "bookId": "txt_book_202",
        "bookVersion": 1,
        "chapters": [
            {
                "chapterUid": 1,
                "chapterIdx": 1,
                "title": "第一章 序章",
                "files": ["txt_book_202_1_o"]
            }
        ]
    }
    info_bytes = json.dumps(info_json, ensure_ascii=False, indent=2).encode("utf-8")
    text_bytes = (
        "第一章 序章\n"
        "这是一个纯文本书籍的测试章节。\n"
        "夜幕低垂，星光穿透薄雾照耀大地。\n"
        "第二行文字继续展开故事的序幕。\n"
    ).encode("utf-8")

    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tf:
        ti_info = tarfile.TarInfo(name="info.txt")
        ti_info.size = len(info_bytes)
        tf.addfile(ti_info, io.BytesIO(info_bytes))

        ti_txt = tarfile.TarInfo(name="txt_book_202_1_o")
        ti_txt.size = len(text_bytes)
        tf.addfile(ti_txt, io.BytesIO(text_bytes))

    blob = buf.getvalue()
    with open(output_path, "wb") as f:
        f.write(blob)
    print(f"Generated {output_path} ({len(blob)} bytes)")


def make_corrupted_crc_fixture(output_path: str, pwd: bytes) -> None:
    """ZIP fixture with corrupt CRC in header."""
    entries = {
        "info.txt": b'{"corrupted": true}',
        "chapter.txt": b"Should fail CRC check!"
    }
    blob = _make_zipcrypto_zip(entries, pwd, corrupt_crc=True)
    with open(output_path, "wb") as f:
        f.write(blob)
    print(f"Generated {output_path} ({len(blob)} bytes)")


def make_path_traversal_fixture(output_path: str, pwd: bytes) -> None:
    """ZIP fixture with path traversal entry."""
    entries = {
        "info.txt": b'{"attack": true}',
        "../../evil.txt": b"MALICIOUS_OVERWRITE_PAYLOAD"
    }
    blob = _make_zipcrypto_zip(entries, pwd)
    with open(output_path, "wb") as f:
        f.write(blob)
    print(f"Generated {output_path} ({len(blob)} bytes)")


def main() -> None:
    os.makedirs(FIX_DIR, exist_ok=True)
    pwd = b"test_password_123"

    make_epub_fixture(os.path.join(FIX_DIR, "sample_epub_chapter0_and_1.zip"), pwd)
    make_tar_fixture(os.path.join(FIX_DIR, "sample_txt_chapter1.tar"))
    make_corrupted_crc_fixture(os.path.join(FIX_DIR, "corrupted_crc.zip"), pwd)
    make_path_traversal_fixture(os.path.join(FIX_DIR, "path_traversal_attack.zip"), pwd)
    print("All fixtures generated successfully.")


if __name__ == "__main__":
    main()
