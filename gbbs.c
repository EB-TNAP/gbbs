/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 EB-TNAP
 * Licensed under the GNU General Public License, version 2 only (GPL-2.0-only).
 * See the LICENSE file in the source tree.
 *
 * gbbs v3 - open replacement for GigaBlue's closed "gigablue_blindscan" wrapper.
 *
 * Reverse engineered from gigablue-blindscan-dvbs-utils-arm 4.0-r13 and the GigaBlue
 * 7252 dvb.ko (BuildID e7395bbeb437a240...). The work is done in dvb.ko's
 * dev_blindscan0_ioctl, which sits on Broadcom Nexus peak scan:
 *
 *   0x0a SELECT  arg BY VALUE = frontend index n. Uses NEXUS_PlatformConfiguration
 *                .frontend[n]. open() always resets to n=0 and the stock tool never
 *                calls 0x0a, which is why it only ever scans Tuner A.
 *   0x0b PEAK    in {freq_hz, sr_min, sr_max, -, -} -> NEXUS_Frontend_SatellitePeakscan,
 *                waits up to 2 s. The driver CLAMPS sr_min up to 10 Msps and sr_max down
 *                to 30 Msps; frequencyRange/Step stay at Nexus defaults.
 *                Returns 0 = ok, 5 = timeout.
 *   0x0c LOCK    GetSatellitePeakscanResult; if a peak was found, TuneSatellite at the
 *                peak with mode 17, then mode 10 on timeout (2 s each), then checks
 *                demod lock. It rejects a lock within 9 MHz of the PREVIOUS lock (and
 *                resets that memory on any failure). Returns 0 on success.
 *   0x0d STATUS  out {freq_hz(IF), sr, delsys(5/6/21), mod, inv, fec, pilot, rolloff}
 *
 * dmesg carries the driver's own trace: "try> / peak> / tune> / lock> ..."
 *
 * Command line (as sent by the OE-A Blindscan plugin):
 *   gbbs start_if end_if sr_min sr_max pol(0=H 1=V) band(0=lo 1=hi) feid [i2c...]
 *   gbbs probe
 * Sockets named TS3L10/TS2L08 use the SiLabs seek engine (see seek_scan below);
 * FBC/BCM sockets use the Nexus peak-scan path.
 * Environment:
 *   GBBS_TIMEOUT=s  seek path: max seconds to wait for one carrier (default 180)
 *   GBBS_STEP=MHz   step between peak-scan centres (default 2)
 *   GBBS_FE=n       force frontend index (default: feid argument)
 *   GBBS_FORCE=1    skip the "is this a Broadcom/Nexus NIM" safety check
 *   GBBS_DEBUG=1    per-step ioctl trace on stderr
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#define BS_SELECT 0x0a
#define BS_PEAK   0x0b
#define BS_LOCK   0x0c
#define BS_STATUS 0x0d

struct bs_req { unsigned int freq, sr_min, sr_max, r0, r1; };
struct bs_res { unsigned int freq, sr, delsys, modulation, inversion, fec, pilot, rolloff; };

/* strings chosen to match what the OE-A plugin's parser accepts */
static const char *pol_s[] = { "HORIZONTAL", "VERTICAL" };
static const char *inv_s[] = { "INVERSION_OFF", "INVERSION_ON", "INVERSION_AUTO" };
static const char *fec_s[] = { "FEC_NONE", "FEC_1_2", "FEC_2_3", "FEC_3_4", "FEC_4_5", "FEC_5_6",
                               "FEC_6_7", "FEC_7_8", "FEC_8_9", "FEC_AUTO", "FEC_3_5", "FEC_9_10" };
static const char *pil_s[] = { "PILOT_ON", "PILOT_OFF", "PILOT_AUTO" };
static const char *ro_s[]  = { "ROLLOFF_35", "ROLLOFF_20", "ROLLOFF_25", "ROLLOFF_AUTO" };
#define PICK(tab, i, dflt) ((i) < sizeof(tab) / sizeof(tab[0]) ? (tab)[i] : (dflt))

static const char *sys_str(unsigned d)
{
	return d == 21 ? "DVB-S2X" : d == 6 ? "DVB-S2" : "DVB-S";
}

static const char *mod_str(unsigned m)
{
	switch (m) {
	case 9:  return "8PSK";
	case 10: return "16APSK";
	case 11: return "32APSK";
	default: return "QPSK";
	}
}

