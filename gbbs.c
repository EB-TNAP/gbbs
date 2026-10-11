/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 EB-TNAP
 * Licensed under the GNU General Public License, version 2 only (GPL-2.0-only).
 * See the LICENSE file in the source tree.
 *
 * gbbs v4.5 - open replacement for GigaBlue's closed "gigablue_blindscan" wrapper.
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
 *   gbbs reset nim_socket   (stop a seek a killed run left running)
 * sr_min / sr_max are Msps and may be decimal (0.3 = 300 ks/s).
 * Sockets named TS3L10/TS2L08 use the SiLabs seek engine (see seek_scan below);
 * FBC/BCM sockets use the Nexus peak-scan path.
 * Environment:
 *   GBBS_TIMEOUT=s  seek path: seconds without a result before resuming the
 *                   seek (default 125, just past the driver's 120 s step)
 *   GBBS_KICKS=n    seek path: resumes before a stalled pass is abandoned (default 3)
 *   GBBS_SRMIN_KSPS=k  seek path: override the minimum symbol rate in ks/s (e.g. 250)
 *   GBBS_LOWSR=0    seek path: don't run the separate sub-1 Msps pass
 *   GBBS_LOWSR_CHUNK=MHz    sub-1 Msps pass: seek this many MHz at a time
 *                           (default 0 = whole span in one pass)
 *   GBBS_LOWSR_TIMEOUT=s    sub-1 Msps pass: as GBBS_TIMEOUT, for the low pass (default 125)
 *   GBBS_LOWSR_REPEAT=n     sub-1 Msps pass: run the whole low pass n times (default 1)
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
		unsigned dsr = hits[i].sr > sr ? hits[i].sr - sr : sr - hits[i].sr;
		/* The vendor dvb.ko rounds seek hits to whole MHz, so the same narrow
		 * carrier can come back 1 MHz apart (11852 / 11853 at 593 kS/s): same
		 * SR within 2% and <= 1 MHz apart is the same carrier. With
		 * patch-dvb-seekfreq.py the driver reports kHz, and two SCPC
		 * carriers less than 1 MHz apart must stay separate, so the 1 MHz
		 * rule only applies when both hits are whole MHz. */
		if (dsr * 50 <= sr && half < 1000 &&
		    f_khz % 1000 == 0 && hits[i].f_khz % 1000 == 0)
			half = 1000;
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
 * The vendor TS3L10/TS2L08_Seek_Next round the SiLabs kHz result to whole MHz;
 * patch-dvb-seekfreq.py removes that rounding.
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

/* One SiLabs seek over [start, end] MHz IF for symbol rates srmin..srmax (sym/s).
 * Prints carriers not already reported by an earlier pass. Returns carriers printed,
 * or -1 if the seek engine could not be started. */
static int seek_pass(int fd, int fe, int start, int end, unsigned srmin, unsigned srmax,
                     int pol, unsigned lo_khz, int timeout)
{
	static struct seek_res r, prev;
	int ret, found = 0, max_kicks = 3;
	double t0 = now();
	const char *e;

	if ((e = getenv("GBBS_KICKS")) != NULL) max_kicks = atoi(e);
	if (max_kicks < 0) max_kicks = 0;

	ioctl(fd, SEEK_SELECT, (unsigned long)fe);
	ioctl(fd, SEEK_STOP);          /* clear any session a killed run / earlier pass left */

	struct bs_req req = { (unsigned)start * 1000000u, srmin, srmax,
	                      (unsigned)(end - start) * 1000000u, 0 };
	if (dbg)
		fprintf(stderr, "gbbs: SEEK pass fe %d IF %d-%d MHz SR %u-%u ks/s\n",
		        fe, start, end, srmin / 1000, srmax / 1000);
	sleep(2);                      /* let the driver tear down the previous seek thread */
	ret = ioctl(fd, SEEK_START, &req);
	if (ret != 1) {
		fprintf(stderr, "gbbs: seek start on fe %d returned %d (%s)\n", fe, ret,
		        ret < 0 ? strerror(errno) : "not a seek-capable NIM");
		ioctl(fd, SEEK_STOP);
		return -1;
	}
	memset(&prev, 0, sizeof(prev));

	while (!stop_req) {
		double ts = now();
		int kicks = 0;
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
				/* Si2183_L2_Channel_Seek_Next allows itself up to 120 s on
				 * satellite and can end that step without posting a result
				 * or an end-of-range. Abandoning the pass here threw away
				 * the rest of the span (and, at a short timeout, a search
				 * still in progress). Ask for the next step instead: the
				 * firmware seek continues from where it stopped. */
				if (kicks < max_kicks) {
					kicks++;
					if (dbg)
						fprintf(stderr, "  no result in %d s, resuming seek (%d/%d)\n",
						        timeout, kicks, max_kicks);
					ioctl(fd, SEEK_NEXT);
					usleep(150000);
					ts = now();
					continue;
				}
				fprintf(stderr, "gbbs: seek stalled %d x %d s, stopping pass\n",
				        max_kicks + 1, timeout);
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

		unsigned f_khz = r.freq / 1000 + lo_khz, sr = r.sr / 1000 * 1000;
		if (seen(f_khz, sr)) {
			if (dbg)
				fprintf(stderr, "  dup %u %u\n", f_khz, sr);
			continue;
		}
		found++;

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
	if (dbg)
		fprintf(stderr, "gbbs: pass SR %u-%u ks/s: %d new carriers, %.0f s%s\n",
		        srmin / 1000, srmax / 1000, found, now() - t0,
		        stop_req ? " (interrupted)" : "");
	return found;
}

/* The firmware seek with SCAN_SYMB_RATE_MIN at 1 Msps never reports narrower
 * carriers, and widening one combined pass risks changing what the normal pass
 * finds. So: the normal pass (>= 1 Msps) exactly as before, then - only if the
 * caller asked for less than 1 Msps - a separate sub-1 Msps pass. Results are
 * de-duplicated across passes. The patched dvb.ko (symbol_rate_min lowered)
 * is needed to tune anything this second pass finds. */
#define LOWSR_SPLIT 1000000u

static int seek_scan(int fe, const char *name, int start, int end, unsigned srmin,
                     unsigned srmax, int pol, int band)
{
	unsigned lo_khz = band ? 10600000 : 9750000;
	int timeout = 125, total = 0, n;   /* just past the driver's 120 s seek step */
	const char *e;
	double t0 = now();

	if ((e = getenv("GBBS_TIMEOUT")) != NULL) timeout = atoi(e);
	if (timeout < 5) timeout = 5;
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGHUP, on_sig);

	int fd = open("/dev/blindscan0", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "gbbs: open /dev/blindscan0: %s\n", strerror(errno));
		return 1;
	}
	if (dbg)
		fprintf(stderr, "gbbs: SEEK fe %d (%s) IF %d-%d MHz SR %u-%u ks/s pol %s band %d\n",
		        fe, name, start, end, srmin / 1000, srmax / 1000, pol_s[pol], band);

	int lowsr = srmin < LOWSR_SPLIT && srmax > LOWSR_SPLIT;
	if ((e = getenv("GBBS_LOWSR")) != NULL && !atoi(e))
		lowsr = 0;

	if (!lowsr) {
		n = seek_pass(fd, fe, start, end, srmin, srmax, pol, lo_khz, timeout);
	} else {
		/* Narrow carriers are detected only some of the time by the firmware
		 * seek, and a stalled seek forfeits the rest of its span. So the low
		 * pass works in short overlapping chunks, each with its own Seek_Init
		 * and a shorter stall limit, optionally repeated; dedup keeps the
		 * output clean. */
		int chunk = 0, ltimeout = 125, repeat = 1, rep, c;
		if ((e = getenv("GBBS_LOWSR_CHUNK")) != NULL) chunk = atoi(e);
		if ((e = getenv("GBBS_LOWSR_TIMEOUT")) != NULL) ltimeout = atoi(e);
		if ((e = getenv("GBBS_LOWSR_REPEAT")) != NULL) repeat = atoi(e);
		if (chunk <= 0 || chunk > end - start)
			chunk = end - start;     /* default: one pass, no thread restarts */
		else if (chunk < 4)
			chunk = 4;
		if (ltimeout < 5) ltimeout = 5;
		if (repeat < 1) repeat = 1;

		n = seek_pass(fd, fe, start, end, LOWSR_SPLIT, srmax, pol, lo_khz, timeout);
		if (n < 0) {
			close(fd);
			return 1;
		}
		total += n;
		for (rep = 0; rep < repeat && !stop_req; rep++)
			for (c = start; c < end && !stop_req; c += chunk) {
				int ce = c + chunk + 2;     /* 2 MHz overlap between chunks */
				if (ce > end) ce = end;
				n = seek_pass(fd, fe, c, ce, srmin, LOWSR_SPLIT, pol, lo_khz, ltimeout);
				if (n > 0) total += n;
			}
		n = 0;
	}
	close(fd);
	if (n < 0 && !total)
		return 1;
	total += n > 0 ? n : 0;
	if (dbg)
		fprintf(stderr, "gbbs: seek done, %d carriers, %.0f s%s\n", total, now() - t0,
		        stop_req ? " (interrupted)" : "");
	return 0;
}

