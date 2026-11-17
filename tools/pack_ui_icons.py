#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 kiikoread
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Losslessly pack the canonical generated icon masks without rasterizing or changing artwork."""
from pathlib import Path
import argparse, hashlib, re

ROOT=Path(__file__).resolve().parents[1]

def pack(data):
    out=bytearray();i=0
    while i<len(data):
        run=1
        while run<128 and i+run<len(data) and data[i+run]==data[i]:run+=1
        if run>=3:
            out.extend((128+run-1,data[i]));i+=run;continue
        start=i;i+=run
        while i<len(data) and i-start<128:
            run=1
            while run<3 and i+run<len(data) and data[i+run]==data[i]:run+=1
            if run>=3:break
            i+=min(run,128-(i-start))
        out.append(i-start-1);out.extend(data[start:i])
    return bytes(out)

def unpack(data):
    out=bytearray();i=0
    while i<len(data):
        command=data[i];i+=1;count=(command&127)+1
        if command&128:out.extend(bytes([data[i]])*count);i+=1
        else:out.extend(data[i:i+count]);i+=count
    return bytes(out)

def canonical(board):
    header=ROOT/f'boards/{board}/src/main/assets/ui_icons.h'
    source=header.read_text();side=int(re.search(r'#define UI_ICON_MASK_SIZE (\d+)',source)[1])
    blocks=re.findall(r'\{([^{}]+)\}',source[source.index('static const uint8_t ui_icon_masks'):])
    masks=[bytes.fromhex(''.join(re.findall(r'0x([0-9a-fA-F]{2})',b))) for b in blocks]
    assert masks and all(len(m)==side*side//2 for m in masks)
    return masks

def generate(board,check=False):
    masks=canonical(board);offsets=[0];payload=bytearray()
    for mask in masks:
        encoded=pack(mask);assert unpack(encoded)==mask
        payload.extend(encoded);offsets.append(len(payload))
    digest=hashlib.sha256(b''.join(masks)).hexdigest()
    text='''/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-FileCopyrightText: 2026 kiikoread (lossless repacking)
 * SPDX-License-Identifier: Apache-2.0
 * 中文：由tools/pack_ui_icons.py无损编码原ui_icons.h图标，保留原像素与许可。
 * English: Losslessly repacked from ui_icons.h; preserve its original artwork and licensing.
 * 冻结：Lucide图标为ISC、AnimateIcons为MIT/ISC，原署名见THIRD_PARTY_NOTICES.md及ui_icons.h。
 * Frozen: Lucide (ISC) and AnimateIcons (MIT/ISC); retain notices in THIRD_PARTY_NOTICES.md and ui_icons.h.
 */
#pragma once
#include <stdint.h>
'''
    text+=f'#define UI_ICON_PACKED_COUNT {len(masks)}\n#define UI_ICON_UNPACKED_BYTES {len(masks[0])}\n'
    text+=f'#define UI_ICON_DECODED_SHA256 "{digest}"\n'
    text+='static const uint32_t ui_icon_packed_offsets[] = {'+','.join(map(str,offsets))+'};\n'
    text+='static const uint8_t ui_icon_packed[] = {\n'
    for at in range(0,len(payload),24):text+='    '+','.join(f'0x{v:02x}' for v in payload[at:at+24])+',\n'
    text+='};\n'
    path=ROOT/f'boards/{board}/src/main/assets/ui_icons_packed.h'
    if check:assert path.read_text()==text,f'stale packed icons: {board}'
    else:path.write_text(text)
    print(f'{board}: {sum(map(len,masks))} -> {len(payload)+len(offsets)*4} bytes; decoded SHA256 {digest}')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--check',action='store_true');args=parser.parse_args()
    for board in ('pico','metalio47'):generate(board,args.check)
