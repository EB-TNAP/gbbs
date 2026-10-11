#!/bin/sh
# dvb-patch.sh - optionally patch GigaBlue's prebuilt gb7252 dvb.ko.
#
# gbbs works with the stock dvb.ko (1-45 MS/s, frequencies in whole MHz).
# Each patch below is optional; nothing is changed unless it is asked for.
#
#   --srmin[=MIN:MAX]  symbol-rate range 1..45 MS/s -> MIN..MAX in S/s
#                      (default 100000:60000000): tune carriers below 1 MS/s
#                      or above 45 MS/s, and keep the range after a scan stops
#   --snr              SNR meter keeps moving below 6.75 dB (vendor: fixed 40 %)
#   --seekfreq         blindscan frequencies in kHz instead of whole MHz
#   --all              all three
#   --check            report the state of every patch, change nothing
#   --restore          put the vendor module back from the backup
#
# Usage: dvb-patch.sh [options] dvb.ko [dvb.ko ...]
#
# A copy of the vendor module is kept as <dvb.ko>.gbbs-orig before the first
# change. Each patcher finds its target by pattern and leaves a module it does
# not recognise untouched. Exit status: 0 ok, 1 error / not recognised.

dir=$(dirname "$0")
PY=python3
command -v $PY >/dev/null 2>&1 || PY=python

do_srmin=0 do_snr=0 do_seekfreq=0 mode=patch
srmin_args=

for a in "$@"; do
	case "$a" in
	--srmin)   do_srmin=1 ;;
	--srmin=*) do_srmin=1
		r=${a#--srmin=}
		srmin_args="--min ${r%%:*} --max ${r##*:}" ;;
	--snr)      do_snr=1 ;;
	--seekfreq) do_seekfreq=1 ;;
	--all)      do_srmin=1 do_snr=1 do_seekfreq=1 ;;
	--check)    mode=check ;;
	--restore)  mode=restore ;;
	-h|--help)  sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	-*) echo "dvb-patch.sh: unknown option $a" >&2; exit 1 ;;
	esac
done

rc=0
nfiles=0
for ko in "$@"; do
	case "$ko" in -*) continue ;; esac
	nfiles=$((nfiles + 1))
	if [ ! -f "$ko" ]; then
		echo "dvb-patch.sh: $ko: no such file" >&2
		rc=1
		continue
	fi
	case $mode in
	check)
		for p in srmin snr seekfreq; do
			echo "== $p"
			$PY "$dir/patch-dvb-$p.py" "$ko" --check
		done
		continue ;;
	restore)
		if [ -f "$ko.gbbs-orig" ]; then
			cp -p "$ko.gbbs-orig" "$ko" && echo "restored $ko"
		else
			echo "dvb-patch.sh: $ko: no $ko.gbbs-orig backup" >&2
			rc=1
		fi
		continue ;;
	esac
	[ $((do_srmin + do_snr + do_seekfreq)) -gt 0 ] || { echo "dvb-patch.sh: nothing selected (--srmin --snr --seekfreq --all)" >&2; exit 1; }
	tmp=$ko.gbbs-new
	cp -p "$ko" "$tmp" || { rc=1; continue; }
	ok=1
	[ $do_srmin -eq 1 ]    && { $PY "$dir/patch-dvb-srmin.py" "$tmp" $srmin_args >/dev/null || ok=0; }
	[ $do_snr -eq 1 ]      && { $PY "$dir/patch-dvb-snr.py" "$tmp" >/dev/null || ok=0; }
	[ $do_seekfreq -eq 1 ] && { $PY "$dir/patch-dvb-seekfreq.py" "$tmp" >/dev/null || ok=0; }
	if [ $ok -eq 1 ]; then
		[ -f "$ko.gbbs-orig" ] || cp -p "$ko" "$ko.gbbs-orig"
		mv -f "$tmp" "$ko" && echo "patched $ko"
	else
		rm -f "$tmp"
		echo "dvb-patch.sh: $ko: not recognised or patch failed - left unchanged" >&2
		rc=1
	fi
done
[ $nfiles -gt 0 ] || { echo "dvb-patch.sh: no dvb.ko given" >&2; exit 1; }
exit $rc