/* Msps argument -> sym/s; accepts "2", "0.3", "0,3". */
static unsigned parse_msps(const char *s)
{
	char buf[32], *c;
	snprintf(buf, sizeof(buf), "%s", s);
	if ((c = strchr(buf, ',')) != NULL)
		*c = '.';
	double v = atof(buf);
	if (v < 0)
		v = 0;
	return (unsigned)(v * 1e6 + 0.5);
}

int main(int argc, char **argv)
{
	int start = 950, end = 2150, pol = 0, band = 0, fe = 0, step = 2;
	unsigned srmin = 2000000, srmax = 45000000;   /* sym/s */
	char name[128] = "";
	const char *e;

	dbg = getenv("GBBS_DEBUG") != NULL;

	if (argc == 2 && !strcmp(argv[1], "probe"))
		return probe();
	if (argc >= 2 && !strcmp(argv[1], "reset")) {
		/* Recover a tuner left in SiLabs seek mode by a killed scan (no signal
		 * until reboot otherwise). Same as what every seek run does first. */
		int nim = argc > 2 ? atoi(argv[2]) : ((e = getenv("GBBS_FE")) ? atoi(e) : -1);
		if (nim < 0) {
			fprintf(stderr, "usage: %s reset <nim_socket>\n", argv[0]);
			return 1;
		}
		int fd = open("/dev/blindscan0", O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "gbbs: open /dev/blindscan0: %s\n", strerror(errno));
			return 1;
		}
		ioctl(fd, SEEK_SELECT, (unsigned long)nim);
		int r = ioctl(fd, SEEK_STOP);
		close(fd);
		printf("gbbs: seek stopped on NIM %d (%d)\n", nim, r);
		return 0;
	}
	/* The stock helper prints usage when run bare; do not start a full scan. */
	if ((argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) ||
	    (argc == 1 && !getenv("GBBS_FE"))) {
		fprintf(stderr, "usage: %s start_if end_if srmin srmax pol(0=H,1=V) band(0=lo,1=hi) feid\n"
		                "       %s probe\n"
		                "       %s reset nim_socket\n"
		                "       srmin/srmax in Msps, decimals allowed (0.3 = 300 ks/s)\n"
		                "env:   GBBS_SRMIN_KSPS=k GBBS_LOWSR=0 GBBS_TIMEOUT=s GBBS_STEP=MHz\n"
		                "       GBBS_LOWSR_CHUNK=MHz GBBS_LOWSR_TIMEOUT=s GBBS_LOWSR_REPEAT=n GBBS_KICKS=n\n"
		                "       GBBS_FE=n GBBS_FORCE=1 GBBS_DEBUG=1\n", argv[0], argv[0], argv[0]);
		return 1;
	}
	if (argc > 1) start = atoi(argv[1]);
	if (argc > 2) end   = atoi(argv[2]);
	if (argc > 3) srmin = parse_msps(argv[3]);
	if (argc > 4) srmax = parse_msps(argv[4]);
	if (argc > 5) pol   = atoi(argv[5]) ? 1 : 0;
	if (argc > 6) band  = atoi(argv[6]);
	if (argc > 7) fe    = atoi(argv[7]);
	if ((e = getenv("GBBS_FE")) != NULL)   fe = atoi(e);
	if ((e = getenv("GBBS_STEP")) != NULL) step = atoi(e);
	if (step < 1) step = 1;
	if ((e = getenv("GBBS_SRMIN_KSPS")) != NULL) srmin = (unsigned)atoi(e) * 1000u;
	if (srmin < 100000) srmin = 100000;           /* below this the firmware is guessing */
	if (srmax < srmin) srmax = srmin;

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
	if (srmin < 10000000 || srmax > 30000000)
		fprintf(stderr, "gbbs: note: dvb.ko clamps peak scan SR to 10-30 Msps "
		                "(asked %.3g-%.3g)\n", srmin / 1e6, srmax / 1e6);

	int fd = open("/dev/blindscan0", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "gbbs: open /dev/blindscan0: %s\n", strerror(errno));
		return 1;
	}
	if (ioctl(fd, BS_SELECT, (unsigned long)fe))
		fprintf(stderr, "gbbs: select fe %d: %s\n", fe, strerror(errno));
	if (dbg)
		fprintf(stderr, "gbbs: fe %d (%s) IF %d-%d MHz step %d SR %.3g-%.3g pol %s band %d\n",
		        fe, name, start, end, step, srmin / 1e6, srmax / 1e6, pol_s[pol], band);
	sleep(3); /* stock tool does this too; lets the LNB settle after the plugin's tune */

	unsigned int lo_khz = band ? 10600000 : 9750000;
	double t0 = now();
	int f = start;
	while (f < end) {
		struct bs_req req = { (unsigned)f * 1000000u, srmin, srmax, 0, 0 };
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
