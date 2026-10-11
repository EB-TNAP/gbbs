# gbbs-dvb-patch.bbclass - optionally patch GigaBlue's prebuilt dvb.ko in an
# image rootfs. Does nothing unless GBBS_DVB_PATCHES is set.
#
# local.conf or the image recipe:
#
#   IMAGE_CLASSES += "gbbs-dvb-patch"
#   GBBS_DVB_PATCH_DIR = "/path/to/gbbs/dvb-patch"
#   GBBS_DVB_PATCHES = "srmin snr seekfreq"      # any subset, or leave empty
#   GBBS_DVB_SRMIN = "250000:45000000"           # optional MIN:MAX in S/s
#
# srmin     symbol-rate range 1..45 MS/s -> 0.1..60 MS/s
# snr       SNR meter keeps moving below 6.75 dB
# seekfreq  blindscan frequencies in kHz instead of whole MHz

GBBS_DVB_PATCH_DIR ?= ""
GBBS_DVB_PATCHES ?= ""
GBBS_DVB_SRMIN ?= ""

ROOTFS_POSTPROCESS_COMMAND:append = " gbbs_dvb_patch;"

gbbs_dvb_patch() {
    [ -n "${GBBS_DVB_PATCHES}" ] || return 0
    if [ ! -x "${GBBS_DVB_PATCH_DIR}/dvb-patch.sh" ]; then
        bbfatal "GBBS_DVB_PATCH_DIR must point at the gbbs dvb-patch directory (dvb-patch.sh not found)"
    fi
    opts=""
    for p in ${GBBS_DVB_PATCHES}; do
        case "$p" in
        srmin)
            if [ -n "${GBBS_DVB_SRMIN}" ]; then
                opts="$opts --srmin=${GBBS_DVB_SRMIN}"
            else
                opts="$opts --srmin"
            fi ;;
        snr|seekfreq) opts="$opts --$p" ;;
        *) bbfatal "GBBS_DVB_PATCHES: unknown patch '$p' (srmin snr seekfreq)" ;;
        esac
    done
    found=0
    for ko in ${IMAGE_ROOTFS}/home/root/platform/dvb.ko \
              ${IMAGE_ROOTFS}/usr/share/platform/dvb.ko \
              ${IMAGE_ROOTFS}/lib/modules/*/extra/dvb.ko; do
        [ -f "$ko" ] || continue
        found=1
        "${GBBS_DVB_PATCH_DIR}/dvb-patch.sh" $opts "$ko" || bbfatal "dvb.ko patch failed: $ko"
        rm -f "$ko.gbbs-orig"
    done
    [ $found = 1 ] || bbwarn "GBBS_DVB_PATCHES is set but no dvb.ko was found in the rootfs"
}
