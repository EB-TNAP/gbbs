# gbbs — GigaBlue blindscan

An open replacement for GigaBlue's closed `gigablue_blindscan` helper. It gives
the **GigaBlue plug-and-play TS3L10 / TS2L08 DVB-S2X tuners** a working
blindscan: DVB-S, DVB-S2 and DVB-S2X carriers from 1 to 45 MS/s, including
multistream ISIs, plus an optional pass for narrow carriers below 1 MS/s. It does this through the SiLabs firmware channel-seek engine
that is already inside GigaBlue's `dvb.ko` but was never used by anything in
userspace.

Tested on a GigaBlue UE 4K (BCM7252s) under OpenViX and TNAP: a full
950–2150 MHz pass on one polarisation takes about two minutes.

## Why

On 7252-based GigaBlue receivers the stock `gigablue_blindscan`:

- ignores the tuner number it is given and always opens frontend 0, so every
  "blindscan" — including one started on a plug-and-play tuner — actually runs
  on FBC tuner A;
- only drives the Broadcom Nexus peak-scan path, which the driver clamps to
  10–30 MS/s.

`dvb.ko` also contains a complete SiLabs seek interface for the TS3L10/TS2L08
NIMs on the same `/dev/blindscan0` node. gbbs uses it.

## Driver interface (reverse engineered, GigaBlue 7252 `dvb.ko`)

All ioctls are raw numbers (not `_IOC` encoded) on `/dev/blindscan0`.

SiLabs seek path — TS3L10 / TS2L08 sockets:

| ioctl | name | notes |
|---|---|---|
| `0x6e` | select | argument **by value**: NIM socket number |
| `0x6f` | start | in `{start_if_hz, sr_min, sr_max, span_hz, 0}`; returns 1 when started |
| `0x70` | next | wakes the seek thread for one `SiLabs_API_Channel_Seek_Next()` |
| `0x71` | stop | abort, end and destroy the thread — always call it |
| `0x72` | result | 1 = result, 0 = still seeking, -1 = finished; 0x1410-byte struct |

Nexus peak-scan path — Broadcom 45308X FBC sockets:

| ioctl | name | notes |
|---|---|---|
| `0x0a` | select | argument by value: frontend index (`open()` resets it to 0) |
| `0x0b` | peak | `NEXUS_Frontend_SatellitePeakscan`; SR clamped to 10–30 MS/s by the driver |
| `0x0c` | lock | tune to the peak and check lock; drops a lock within 9 MHz of the previous one |
| `0x0d` | status | `{freq_hz (IF), sr, delsys 5/6/21, mod, inv, fec, pilot, rolloff}` |

The driver's own trace (`try> / peak> / tune> / lock>`) appears in `dmesg`.

## Usage

The command line matches what the OE-A Blindscan plugin passes to vendor
blindscan helpers, so gbbs is a drop-in:

```
gbbs start_if_MHz end_if_MHz sr_min sr_max pol(0=H,1=V) band(0=low,1=high) nim_socket
gbbs probe          # list /dev/blindscan* and which path each NIM uses
gbbs reset 9        # stop a seek left running on NIM 9 by a killed scan
```

Each carrier is printed as one line in the vuplus_blindscan format:

```
OK VERTICAL 11833000 4998000 DVB-S2X INVERSION_AUTO PILOT_AUTO FEC_3_4 16APSK ROLLOFF_AUTO
```

gbbs does not switch LNB voltage or tone. The caller must tune the tuner to
the wanted polarisation and band first, as the Blindscan plugins do.

`sr_min` / `sr_max` are in MS/s and may be decimal (`0.25` = 250 kS/s).

### Narrow carriers (below 1 MS/s)

When `sr_min` is below 1 MS/s, the seek path runs two passes: the normal
pass from 1 MS/s up (unchanged), then a separate pass from `sr_min` to
1 MS/s. Carriers found by both are reported once. Asking the SiLabs seek
engine for 1 MS/s and up never reports narrower carriers, so passing `1`
from a plugin hides them.

The second pass covers the whole span in one seek by default; restarting
the driver's seek thread per chunk proved fragile. `GBBS_LOWSR_CHUNK=MHz`
still splits it into overlapping chunks, and the pass can be repeated
(`GBBS_LOWSR_REPEAT`, default 1).

