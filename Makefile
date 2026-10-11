# gbbs - GigaBlue blindscan helper
# Native build on the receiver:     make
# Cross build (static, ARMv7 hard-float, as shipped):
#   make CROSS_COMPILE=arm-linux-gnueabihf- STATIC=1
CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
STRIP   := $(CROSS_COMPILE)strip
CFLAGS  ?= -O2 -Wall -Wextra
ifeq ($(STATIC),1)
CFLAGS  += -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard
LDFLAGS += -static
endif
PREFIX  ?= /usr

gbbs: gbbs.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	$(STRIP) $@

install: gbbs
	install -D -m 0755 gbbs $(DESTDIR)$(PREFIX)/bin/gbbs

# Optional dvb.ko patches (all off by default), e.g.
#   make patch-dvb DVB_KO=/path/to/dvb.ko DVB_PATCHES="srmin snr seekfreq"
#   make patch-dvb DVB_KO=dvb.ko DVB_PATCHES=srmin DVB_SRMIN=250000:45000000
#   make check-dvb DVB_KO=dvb.ko
DVB_PATCHES ?=
DVB_SRMIN   ?=

patch-dvb:
	@test -n "$(DVB_KO)" || { echo "set DVB_KO=/path/to/dvb.ko"; exit 1; }
	@test -n "$(DVB_PATCHES)" || { echo "set DVB_PATCHES=\"srmin snr seekfreq\" (any subset)"; exit 1; }
	dvb-patch/dvb-patch.sh $(foreach p,$(DVB_PATCHES),$(if $(and $(filter srmin,$(p)),$(DVB_SRMIN)),--srmin=$(DVB_SRMIN),--$(p))) $(DVB_KO)

check-dvb:
	@test -n "$(DVB_KO)" || { echo "set DVB_KO=/path/to/dvb.ko"; exit 1; }
	dvb-patch/dvb-patch.sh --check $(DVB_KO)

clean:
	rm -f gbbs

.PHONY: install clean patch-dvb check-dvb
