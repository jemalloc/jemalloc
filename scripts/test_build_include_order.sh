#!/bin/sh

# Regression test for https://github.com/jemalloc/jemalloc/issues/2619.
#
# Package-owned headers must take precedence over headers from an older
# jemalloc installation, even when that installation is present in one of the
# user-controlled compiler or preprocessor flag variables.

set -eu

srcroot=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if ! test -x "$srcroot/configure"; then
	printf '%s\n' 'configure is missing; run autoconf first' >&2
	exit 1
fi

test_root=$(mktemp -d "${TMPDIR:-/tmp}/jemalloc-include-order.XXXXXX")
trap 'rm -rf "$test_root"' EXIT HUP INT TERM

stale_prefix="$test_root/stale"
mkdir -p "$stale_prefix/include/jemalloc/internal"
printf '%s\n' '#error STALE_JEMALLOC_HEADER_SELECTED' \
    > "$stale_prefix/include/jemalloc/internal/jemalloc_preamble.h"

failures=0

run_case() {
	flag_name=$1
	target=$2
	build_dir="$test_root/build-$flag_name"
	configure_log="$test_root/configure-$flag_name.log"
	build_log="$test_root/build-$flag_name.log"
	mkdir -p "$build_dir"

	printf '== %s: %s ==\n' "$flag_name" "$target"
	if ! (cd "$build_dir" && env \
	    CFLAGS= CXXFLAGS= CPPFLAGS= \
	    "$flag_name=-I$stale_prefix/include" \
	    "$srcroot/configure" --disable-shared --disable-doc) \
	    > "$configure_log" 2>&1; then
		printf 'configure failed; output follows:\n'
		cat "$configure_log"
		failures=1
		return
	fi

	if (cd "$build_dir" && make -j1 "$target") \
	    > "$build_log" 2>&1; then
		printf 'PASS: %s with %s\n' "$target" "$flag_name"
	else
		printf 'FAIL: %s with %s\n' "$target" "$flag_name"
		awk '
		    /^(gcc|g\+\+) / {
		        compiler_command = $0
		    }
		    /error: #error STALE_JEMALLOC_HEADER_SELECTED/ {
		        print compiler_command
		        print
		        found = 1
		        exit
		    }
		    END {
		        if (!found) {
		            exit 1
		        }
		    }
		' "$build_log" || cat "$build_log"
		failures=1
	fi
}

run_case CFLAGS src/jemalloc.sym.o
run_case CXXFLAGS src/jemalloc_cpp.o
run_case CPPFLAGS src/jemalloc.sym.o

if test "$failures" -ne 0; then
	printf '%s\n' 'FAIL: an installed jemalloc header shadowed an in-tree header'
	exit 1
fi

printf '%s\n' 'PASS: in-tree jemalloc headers take precedence'
