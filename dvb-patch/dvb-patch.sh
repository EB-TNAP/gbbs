#!/bin/sh
# dvb-patch.sh - optionally patch GigaBlue's prebuilt gb7252 dvb.ko.
#
# gbbs works with the stock dvb.ko (1-45 MS/s, frequencies in whole MHz).
# Each patch below is optional; nothing is changed unless it is asked for.
#
#   --srmin[=MIN[:MAX]] symbol-rate range 1..45 MS/s -> MIN..MAX in S/s
#                      (default 100000:60000000; --srmin=250000 sets only MIN): tune carriers below 1 MS/s
#                      or above 45 MS/s, and keep the range after a scan stops
#   --snr              SNR meter keeps moving below 6.75 dB (vendor: fixed 40 %)
#   --seekfreq         blindscan frequencies in kHz instead of whole MHz
#   --all              all three
#   --check            report the state of the selected patches (all if none
#                      selected), change nothing; exit 1 unless every one is
#                      applied
#   --restore          put the vendor module back from the backup (also reads
#                      the .gb7252-pnp.orig copy of the gb7252-pnp-blindscan ipk)
#
# Usage: dvb-patch.sh [options] dvb.ko [dvb.ko ...]
#
# A copy of the vendor module is kept as <dvb.ko>.gbbs-orig before the first
# change, unless the module already carries a patch (then there is no vendor
# copy to keep and --restore is not possible). Each patcher finds its target by pattern and leaves a module it does
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
		case "$r" in
		*:*) min=${r%%:*} max=${r#*:}
			srmin_args=
			[ -n "$min" ] && srmin_args="--min $min"
			[ -n "$max" ] && srmin_args="$srmin_args --max $max" ;;
		*) srmin_args="--min $r" ;;
		esac ;;
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
		sel=
		[ $do_srmin -eq 1 ]    && sel="$sel srmin"
		[ $do_snr -eq 1 ]      && sel="$sel snr"
		[ $do_seekfreq -eq 1 ] && sel="$sel seekfreq"
		[ -n "$sel" ] || sel="srmin snr seekfreq"
		for p in $sel; do
			echo "== $p"
			$PY "$dir/patch-dvb-$p.py" "$ko" --check || rc=1
		done
		continue ;;
	restore)
		bak=
		for b in "$ko.gbbs-orig" "$ko.gb7252-pnp.orig"; do
			[ -f "$b" ] && { bak=$b; break; }
		done
		if [ -n "$bak" ]; then
			cp -p "$bak" "$ko" && echo "restored $ko"
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
		if [ ! -f "$ko.gbbs-orig" ] && [ ! -f "$ko.gb7252-pnp.orig" ]; then
			if $PY "$dir/patch-dvb-snr.py" "$ko" --check >/dev/null 2>&1 || \
			   $PY "$dir/patch-dvb-seekfreq.py" "$ko" --check >/dev/null 2>&1 || \
			   $PY "$dir/patch-dvb-srmin.py" "$ko" --check >/dev/null 2>&1; then
				echo "dvb-patch.sh: $ko already carries a patch; no vendor copy kept" >&2
			else
				cp -p "$ko" "$ko.gbbs-orig"
			fi
		fi
		mv -f "$tmp" "$ko" && echo "patched $ko"
	else
		rm -f "$tmp"
		echo "dvb-patch.sh: $ko: not recognised or patch failed - left unchanged" >&2
		rc=1
	fi
done
[ $nfiles -gt 0 ] || { echo "dvb-patch.sh: no dvb.ko given" >&2; exit 1; }
exit $rc
