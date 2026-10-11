SUMMARY = "Optional dvb.ko patch tools for GigaBlue gb7252 (gbbs)"
HOMEPAGE = "https://github.com/EB-TNAP/gbbs"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=75859989545e37968a99b631ef42722e"

inherit native

# Pin SRCREV to a gbbs commit in your layer for reproducible builds.
SRC_URI = "git://github.com/EB-TNAP/gbbs.git;protocol=https;branch=main;destsuffix=gbbs"
SRCREV = "${AUTOREV}"
PV = "git"

S = "${UNPACKDIR}/gbbs"

do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/dvb-patch/dvb-patch.sh ${S}/dvb-patch/patch-dvb-*.py ${D}${bindir}
}
