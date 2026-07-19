#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
#
# Convenience wrapper to build U-Boot for the coreboot64 target in one of
# two flavours:
#
#   release  speed-optimised, silent boot  (coreboot64-fast_defconfig)
#            -> build/release/
#   debug    size-optimised, full console + logging (coreboot64_defconfig)
#            -> build/debug/
#
# With no flavour (or "both") it builds release and debug back to back.
#
# Usage:
#   ./mkuboot.sh                    # build both flavours (default)
#   ./mkuboot.sh both               # same as above, explicit
#   ./mkuboot.sh release            # configure + build the release image
#   ./mkuboot.sh debug              # configure + build the debug image
#   ./mkuboot.sh both clean         # remove both build directories
#   ./mkuboot.sh release -- <args>  # pass extra args straight to make
#
# Any make target/flags after "--" are forwarded verbatim, e.g.
#   ./mkuboot.sh debug -- menuconfig

set -eu

usage() {
	cat >&2 <<EOF
Usage: $0 [release|debug|both] [clean] [-- <extra make args>]

  (no arg)  build both flavours (default)
  release   coreboot64-fast_defconfig  (-O2, silent) -> build/release/
  debug     coreboot64_defconfig       (-Os, verbose) -> build/debug/
  both      build release and debug
  clean     remove the selected build directory/directories and exit
EOF
	exit 1
}

# Resolve a flavour name to its defconfig + output directory.
# Sets the globals $defconfig and $outdir.
resolve() {
	case "$1" in
	release)
		defconfig=coreboot64-fast_defconfig
		outdir=build/release
		;;
	debug)
		defconfig=coreboot64_defconfig
		outdir=build/debug
		;;
	*)
		echo "$0: unknown flavour '$1'" >&2
		usage
		;;
	esac
}

# build_one <flavour> : configure (if needed) and build a single flavour,
# honouring the global $do_clean, $extra_args and $jobs.
build_one() {
	resolve "$1"

	if [ "$do_clean" = 1 ]; then
		echo ">> removing $outdir"
		rm -rf "$outdir"
		return 0
	fi

	echo ">> flavour=$1 defconfig=$defconfig outdir=$outdir jobs=$jobs"

	# (Re)generate the config only when missing, so incremental builds
	# stay fast; use "clean" to force a fresh configure.
	if [ ! -f "$outdir/.config" ]; then
		echo ">> configuring $defconfig"
		make O="$outdir" "$defconfig"
	fi

	echo ">> building $1"
	# shellcheck disable=SC2086 # word-splitting of extra_args is intentional
	make O="$outdir" -j"$jobs" $extra_args

	echo ">> done: $outdir/u-boot.bin"
	ls -l "$outdir/u-boot.bin" 2>/dev/null || true
}

# --- argument parsing -------------------------------------------------

case "${1:-}" in
-h | --help)
	usage
	;;
release | debug | both)
	flavour=$1
	shift
	;;
"" | --)
	flavour=both
	;;
clean)
	flavour=both # "clean" with no flavour cleans everything
	;;
*)
	echo "$0: unknown flavour '$1'" >&2
	usage
	;;
esac

do_clean=0
if [ "${1:-}" = "clean" ]; then
	do_clean=1
	shift
fi

extra_args=""
if [ "${1:-}" = "--" ]; then
	shift
	extra_args="$*"
	if [ "$flavour" = both ]; then
		echo "$0: refusing to forward extra make args to both flavours;" >&2
		echo "     pick 'release' or 'debug' when using '-- <args>'." >&2
		exit 1
	fi
fi

jobs=$(nproc 2>/dev/null || echo 4)

# --- run --------------------------------------------------------------

if [ "$flavour" = both ]; then
	build_one release
	build_one debug
else
	build_one "$flavour"
fi
