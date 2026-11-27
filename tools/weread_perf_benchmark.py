#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Deterministic WeRead comprehensive performance & I/O/allocation benchmark harness."""
from pathlib import Path
import argparse
import json
import os
import runpy
import statistics
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "build/weread-perf-bench"


def make_png_bytes(width: int, height: int, target_bytes: int) -> bytes:
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    def chunk(ctype: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(ctype + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + ctype + data + struct.pack(">I", crc)
    idat_payload = b"X" * max(64, target_bytes - 64)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat_payload) + chunk(b"IEND", b"")


def make_tar(entries: dict[str, bytes]) -> bytes:
    buf = bytearray()
    for name, data in entries.items():
        hdr = bytearray(512)
        nb = name.encode("utf-8")[:100]
        hdr[:len(nb)] = nb
        hdr[100:108] = b"0000644\0"
        hdr[108:116] = b"0000000\0"
        hdr[116:124] = b"0000000\0"
        hdr[124:136] = f"{len(data):011o}\0".encode("ascii")
        hdr[136:148] = b"00000000000\0"
        hdr[148:156] = b"        "
        hdr[156:157] = b"0"
        hdr[257:263] = b"ustar\0"
        hdr[263:265] = b"00"
        chk = sum(hdr)
        hdr[148:156] = f"{chk:06o}\0 ".encode("ascii")
        buf.extend(hdr)
        buf.extend(data)
        rem = len(data) % 512
        if rem:
            buf.extend(b"\0" * (512 - rem))
    buf.extend(b"\0" * 1024)
    return bytes(buf)


def build_fixtures(fixtures_dir: Path) -> None:
    fixtures_dir.mkdir(parents=True, exist_ok=True)
    gen = runpy.run_path(str(ROOT / "tools/fixtures/weread/generate_fixtures.py"))
    make_zip = gen["_make_zipcrypto_zip"]
    pw = b"test_password_123"

    # 1. Crypto ZIP fixture (~256 KiB total across 12 entries)
    pack_entries = {}
    for i in range(4):
        txt = (f"<html><body><h1>Part {i}</h1>" + "".join(f"<p>第{j}段中文测试文本HelloWorld。</p>" for j in range(180)) + "</body></html>").encode()
        pack_entries[f"Text/part{i}.xhtml"] = txt
    for i in range(2):
        pack_entries[f"Styles/s{i}.css"] = (f"p.c{i} {{ font-size: 1.0em; }}\n" * 60).encode()
    for i in range(6):
        pack_entries[f"Images/im{i}.png"] = make_png_bytes(160, 220, 28 * 1024)
    (fixtures_dir / "bench_pack.zip").write_bytes(make_zip(pack_entries, pw))

    # TAR fixture (~256 KiB across 4 entries)
    tar_entries = {f"Text/t{i}.txt": (("中文一行测试内容ABC123\n" * 2400).encode()) for i in range(4)}
    (fixtures_dir / "bench_txt.tar").write_bytes(make_tar(tar_entries))

    # 2. Long chapter fixture (~30,000 Chinese/ASCII chars across 4 XHTML files)
    paras = "".join(
        f'<p id="p{k}">这是第{k:04d}段性能测试正文，包含中文字符与English words以及实体&amp;测试<a href="part{(k+1)%4}.xhtml#p0">[{k}]</a>。</p>'
        for k in range(135)
    )
    big_png = make_png_bytes(800, 1200, 512 * 1024)
    long_entries = {
        "info.txt": json.dumps({
            "bookId": "bench_book", "bookVersion": 100,
            "chapters": [{"chapterUid": uid, "files": [f"Text/part{j}.xhtml" for j in range(4)]} for uid in range(1, 13)],
        }).encode(),
        "Styles/ch.css": b"p { line-height: 1.4; }",
        "Images/big.png": big_png,
    }
    for j in range(4):
        img_tag = '<img src="../Images/big.png"/>' if j == 0 else ''
        long_entries[f"Text/part{j}.xhtml"] = f"<html><head><link rel=\"stylesheet\" href=\"../Styles/ch.css\"/></head><body>{img_tag}{paras}</body></html>".encode()
    (fixtures_dir / "long_chapter.zip").write_bytes(make_zip(long_entries, pw))

    # 3. Small chapter fixture for prefetch tests
    small_entries = {
        "info.txt": json.dumps({
            "bookId": "book_0000", "bookVersion": 100,
            "chapters": [{"chapterUid": uid, "files": ["Text/main.xhtml"]} for uid in range(1, 21)],
        }).encode(),
        "Text/main.xhtml": ("<html><body>" + "<p>简短预取章节正文测试内容。</p>" * 12 + "</body></html>").encode(),
    }
    (fixtures_dir / "small_chapter.zip").write_bytes(make_zip(small_entries, pw))
    (fixtures_dir / "cover.png").write_bytes(make_png_bytes(176, 240, 2048))
    (fixtures_dir / "big_sample.png").write_bytes(make_png_bytes(800, 1200, 512 * 1024))


def compile_binaries(extra_flags: list[str] | None = None) -> tuple[Path, Path]:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    cc = os.environ.get("CC", "cc")
    common_flags = [
        "-O2", "-pthread", "-std=c11", "-D_DEFAULT_SOURCE",
        "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
        "-Imain", "-Imain/book", "-Imain/weread", "-Itools/weread_stubs",
        *(extra_flags or []),
    ]
    main_bin = OUT_DIR / "test_weread_perf_benchmark"
    main_sources = [
        "tools/test_weread_perf_benchmark.c",
        "tools/weread_stubs/heap_caps_memory_mock.c",
        "main/book/book_weread_crypto.c",
        "main/book/book_source_weread.c",
        "main/book/book_image_header.c",
        "main/book/html_text.c",
        "main/weread/weread_runtime.c",
        "main/weread/weread_chapter.c",
        "main/weread/weread_position.c",
        "main/weread/weread_progress.c",
        "main/weread/weread_shelf.c",
        "main/weread/weread_toc.c",
        "main/weread/weread_store.c",
        "main/weread/weread_auth.c",
        "main/weread/weread_client.c",
        "tools/weread_stubs/cJSON.c",
    ]
    # Note: force-include spy macros on weread/book production sources, NOT heap_caps_memory_mock.c or cJSON.c
    spy_objs = []
    other_objs = []
    obj_dir = OUT_DIR / "objs"
    obj_dir.mkdir(exist_ok=True)
    mem_flags = [
        "-DWEREAD_TEST_MEMORY",
        "-Dmalloc=weread_test_malloc",
        "-Dcalloc=weread_test_calloc",
        "-Drealloc=weread_test_realloc",
        "-Dfree=weread_test_free",
    ]
    for src in main_sources:
        obj = obj_dir / (Path(src).name + ".o")
        cmd = [cc, *common_flags]
        if src == "tools/test_weread_perf_benchmark.c":
            cmd.extend(["-DWEREAD_TEST_MEMORY", "-Dfree=weread_test_free"])
        else:
            cmd.extend(mem_flags)
        if src.startswith("main/"):
            cmd.extend(["-include", str(ROOT / "tools/weread_perf_io_spy.h")])
        cmd.extend(["-c", str(ROOT / src), "-o", str(obj)])
        subprocess.run(cmd, cwd=ROOT, check=True)
        spy_objs.append(str(obj))
    subprocess.run([cc, *spy_objs, "-lz", "-lm", "-pthread", "-o", str(main_bin)], cwd=ROOT, check=True)

    http_bin = OUT_DIR / "test_weread_http_poll_bench"
    http_flags = [*common_flags, "-DWEREAD_USE_ESP_HTTP_CLIENT", "-DWEREAD_TEST_ASYNC_HTTP"]
    http_sources = [
        "tools/test_weread_http_poll_bench.c",
        "main/weread/weread_client.c",
        "main/weread/weread_store.c",
        "main/weread/weread_auth.c",
        "tools/weread_stubs/cJSON.c",
    ]
    subprocess.run([cc, *http_flags, *http_sources, "-lz", "-lm", "-o", str(http_bin)], cwd=ROOT, check=True)
    return main_bin, http_bin


def parse_metrics(stdout: str) -> tuple[dict[str, dict], dict[str, int]]:
    metrics = {}
    mem = {}
    nvs = {}
    for line in stdout.splitlines():
        if line.startswith("METRIC "):
            parts = line.split()[1:]
            kv = {}
            for p in parts:
                k, v = p.split("=", 1)
                if k == "name":
                    kv[k] = v
                else:
                    kv[k] = float(v) if "." in v else int(v)
            metrics[kv["name"]] = kv
        elif line.startswith("NVS "):
            for p in line.split()[2:]:
                k, v = p.split("=", 1)
                nvs[k] = int(v)
        elif "internal peak=" in line:
            for tok in line.replace(".", "").split():
                if tok.startswith("peak="):
                    pass
            import re
            m = re.search(r"internal peak=(\d+),\s*PSRAM peak=(\d+)", line)
            if m:
                mem = {"internal_peak": int(m.group(1)), "psram_peak": int(m.group(2))}
    if nvs and "page_turn_save_20x_long" in metrics:
        metrics["page_turn_save_20x_long"].update(nvs)
    if mem:
        for m in metrics.values():
            m.setdefault("internal_peak", mem["internal_peak"])
            m.setdefault("psram_peak", mem["psram_peak"])
    return metrics, mem


def run_suite(repeats: int, json_out: Path | None, extra_flags: list[str] | None = None) -> dict:
    fixtures_dir = OUT_DIR / "fixtures"
    build_fixtures(fixtures_dir)
    main_bin, http_bin = compile_binaries(extra_flags=extra_flags)

    benches = ["crypto", "chapter", "shelf", "toc", "open_save", "prefetch"]
    aggregated = {}
    for bench in benches:
        samples = []
        for _ in range(repeats):
            with tempfile.TemporaryDirectory(prefix=f"wr-perf-{bench}-") as tmp:
                res = subprocess.run([str(main_bin), bench, tmp, str(fixtures_dir)], capture_output=True, text=True, check=True)
                m, _ = parse_metrics(res.stdout)
                samples.append(m)
        for name in samples[0]:
            entry = {"repeats": repeats}
            for k, v in samples[0][name].items():
                if k == "name":
                    continue
                vals = [s[name][k] for s in samples]
                entry[k] = statistics.median(vals) if k == "us" or isinstance(v, float) else vals[0]
            aggregated[name] = entry

    # HTTP poll bench
    http_samples = []
    for _ in range(repeats):
        with tempfile.TemporaryDirectory(prefix="wr-perf-http-") as tmp:
            res = subprocess.run([str(http_bin), tmp], capture_output=True, text=True, check=True)
            m, _ = parse_metrics(res.stdout)
            http_samples.append(m)
    for name in http_samples[0]:
        entry = {"repeats": repeats}
        for k in http_samples[0][name]:
            if k == "name":
                continue
            vals = [s[name][k] for s in http_samples]
            entry[k] = statistics.median(vals) if k == "us" else vals[0]
        aggregated[name] = entry

    if json_out:
        json_out.parent.mkdir(parents=True, exist_ok=True)
        json_out.write_text(json.dumps(aggregated, indent=2) + "\n")

    print(f"\n=== WeRead Perf Benchmark (repeats={repeats}, -O2) ===")
    print(f"{'Metric':32s} {'ms':>8s} {'fopen':>6s} {'fseek':>6s} {'stat':>6s} {'fsync':>6s} {'shelfW':>6s} {'PSRAM_KB':>9s} {'Extra'}")
    for name, d in aggregated.items():
        ms = d.get("us", 0) / 1000.0
        psram = d.get("psram_peak", 0) / 1024.0
        extra = ""
        if "mb_per_s" in d:
            extra += f"{d['mb_per_s']:.1f}MB/s "
        if "map_cache_bytes" in d:
            extra += f"map={int(d['map_cache_bytes'])}B "
        if "shelf_bytes" in d and d["shelf_bytes"]:
            extra += f"shelfB={int(d['shelf_bytes'])} "
        if "nvs_reads" in d:
            extra += f"nvsR={int(d['nvs_reads'])} "
        if "fread_bytes" in d and name.startswith("image_"):
            extra += f"freadB={int(d['fread_bytes'])}"
        print(f"{name:32s} {ms:8.2f} {int(d.get('fopen', 0)):6d} {int(d.get('fseek', 0)):6d} "
              f"{int(d.get('stat', 0)):6d} {int(d.get('fsync', 0)):6d} {int(d.get('shelf_writes', 0)):6d} "
              f"{psram:9.1f} {extra}")
    return aggregated


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--image-dim-api", action="store_true")
    args = parser.parse_args()
    flags = ["-DWR_BENCH_IMAGE_DIM_API"] if args.image_dim_api else None
    run_suite(args.repeats, args.json, flags)


if __name__ == "__main__":
    main()
