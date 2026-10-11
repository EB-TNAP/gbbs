#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# patch-dvb-seekfreq.py - let GigaBlue's prebuilt dvb.ko report blindscan
# (channel seek) frequencies to the kHz for the TS3L10 / TS2L08 plug-in NIMs.
#
# The SiLabs API returns the carrier frequency of each seek hit in kHz.
# TS3L10_Seek_Next / TS2L08_Seek_Next then round it to whole MHz before
# storing it for the /dev/blindscan0 SEEK_RESULT ioctl:
#
#     f = freq_khz / 1000; if (freq_khz % 1000 > 500) f++;
#     freq_khz = f * 1000;                 <- str r0, [sp, #40]
#     fe->seek_freq = freq_khz;            <- ldr r3, [sp, #40]; str r3, [r4, #584]
#
# For a 447 kS/s carrier that is an error of up to 500 kHz, more than its
# whole bandwidth, and two SCPC carriers in the same MHz come back as one.
# The fix is to skip the store of the rounded value (str -> nop), so the
# kHz value the SiLabs API returned is passed on unchanged.
#
# Usage:
#   patch-dvb-seekfreq.py dvb.ko             # apply
#   patch-dvb-seekfreq.py dvb.ko --restore   # back to whole MHz
#   patch-dvb-seekfreq.py dvb.ko --check     # report only, exit 1 if not patched
#
# Stdlib only.

import argparse
import struct
import sys

FUNCS = (b"TS3L10_Seek_Next", b"TS2L08_Seek_Next")
STR_ROUNDED = 0xE58D0028      # str r0, [sp, #40]
NOP = 0xE1A00000              # mov r0, r0
LDR_BACK = 0xE59D3028         # ldr r3, [sp, #40]
STR_FE = 0xE5843248           # str r3, [r4, #584]
CMP_500 = 0xE3510F7D          # cmp r1, #500
MUL_1000 = 0xE0000093         # mul r0, r3, r0


def symbols(data):
	if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
		sys.exit("not a 32-bit little-endian ELF")
	shoff, = struct.unpack_from("<I", data, 0x20)
	shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
	secs = [struct.unpack_from("<10I", data, shoff + i * shentsize) for i in range(shnum)]
	out = {}
	for s in secs:
		if s[1] != 2:
			continue
		strtab = secs[s[6]]
		for j in range(s[5] // 16):
			name, value, size, _i, _o, shndx = struct.unpack_from("<IIIBBH", data, s[4] + j * 16)
			p = strtab[4] + name
			nm = bytes(data[p:data.index(b"\0", p)])
			if nm in FUNCS:
				out[nm] = (secs[shndx][4] + value, size)
	return out


def find_site(data, start, size):
	hits = []
	for off in range(start + 16, start + size - 8, 4):
		w, b, c = struct.unpack_from("<3I", data, off)
		if w in (STR_ROUNDED, NOP) and b == LDR_BACK and c == STR_FE:
			window = struct.unpack_from("<%dI" % 24, data, off - 96)
			if CMP_500 in window and MUL_1000 in window:
				hits.append(off)
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
		sys.exit("no TS3L10/TS2L08 Seek_Next symbols - not the GigaBlue dvb.ko?")

	changed = unpatched = 0
	for name in FUNCS:
		if name not in syms:
			print("%-17s not present" % name.decode())
			continue
		hits = find_site(data, *syms[name])
		if len(hits) != 1:
			sys.exit("%s: expected one MHz rounding store, found %d - driver changed" % (name.decode(), len(hits)))
		off = hits[0]
		w, = struct.unpack_from("<I", data, off)
		cur = "whole MHz (vendor)" if w == STR_ROUNDED else "kHz (patched)"
		want = w if a.check else (STR_ROUNDED if a.restore else NOP)
		print("0x%06x  %-17s %s%s" % (off, name.decode(), cur,
		      "" if want == w else "  ->  " + ("whole MHz (vendor)" if want == STR_ROUNDED else "kHz (patched)")))
		if w == STR_ROUNDED:
			unpatched += 1
		if want != w:
			struct.pack_into("<I", data, off, want)
			changed += 1
	if a.check:
		return 1 if unpatched else 0
	if changed:
		open(a.ko, "wb").write(data)
		print("patched %d instruction(s) in %s" % (changed, a.ko))
	else:
		print("nothing to do")
	return 0


if __name__ == "__main__":
	sys.exit(main())