static int dbg;

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Copy the Name: of NIM socket n from /proc/bus/nim_sockets into buf. */
static int nim_name(int n, char *buf, size_t len)
{
	char line[256], hdr[32];
	int in = 0;
	FILE *f = fopen("/proc/bus/nim_sockets", "r");
	if (!f)
		return -1;
	snprintf(hdr, sizeof(hdr), "NIM Socket %d:", n);
	buf[0] = 0;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "NIM Socket ", 11)) {
			in = !strncmp(line, hdr, strlen(hdr));
			continue;
		}
		if (in) {
			char *p = strstr(line, "Name:");
			if (p) {
				p += 5;
				while (*p == ' ' || *p == '\t') p++;
				p[strcspn(p, "\r\n")] = 0;
				snprintf(buf, len, "%s", p);
				break;
			}
		}
	}
	fclose(f);
	return buf[0] ? 0 : -1;
}

static int probe(void)
{
	char path[32], name[128];
	int i, found = 0;
	for (i = 0; i < 16; i++) {
		struct stat st;
		snprintf(path, sizeof(path), "/dev/blindscan%d", i);
		if (stat(path, &st))
			continue;
		found++;
		int fd = open(path, O_RDWR);
		printf("%s  major %u minor %u  open: %s\n", path,
		       (unsigned)(st.st_rdev >> 8) & 0xfff, (unsigned)(st.st_rdev & 0xff),
		       fd < 0 ? strerror(errno) : "ok");
		if (fd >= 0)
			close(fd);
	}
	if (!found)
		printf("no /dev/blindscan* nodes (check: grep -i blind /proc/devices)\n");
	for (i = 0; i < 16; i++)
		if (!nim_name(i, name, sizeof(name)))
			printf("NIM %d: %-32s %s\n", i, name,
			       (strstr(name, "TS3L10") || strstr(name, "TS2L08")) ? "SiLabs seek path" :
			       (strstr(name, "FBC") || strstr(name, "BCM")) ? "Nexus peak-scan path" :
			       "not supported");
	return 0;
}

#define MAXHITS 512
static struct { unsigned f_khz, sr; } hits[MAXHITS];
static int nhits;

static int seen(unsigned f_khz, unsigned sr)
{
	int i;
	for (i = 0; i < nhits; i++) {
		unsigned half = (hits[i].sr > sr ? hits[i].sr : sr) / 2000; /* kHz */
		unsigned d = f_khz > hits[i].f_khz ? f_khz - hits[i].f_khz : hits[i].f_khz - f_khz;
		if (d <= half)
			return 1;
	}
	if (nhits < MAXHITS) {
		hits[nhits].f_khz = f_khz;
		hits[nhits].sr = sr;
		nhits++;
	}
	return 0;
}

/* ------------------------------------------------------------------------------
 * Seek path for the GigaBlue TS3L10 / TS2L08 plug-in NIMs (SiLabs Si2183-family).
 * Same /dev/blindscan0 node, different ioctls (dev_blindscan0_ioctl + autoscan_seek_thread):
 *
 *   0x6e SEEK_SELECT  arg BY VALUE = frontend number (NIM socket, e.g. 8)
 *   0x6f SEEK_START   in {start_if_hz, sr_min, sr_max, span_hz, 0}
 *                     -> SiLabs_API_Channel_Seek_Init(start..start+span kHz, SR min..max)
 *                     and spawns the seek thread. Returns 1 if started, 0 if not a seek NIM.
 *   0x70 SEEK_NEXT    wakes the thread for ONE SiLabs_API_Channel_Seek_Next()
 *   0x71 SEEK_STOP    abort + end + destroy thread (ALWAYS call it, also on exit)
 *   0x72 SEEK_RESULT  1 = result copied, 0 = still seeking, -1 = scan finished
 *                     out: same 8 words as the peak path, then u8 n_isi @0x20,
 *                     u32 isi[] @0x24 (only for multistream); struct is 0x1410 bytes
 * Seek_Next reports frequency rounded to whole MHz.
 * ------------------------------------------------------------------------------ */
#define SEEK_SELECT 0x6e
#define SEEK_START  0x6f
#define SEEK_NEXT   0x70
#define SEEK_STOP   0x71
#define SEEK_RESULT 0x72

struct seek_res {
	unsigned int freq, sr, delsys, modulation, inversion, fec, pilot, rolloff;
	unsigned char n_isi, pad[3];
	unsigned int isi[255];
	unsigned char rest[0x1410 - 0x24 - 255 * 4];
};

#include <signal.h>
static volatile sig_atomic_t stop_req;
static void on_sig(int s) { (void)s; stop_req = 1; }

