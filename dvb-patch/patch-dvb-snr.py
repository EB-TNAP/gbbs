#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# patch-dvb-snr.py - remove the 6.75 dB floor from the SNR GigaBlue's
# prebuilt dvb.ko reports for the TS3L10 / TS2L08 plug-in NIMs.
#
# TS3L10_read_snr / TS2L08_read_snr turn the Si2183 C/N (0.01 dB, "c") into
# a 0..65535 value:
#     c + 25 > 1800        -> 65535
#     c + 25 < 700         -> 26214 (40 %), whatever c really is
#     otherwise            -> ((c - 676) / 19 + 43) % of 65535
# so every signal under ~6.75 dB C/N reads the same, and the meter cannot
# show a dish being peaked on a weak carrier. The formula itself stays valid
# down to c = -141 (0 %), so the fix is to always take it:
#     cmp r3, #700 ; bge <formula>   ->   cmp r3, #700 ; b <formula>
# One instruction per function; above 6.75 dB nothing changes.
#
# Usage:
#   patch-dvb-snr.py dvb.ko             # apply
#   patch-dvb-snr.py dvb.ko --restore   # back to the vendor floor
#   patch-dvb-snr.py dvb.ko --check     # report only, exit 1 if not patched
#
# Stdlib only.

import argparse
import struct
import sys

FUNCS = (b"TS3L10_read_snr", b"TS2L08_read_snr")
CMP_700 = 0xE3530FAF          # cmp r3, #700
MOVW_FLOOR = 0xE3063666       # movw r3, #26214
BGE_COND = 0xA                # cond field of the vendor branch
AL_COND = 0xE                 # always


def symbols(data):
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        sys.exit("not a 32-bit little-endian ELF")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    secs = [struct.unpack_from("<10I", data, shoff + i * shentsize)
            for i in range(shnum)]
    out = {}
    for s in secs:
        if s[1] != 2:
            continue
        strtab = secs[s[6]]
        for j in range(s[5] // 16):
            name, value, size, _i, _o, shndx = struct.unpack_from(
                "<IIIBBH", data, s[4] + j * 16)
            p = strtab[4] + name
            nm = bytes(data[p:data.index(b"\0", p)])
            if nm in FUNCS:
                out[nm] = (secs[shndx][4] + value, size)
    return out


def find_branch(data, start, size):
    hits = []
    for off in range(start, start + size - 8, 4):
        a, b, c = struct.unpack_from("<3I", data, off)
        if a == CMP_700 and c == MOVW_FLOOR and (b & 0x0F000000) == 0x0A000000 \
                and (b >> 28) in (BGE_COND, AL_COND):
            hits.append(off + 4)
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ko")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--restore", action="store_true")
    g.add_argument("--check", action="store_true")
    a = ap.parse_args()

    data = bytearray(open(a.ko, "rb").read())
    syms = symbols(data)
    if not syms:
        sys.exit("no TS3L10/TS2L08 read_snr symbols - not the GigaBlue dvb.ko?")

    changed = 0
    unpatched = 0
    for name in FUNCS:
        if name not in syms:
            print("%-16s not present" % name.decode())
            continue
        hits = find_branch(data, *syms[name])
        if len(hits) != 1:
            sys.exit("%s: expected one floor branch, found %d - driver changed"
                     % (name.decode(), len(hits)))
        off = hits[0]
        w, = struct.unpack_from("<I", data, off)
        cur = "floored at 6.75 dB (vendor)" if (w >> 28) == BGE_COND else "no floor (patched)"
        want_cond = w >> 28 if a.check else (BGE_COND if a.restore else AL_COND)
        new = (w & 0x0FFFFFFF) | (want_cond << 28)
        print("0x%06x  %-16s %s%s" % (off, name.decode(), cur,
              "" if new == w else "  ->  " +
              ("floored at 6.75 dB (vendor)" if want_cond == BGE_COND else "no floor (patched)")))
        if (w >> 28) == BGE_COND:
            unpatched += 1
        if new != w:
            struct.pack_into("<I", data, off, new)
            changed += 1
    if a.check:
        return 1 if unpatched else 0
    if changed:
        open(a.ko, "wb").write(data)
        print("patched %d instruction(s) in %s" % (changed, a.ko))
    elif not a.check:
        print("nothing to do")
    return 0


if __name__ == "__main__":
    sys.exit(main())
