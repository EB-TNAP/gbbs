#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# patch-dvb-srmin.py - widen the symbol-rate range (1..45 Msym/s) that
# GigaBlue's prebuilt dvb.ko (gb7252 / BCM7439 proxy driver) registers for
# its satellite frontends.
#
# dvb-core's dvb_frontend_check_parameters() rejects any DVB-S/S2 tune below
# fe->ops.info.symbol_rate_min with:
#   DVB: adapter 0 frontend N symbol rate 360000 out of range (1000000..45000000)
# The value is plain static data (struct dvb_frontend_info in .data, no
# relocations, nothing rewrites it at runtime), so patching the word is all
# that is needed. The same check applies symbol_rate_max. Whether the demod
# then LOCKS outside 1..45 Msym/s is up to the Si2183 / BCM45308 firmware
# (Si2183 is rated 1..45 MSps) - this only stops the kernel refusing to try.
#
# The blindscan ioctl (dev_blindscan0_ioctl) also WRITES those fields: a
# seek copies its search range into frequency_min/max and symbol_rate_min/max
# (fe->ops.info +0x84..+0x98), and stopping a seek restores hard-coded vendor
# defaults (950000..2150000 kHz, 1000000..45000000 sym/s) - undoing the patch
# above until reboot, so sub-1 Msym/s tunes fail again after any blindscan or
# "gbbs reset". That restore is patched too:
#   ldr r3, =1000000 (literal shared with unrelated code)  ->  mov r3, #98304
#   literal 45000000 (used only there)                      ->  --max
# 98304 is the nearest value <= --min that fits one ARM immediate.
#
# Blocks are found by pattern, not fixed offsets, so this survives a vendor
# dvb.ko update: name[128] followed by type==FE_QPSK, freq 950000..2150000,
# symbol_rate_min <= 1000000, symbol_rate_max 10..100 Msym/s.
#
# Usage:
#   patch-dvb-srmin.py dvb.ko                 # min -> 100000, max -> 60000000
#   patch-dvb-srmin.py dvb.ko --min 250000 --max 45000000
#   patch-dvb-srmin.py dvb.ko --check         # report only, exit 1 if unpatched
#
# Idempotent: re-running with the same values changes nothing; re-running
# with different values re-patches already-patched blocks.

import argparse
import struct
import sys

# struct dvb_frontend_info: char name[128]; then u32:
#   type, frequency_min, frequency_max, frequency_stepsize,
#   frequency_tolerance, symbol_rate_min, symbol_rate_max, ...
INFO_FMT = "<7I"
FE_QPSK = 0
OLD_MIN = 1000000


def find_blocks(data):
    found = []
    for i in range(128, len(data) - 28, 4):
        typ, fmin, fmax, _step, _tol, srmin, srmax = struct.unpack_from(
            INFO_FMT, data, i)
        if typ != FE_QPSK or fmin != 950000 or fmax != 2150000:
            continue
        if not (1000 <= srmin <= OLD_MIN) or not (10000000 <= srmax <= 100000000):
            continue
        name = data[i - 128:i].split(b"\0")[0]
        if not name or any(c < 0x20 or c > 0x7e for c in name):
            continue
        found.append((i + 20, srmin, srmax, name.decode("ascii")))
    return found

# --- blindscan seek-stop restore defaults ---------------------------------
_STR = {0x84: 0xE5843084, 0x88: 0xE5843088, 0x8C: 0xE584308C,
        0x94: 0xE5843094, 0x98: 0xE5843098}
_MOV1000 = 0xE3A03FFA          # mov r3, #1000


def _is_ldr_r3_lit(w):
    return (w & 0xFFFFF000) == 0xE59F3000


def _mov_r3_imm(v):
    """Largest single-instruction 'mov r3, #imm' value <= v, and its encoding."""
    best = None
    for rot in range(16):
        for imm8 in range(256):
            val = ((imm8 >> (2 * rot)) | (imm8 << (32 - 2 * rot))) & 0xFFFFFFFF if rot else imm8
            if val <= v and (best is None or val > best[0]):
                best = (val, 0xE3A03000 | (rot << 8) | imm8)
    return best