static int seek_scan(int fe, const char *name, int start, int end, int srmin, int srmax,
                     int pol, int band)
{
	static struct seek_res r, prev;
	unsigned int lo_khz = band ? 10600000 : 9750000;
	int timeout = 180, ret, found = 0;
	const char *e;
	double t0 = now();

	if ((e = getenv("GBBS_TIMEOUT")) != NULL) timeout = atoi(e);
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGHUP, on_sig);

	int fd = open("/dev/blindscan0", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "gbbs: open /dev/blindscan0: %s\n", strerror(errno));
		return 1;
	}
	ioctl(fd, SEEK_SELECT, (unsigned long)fe);
	ioctl(fd, SEEK_STOP);          /* clear any session a killed run left behind */

	struct bs_req req = { (unsigned)start * 1000000u, (unsigned)srmin * 1000000u,
	                      (unsigned)srmax * 1000000u, (unsigned)(end - start) * 1000000u, 0 };
	if (dbg)
		fprintf(stderr, "gbbs: SEEK fe %d (%s) IF %d-%d MHz SR %d-%d pol %s band %d\n",
		        fe, name, start, end, srmin, srmax, pol_s[pol], band);
	sleep(1);
	ret = ioctl(fd, SEEK_START, &req);
	if (ret != 1) {
		fprintf(stderr, "gbbs: seek start on fe %d returned %d (%s)\n", fe, ret,
		        ret < 0 ? strerror(errno) : "not a seek-capable NIM");
		ioctl(fd, SEEK_STOP);
		close(fd);
		return 1;
	}
	memset(&prev, 0, sizeof(prev));

	while (!stop_req) {
		double ts = now();
		ioctl(fd, SEEK_NEXT);
		usleep(150000);          /* let the thread clear the previous result flag */
		for (;;) {
			if (stop_req)
				goto out;
			ret = ioctl(fd, SEEK_RESULT, &r);
			if (ret < 0)
				goto out;        /* seek finished */
			if (ret == 1 && !(r.freq == prev.freq && r.sr == prev.sr))
				break;
			if (now() - ts > timeout) {
				fprintf(stderr, "gbbs: no answer from seek in %d s, stopping\n", timeout);
				goto out;
			}
			usleep(200000);
		}
		prev = r;
		if (dbg)
			fprintf(stderr, "  hit IF %u kHz SR %u sys %u mod %u fec %u isi %u  %.1fs\n",
			        r.freq / 1000, r.sr, r.delsys, r.modulation, r.fec, r.n_isi, now() - ts);
		if (!r.freq || !r.sr)
			continue;
		found++;

		unsigned f_khz = r.freq / 1000 + lo_khz, sr = r.sr / 1000 * 1000;
		const char *sys = sys_str(r.delsys);
		const char *fec = PICK(fec_s, r.fec, "FEC_AUTO"), *mod = mod_str(r.modulation);
		int i, n = r.n_isi > 1 ? r.n_isi : 0;

		if (!n)
			printf("OK %s %u %u %s INVERSION_AUTO PILOT_AUTO %s %s ROLLOFF_AUTO\n",
			       pol_s[pol], f_khz, sr, sys, fec, mod);
		for (i = 0; i < n && i < 255; i++)   /* one line per ISI, gold code 0 */
			printf("OK %s %u %u %s INVERSION_AUTO PILOT_AUTO %s %s ROLLOFF_AUTO 1 %u 0\n",
			       pol_s[pol], f_khz, sr, sys, fec, mod, r.isi[i]);
		fflush(stdout);
	}
out:
	ioctl(fd, SEEK_STOP);
	close(fd);
	if (dbg)
		fprintf(stderr, "gbbs: seek done, %d carriers, %.0f s%s\n", found, now() - t0,
		        stop_req ? " (interrupted)" : "");
	return 0;
}

