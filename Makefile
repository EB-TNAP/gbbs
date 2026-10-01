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

clean:
	rm -f gbbs

.PHONY: install clean