def _ldr_refs(data, lit_off, lo, hi):
    n = 0
    for off in range(lo, hi, 4):
        w, = struct.unpack_from("<I", data, off)
        if (w & 0x0F7F0000) == 0x051F0000 and (w >> 28) != 0xF:   # ldr rX, [pc, #+-imm]
            imm = w & 0xFFF
            tgt = off + 8 + (imm if w & 0x00800000 else -imm)
            if tgt == lit_off:
                n += 1
    return n


def patch_restore(data, new_min, new_max, check):
    hits = []
    for off in range(0, len(data) - 40, 4):
        w = struct.unpack_from("<10I", data, off)
        if (w[1], w[3], w[4], w[5], w[7], w[9]) == (
                _STR[0x84], _STR[0x88], _MOV1000, _STR[0x8C], _STR[0x94], _STR[0x98]) \
                and _is_ldr_r3_lit(w[8]):
            hits.append(off)
    if len(hits) != 1:
        print("seek-stop restore defaults: %d matches, not patched "
              "(driver differs - check by hand)" % len(hits))
        return 0
    off = hits[0]
    min_off, max_ldr_off = off + 24, off + 32
    w_min, w_max = struct.unpack_from("<I", data, min_off)[0], struct.unpack_from("<I", data, max_ldr_off)[0]
    lit = max_ldr_off + 8 + (w_max & 0xFFF)
    cur_max, = struct.unpack_from("<I", data, lit)
    want_val, want_ins = _mov_r3_imm(new_min)
    changed = 0

    if w_min == want_ins:
        print("0x%06x  seek-stop restore min   already mov r3, #%d" % (min_off, want_val))
    else:
        was = "ldr (vendor 1000000)" if _is_ldr_r3_lit(w_min) else "0x%08x" % w_min
        print("0x%06x  seek-stop restore min   %s  ->  mov r3, #%d" % (min_off, was, want_val))
        if not check:
            struct.pack_into("<I", data, min_off, want_ins)
        changed += 1

    if cur_max == new_max:
        print("0x%06x  seek-stop restore max   already %d" % (lit, cur_max))
    else:
        # Only rewrite the literal if nothing else in the module loads it.
        refs = _ldr_refs(data, lit, max(0, lit - 4096), min(len(data), lit + 4096))
        if refs != 1:
            print("0x%06x  seek-stop restore max   literal shared (%d refs), left at %d" % (lit, refs, cur_max))
        else:
            print("0x%06x  seek-stop restore max   %d  ->  %d" % (lit, cur_max, new_max))
            if not check:
                struct.pack_into("<I", data, lit, new_max)
            changed += 1
    return changed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ko")
    ap.add_argument("--min", type=int, default=100000)
    ap.add_argument("--max", type=int, default=60000000)
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()

    if not 1000 <= a.min < OLD_MIN:
        sys.exit("--min must be between 1000 and %d" % (OLD_MIN - 1))
    if not 10000000 <= a.max <= 100000000:
        sys.exit("--max must be between 10000000 and 100000000")

    data = bytearray(open(a.ko, "rb").read())
    if data[:4] != b"\x7fELF":
        sys.exit("%s: not an ELF file" % a.ko)

    blocks = find_blocks(data)
    if not blocks:
        sys.exit("%s: no satellite dvb_frontend_info blocks found - "
                 "driver layout changed, inspect manually" % a.ko)

    todo = 0
    for off, srmin, srmax, name in blocks:
        same = (srmin, srmax) == (a.min, a.max)
        print("0x%06x  %-32s  %d..%d%s" % (
            off, name, srmin, srmax,
            "  (already)" if same else "  ->  %d..%d" % (a.min, a.max)))
        if not same:
            todo += 1
            if not a.check:
                struct.pack_into("<II", data, off, a.min, a.max)

    todo += patch_restore(data, a.min, a.max, a.check)

    if a.check:
        return 1 if todo else 0
    if todo:
        with open(a.ko, "wb") as f:
            f.write(data)
        print("patched %d block(s) in %s" % (todo, a.ko))
    else:
        print("nothing to do")
    return 0


if __name__ == "__main__":
    sys.exit(main())