A seek step that returns nothing within `GBBS_TIMEOUT` / `GBBS_LOWSR_TIMEOUT`
seconds (default 125, just past the driver's own 120 s limit per step) is
resumed with another seek-next rather than abandoning the pass, up to
`GBBS_KICKS` times (default 3). Carriers reported 1 MHz apart at the same
symbol rate (whole-MHz rounding) are reported once.

Tuning what the second pass finds needs GigaBlue's `dvb.ko` patched to lower
its registered `symbol_rate_min` (vendor value 1000000); otherwise the kernel
rejects the tune with `symbol rate … out of range`.

Environment: `GBBS_DEBUG=1` (trace on stderr), `GBBS_TIMEOUT=s` (seek path,
seconds without a result before resuming, default 125), `GBBS_KICKS=n`, `GBBS_SRMIN_KSPS=k` (seek path, override the
minimum symbol rate in kS/s), `GBBS_LOWSR=0` (skip the sub-1 MS/s pass),
`GBBS_STEP=MHz`, `GBBS_FE=n`, `GBBS_FORCE=1` (peak-scan path).

## Build

```
make CROSS_COMPILE=arm-linux-gnueabihf- STATIC=1
```

This produces a stripped static ARMv7 binary with no runtime dependencies.
Install it as `/usr/bin/gbbs`.

## Plugin integration

- OE-Alliance `SystemPlugins/Blindscan`: route NIMs whose description contains
  `TS3L10` or `TS2L08` on GigaBlue boxes to `/usr/bin/gbbs`, and skip the i2c
  lookup for them. The plugin's `/proc/bus/nim_sockets` parser cannot parse the
  name `GIGA DVB-S2X NIM (TS3L10)`.
- TNAP Blindscan: integrated, with live display of carriers as they are found
  and a `/tmp/gbbs_*.txt` list that keeps the DVB-S2X identity.

## Limitations

- Frequencies from the seek engine are rounded to whole MHz by the driver.
- PLS codes are not reported, so scrambled-PLS multistream will not lock.
- The FBC peak-scan path is limited by the driver to large-SR carriers. The
  FBC tuners are better left excluded from blindscan.

## License

gbbs is free software, licensed under the GNU General Public License,
version 2 only (GPL-2.0-only). See [LICENSE](LICENSE).

## Download

A prebuilt static ARMv7 binary is attached to each
[release](../../releases). Copy it to `/usr/bin/gbbs` on the receiver and make
it executable.

## Stopping a scan

gbbs stops the seek on SIGINT/SIGTERM/SIGHUP. If it is killed outright
(SIGKILL - enigma2's eConsoleAppContainer does this when a plugin drops it)
the driver's seek thread keeps running and the tuner has no signal until a
reboot. Run `gbbs reset <nim_socket>` after killing it; the TNAP Blindscan
plugin does this when a scan is cancelled.

## Optional dvb.ko patches

gbbs works with the stock GigaBlue `dvb.ko`: 1 to 45 MS/s, frequencies in
whole MHz. Three optional patches for the prebuilt module live in
`dvb-patch/`; none is applied unless asked for, and each is independent:

| Option | Stock `dvb.ko` | Patched |
|---|---|---|
| `srmin` | symbol-rate range 1..45 MS/s, restored after a scan stops | 0.1..60 MS/s (or `MIN:MAX`), kept after a scan |
| `snr` | SNR meter frozen at 40 % below 6.75 dB | meter keeps moving |
| `seekfreq` | blindscan frequencies rounded to whole MHz | kHz |

```
dvb-patch/dvb-patch.sh --check dvb.ko                 # report only
dvb-patch/dvb-patch.sh --srmin --seekfreq dvb.ko      # any subset, or --all
dvb-patch/dvb-patch.sh --srmin=250000:45000000 dvb.ko
dvb-patch/dvb-patch.sh --restore dvb.ko               # from dvb.ko.gbbs-orig
make patch-dvb DVB_KO=dvb.ko DVB_PATCHES="srmin snr"
```

Each patcher finds its target by pattern and leaves a module it does not
recognise untouched. Image builders can apply a chosen subset to the rootfs
with `oe/gbbs-dvb-patch.bbclass` (`GBBS_DVB_PATCHES = "srmin snr seekfreq"`;
empty means no change).