int main(int argc, char **argv)
{
	int start = 950, end = 2150, srmin = 2, srmax = 45, pol = 0, band = 0, fe = 0, step = 2;
	char name[128] = "";
	const char *e;

	dbg = getenv("GBBS_DEBUG") != NULL;

	if (argc == 2 && !strcmp(argv[1], "probe"))
		return probe();
	if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		fprintf(stderr, "usage: %s start_if end_if srmin srmax pol(0=H,1=V) band(0=lo,1=hi) feid\n"
		                "       %s probe\n"
		                "env:   GBBS_STEP=MHz GBBS_FE=n GBBS_FORCE=1 GBBS_DEBUG=1\n", argv[0], argv[0]);
		return 1;
	}
	if (argc > 1) start = atoi(argv[1]);
	if (argc > 2) end   = atoi(argv[2]);
	if (argc > 3) srmin = atoi(argv[3]);
	if (argc > 4) srmax = atoi(argv[4]);
	if (argc > 5) pol   = atoi(argv[5]) ? 1 : 0;
	if (argc > 6) band  = atoi(argv[6]);
	if (argc > 7) fe    = atoi(argv[7]);
	if ((e = getenv("GBBS_FE")) != NULL)   fe = atoi(e);
	if ((e = getenv("GBBS_STEP")) != NULL) step = atoi(e);
	if (step < 1) step = 1;

	/* TS3L10 / TS2L08 plug-in NIMs: SiLabs firmware seek engine */
	if (nim_name(fe, name, sizeof(name)) == 0 &&
	    (strstr(name, "TS3L10") || strstr(name, "TS2L08")))
		return seek_scan(fe, name, start, end, srmin, srmax, pol, band);

	/* dev_blindscan0_ioctl only knows Nexus frontends; the TS3L10/TS2L08 "mtuner"
	 * sockets have no Nexus handle there and would hand NULL to Nexus. */
	if (!getenv("GBBS_FORCE") && name[0] &&
	    !(strstr(name, "FBC") || strstr(name, "BCM"))) {
		fprintf(stderr, "gbbs: NIM %d is '%s' - not a Nexus peakscan tuner, refusing "
		                "(GBBS_FORCE=1 to override, at your own risk)\n", fe, name);
		return 1;
	}
	if (srmin < 10 || srmax > 30)
		fprintf(stderr, "gbbs: note: dvb.ko clamps peak scan SR to 10-30 Msps "
		                "(asked %d-%d)\n", srmin, srmax);

	int fd = open("/dev/blindscan0", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "gbbs: open /dev/blindscan0: %s\n", strerror(errno));
		return 1;
	}
	if (ioctl(fd, BS_SELECT, (unsigned long)fe))
		fprintf(stderr, "gbbs: select fe %d: %s\n", fe, strerror(errno));
	if (dbg)
		fprintf(stderr, "gbbs: fe %d (%s) IF %d-%d MHz step %d SR %d-%d pol %s band %d\n",
		        fe, name, start, end, step, srmin, srmax, pol_s[pol], band);
	sleep(3); /* stock tool does this too; lets the LNB settle after the plugin's tune */

	unsigned int lo_khz = band ? 10600000 : 9750000;
	double t0 = now();
	int f = start;
	while (f < end) {
		struct bs_req req = { (unsigned)f * 1000000u, (unsigned)srmin * 1000000u,
		                      (unsigned)srmax * 1000000u, 0, 0 };
		struct bs_res r;
		int next = f + step;
		double ts = now();
		int ret;

		memset(&r, 0, sizeof(r));
		ret = ioctl(fd, BS_PEAK, &req);
		if (dbg) fprintf(stderr, "  %4d PEAK=%d", f, ret);
		if (ret == 0) {
			ret = ioctl(fd, BS_LOCK);
			if (dbg) fprintf(stderr, " LOCK=%d", ret);
			if (ret == 0) {
				ret = ioctl(fd, BS_STATUS, &r);
				if (dbg) fprintf(stderr, " STATUS=%d {%u %u %u %u %u %u %u %u}", ret,
				                 r.freq, r.sr, r.delsys, r.modulation, r.inversion,
				                 r.fec, r.pilot, r.rolloff);
				if (ret == 0 && r.freq && r.sr) {
					unsigned f_khz = r.freq / 1000 + lo_khz;
					unsigned sr = r.sr / 1000 * 1000;
					int if_mhz = r.freq / 1000000;
					int half = (int)(r.sr * 1.35 / 2e6) + 1; /* occupied half-BW, MHz */

					if (!seen(f_khz, sr)) {
						printf("OK %s %u %u %s %s %s %s %s %s\n", pol_s[pol], f_khz, sr,
						       sys_str(r.delsys), PICK(inv_s, r.inversion, "INVERSION_AUTO"),
						       PICK(pil_s, r.pilot, "PILOT_AUTO"), PICK(fec_s, r.fec, "FEC_AUTO"),
						       mod_str(r.modulation), PICK(ro_s, r.rolloff, "ROLLOFF_AUTO"));
						fflush(stdout);
					} else if (dbg)
						fprintf(stderr, " dup");
					/* don't re-probe inside the carrier we just locked */
					if (next >= if_mhz - half && next <= if_mhz + half)
						next = if_mhz + half + 1;
				}
			}
		}
		if (dbg) fprintf(stderr, "  %.1fs\n", now() - ts);
		f = next;
	}
	if (dbg)
		fprintf(stderr, "gbbs: %d transponders, %.0f s\n", nhits, now() - t0);
	close(fd);
	return 0;
}
