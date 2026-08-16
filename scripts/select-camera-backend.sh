#!/usr/bin/env bash
# Resolve this project's 4 camera capture nodes -- real (TI's
# j721e-csi2rx, fed by mycam004m.ko once its register tables are
# filled in -- see mycam004m-regs.h) or fake (mycam004m-fake.ko's
# static reference images, see tools/gen_fake_frames.py) -- to a fixed
# set of symlinks under /dev/mycam/, so a calling application always
# opens the same paths regardless of which backend is active.
#
# Matching uses only /sys/class/video4linux/videoX/name (no v4l2-ctl
# dependency -- the BeaglePlay's minimal rootfs doesn't have v4l-utils
# installed, confirmed by actually running this on real hardware).
# Both backends name their nodes "... context N" (real: see
# ti_csi2rx_video_register() in j721e-csi2rx.c; fake: see
# mycam004m_fake_cam_init() in mycam004m-fake.c, which deliberately
# mirrors that suffix). The fake backend's names additionally start
# with "mycam004m-fake ", which is what distinguishes the two --
# confirmed against a live mycam004m-fake.ko load:
#   /sys/class/video4linux/video0/name -> "mycam004m-fake context 0"
# N is 0-indexed in both; camN symlinks are 1-indexed (N+1).
#
# This script only resolves and symlinks -- it does not load/unload
# kernel modules or apply devicetree overlays. Do that first (see
# README.md for the real backend, or "insmod mycam004m-fake.ko" with
# firmware findable by the firmware loader for the fake one).
set -euo pipefail

LINKDIR=/dev/mycam

usage() {
	echo "usage: $0 real|fake" >&2
	exit 1
}

[ $# -eq 1 ] || usage
case "$1" in
	real) want_fake=false ;;
	fake) want_fake=true ;;
	*) usage ;;
esac

mkdir -p "$LINKDIR"
rm -f "$LINKDIR"/cam[1-4]

found=0
for name_file in /sys/class/video4linux/video*/name; do
	[ -e "$name_file" ] || continue
	name=$(cat "$name_file")

	[[ "$name" =~ \ context\ ([0-9]+)$ ]] || continue
	idx="${BASH_REMATCH[1]}"

	is_fake=false
	[[ "$name" == "mycam004m-fake "* ]] && is_fake=true
	[ "$is_fake" = "$want_fake" ] || continue

	cam_n=$((idx + 1))
	dev="/dev/$(basename "$(dirname "$name_file")")"
	ln -sf "$dev" "$LINKDIR/cam${cam_n}"
	echo "cam${cam_n} -> ${dev}  (${name})"
	found=$((found + 1))
done

if [ "$found" -ne 4 ]; then
	echo "warning: expected 4 '$1' nodes named '... context N', found $found" >&2
	case "$1" in
		real) echo "  is mycam004m.ko loaded and the devicetree overlay applied?" >&2 ;;
		fake) echo "  is mycam004m-fake.ko loaded, with firmware findable by the firmware loader?" >&2 ;;
	esac
	exit 1
fi

echo "$LINKDIR/cam1 .. cam4 now point at the '$1' backend."
