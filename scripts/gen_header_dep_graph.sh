#!/bin/bash
# Generate header dependency graph + tsort cycle check.
#
# Outputs (under build/):
#   header_deps_baseline.txt   - sorted, unique header->header (and .c->header)
#                                edges derived from #include "..." directives.
#   header_tsort_order.txt     - topological order produced by tsort.
#   header_tsort_cycles.txt    - stderr from tsort; non-empty if there's a cycle.
#   per_tu_deps.txt            - per-translation-unit transitive header lists
#                                (one TU per line; basenames only; sorted).
#
# All headers / sources are reduced to basenames so renames in the cleanup are
# easy to spot in the diff.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"
mkdir -p build

edges_raw=build/header_edges_raw.txt
edges_baseline=build/header_deps_baseline.txt
tsort_order=build/header_tsort_order.txt
tsort_cycles=build/header_tsort_cycles.txt
per_tu=build/per_tu_deps.txt

: > "$edges_raw"

extract_edges() {
    local src="$1"
    local bn
    bn=$(basename "$src")
    # Match `#include "..."` (skip `<...>` system includes). Strip path, keep
    # only the basename of the included file, so the graph collapses
    # `../jemalloc.h` and similar relative paths into a single node.
    awk '
        /^[[:space:]]*#[[:space:]]*include[[:space:]]+"/ {
            match($0, /"[^"]+"/)
            inc = substr($0, RSTART + 1, RLENGTH - 2)
            n = split(inc, parts, "/")
            print parts[n]
        }
    ' "$src" | while read -r dep; do
        printf '%s %s\n' "$bn" "$dep"
    done
}

# Headers (include/jemalloc/internal/) — these are the nodes we care about for
# cycle detection.
for f in include/jemalloc/internal/*.h include/jemalloc/*.h; do
    extract_edges "$f" >> "$edges_raw"
done

# Translation units (src/*.c) — included so that .c files appear as graph
# sources in the baseline. They can't introduce cycles (nothing #includes a
# .c), but the edges are useful when diffing later.
for f in src/*.c; do
    extract_edges "$f" >> "$edges_raw"
done

sort -u "$edges_raw" > "$edges_baseline"

# tsort consumes "A B" pairs and reports cycles on stderr.
: > "$tsort_cycles"
if ! tsort "$edges_baseline" > "$tsort_order" 2> "$tsort_cycles"; then
    echo "tsort exited non-zero; see $tsort_cycles" >&2
fi

# Per-translation-unit transitive header lists, harvested from the .d files
# the build already produced (CC_MM=1 in the Makefile).
: > "$per_tu"
for d in src/*.d; do
    [ -f "$d" ] || continue
    tu=$(basename "${d%.d}.c")
    # Strip the make rule prefix ("foo.o: foo.c \"), drop line continuations,
    # collapse to basenames, sort+uniq.
    deps=$(
        tr '\n' ' ' < "$d" \
            | sed -E 's/\\//g' \
            | tr ' ' '\n' \
            | grep -E '\.h$' \
            | awk -F/ '{print $NF}' \
            | sort -u \
            | tr '\n' ' ' \
            | sed -E 's/[[:space:]]+$//'
    )
    printf '%s: %s\n' "$tu" "$deps" >> "$per_tu"
done
sort -o "$per_tu" "$per_tu"

edge_count=$(wc -l < "$edges_baseline" | tr -d ' ')
cycle_bytes=$(wc -c < "$tsort_cycles" | tr -d ' ')
tu_count=$(wc -l < "$per_tu" | tr -d ' ')
echo "edges: $edge_count"
echo "translation units: $tu_count"
echo "tsort cycle output bytes: $cycle_bytes"
if [ "$cycle_bytes" -gt 0 ]; then
    echo "---- tsort cycle report ----"
    cat "$tsort_cycles"
fi
