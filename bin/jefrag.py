#!/usr/bin/env python3
"""Fragmentation report from a jemalloc heap dump.

Consumes a heap profile produced by prof.dump (heap_v2 format) that contains
the fragmentation records emitted by jemalloc:

  f: <age_ns> <request_size> <usize> <szind> <arena_ind> <thr_uid>
      -- one per live sampled allocation, listed under its "@ <pc>..." stack
  frag_util: <arena_ind> <binind> <reg_size> <slab_size> <nregs> <n_shards>
      <curslabs> <curregs> <nonfull_slabs>
      -- one per (arena, small size class)

Ground-truth slab waste per (arena, size class) comes from frag_util; the
sampled records statistically attribute that waste to the call stacks that
keep old allocations alive in the class.  Sampling is byte-weighted, so each
record is unbiased into an estimated object count with the standard Poisson
factor 1 / (1 - exp(-usize / sample_interval)).

Note: curregs counts regions cached in tcaches as allocated, so waste is a
lower bound estimate ("waste" regions may partially be tcache-cached).

Outputs:
  --report (default)  top-pinner table per fragmented (arena, size class)
  --collapsed         folded stacks weighted by attributed waste bytes, for
                      flamegraph.pl / speedscope
"""

import argparse
import bisect
import math
import re
import shutil
import subprocess
import sys
from collections import defaultdict

PAGE_HINT = 4096


def parse_dump(f):
    sample_interval = None
    stacks = {}  # stack (tuple of pc ints) -> list of records
    utils = {}  # (arena, binind) -> dict
    maps = []  # raw MAPPED_LIBRARIES lines
    cur_stack = None
    in_maps = False

    for line in f:
        line = line.rstrip("\n")
        if in_maps:
            maps.append(line)
            continue
        if line.startswith("MAPPED_LIBRARIES:"):
            in_maps = True
            continue
        stripped = line.strip()
        if sample_interval is None and stripped.startswith("heap_v2/"):
            sample_interval = int(stripped.split("/", 1)[1].split()[0])
            continue
        if stripped.startswith("@ "):
            cur_stack = tuple(int(pc, 16) for pc in stripped[2:].split())
            stacks.setdefault(cur_stack, [])
            continue
        if stripped.startswith("f: "):
            fields = stripped[3:].split()
            if len(fields) != 6 or cur_stack is None:
                continue
            rec = {
                "age_ns": int(fields[0]),
                "size": int(fields[1]),
                "usize": int(fields[2]),
                "szind": int(fields[3]),
                "arena": int(fields[4]),
                "thr_uid": int(fields[5]),
            }
            stacks[cur_stack].append(rec)
            continue
        if stripped.startswith("frag_util: "):
            fields = stripped[len("frag_util: "):].split()
            if len(fields) != 9:
                continue
            (arena, binind, reg_size, slab_size, nregs, n_shards, curslabs,
                curregs, nonfull_slabs) = (int(x) for x in fields)
            utils[(arena, binind)] = {
                "reg_size": reg_size,
                "slab_size": slab_size,
                "nregs": nregs,
                "n_shards": n_shards,
                "curslabs": curslabs,
                "curregs": curregs,
                "nonfull_slabs": nonfull_slabs,
                "waste": curslabs * slab_size - curregs * reg_size,
            }
            continue

    if sample_interval is None:
        sys.exit("error: input is not a heap_v2 jemalloc profile")
    return sample_interval, stacks, utils, maps


def unbias(usize, sample_interval):
    """Estimated number of allocations one sampled allocation stands for."""
    ratio = usize / sample_interval
    if ratio > 30:
        return 1.0
    return 1.0 / (1.0 - math.exp(-ratio))


class Symbolizer:
    def __init__(self, maps_lines, enabled):
        self.enabled = enabled
        self.cache = {}
        # Executable mappings: (start, end, base, path).
        self.mappings = []
        if not enabled:
            return
        for line in maps_lines:
            # <start>-<end> <perms> <offset> <dev> <inode> <path>
            parts = line.split()
            if len(parts) < 6 or "x" not in parts[1]:
                continue
            path = parts[5]
            if not path.startswith("/"):
                continue
            start, end = (int(x, 16) for x in parts[0].split("-"))
            offset = int(parts[2], 16)
            self.mappings.append((start, end, start - offset, path))
        self.mappings.sort()
        self.starts = [m[0] for m in self.mappings]
        self.tool = shutil.which("llvm-symbolizer") or shutil.which(
            "addr2line")
        if self.tool is None:
            self.enabled = False

    def _lookup_mapping(self, pc):
        i = bisect.bisect_right(self.starts, pc) - 1
        if i >= 0:
            start, end, base, path = self.mappings[i]
            if pc < end:
                return base, path
        return None, None

    def resolve_many(self, pcs):
        todo = defaultdict(list)  # path -> [(pc, file-relative addr)]
        for pc in pcs:
            if pc in self.cache:
                continue
            self.cache[pc] = "0x%x" % pc
            if not self.enabled:
                continue
            base, path = self._lookup_mapping(pc)
            if path is not None:
                # The return address points past the call site.
                todo[path].append((pc, pc - base - 1))
        for path, addrs in todo.items():
            self._symbolize_file(path, addrs)

    def _symbolize_file(self, path, addrs):
        if "llvm-symbolizer" in self.tool:
            cmd = [self.tool, "--obj=" + path, "--functions=linkage",
                "--no-inlines"]
        else:
            cmd = [self.tool, "-f", "-e", path]
        try:
            out = subprocess.run(cmd,
                input="".join("0x%x\n" % a for _, a in addrs),
                capture_output=True, text=True, timeout=60).stdout
        except Exception:
            return
        # Both tools print the function name as the first of (up to) two
        # lines per address; llvm-symbolizer separates entries with a blank
        # line.
        names = []
        expect_name = True
        for line in out.splitlines():
            if not line.strip():
                expect_name = True
                continue
            if expect_name:
                names.append(line.strip())
                expect_name = False
        for (pc, _), name in zip(addrs, names):
            if name and name not in ("??", "<invalid>"):
                self.cache[pc] = name

    def name(self, pc):
        return self.cache.get(pc, "0x%x" % pc)


def attribute(sample_interval, stacks, utils, min_age_ns):
    """Return ({(arena, szind): [(stack, est_objs)]}, {(arena, szind): total})."""
    per_class = defaultdict(lambda: defaultdict(float))
    for stack, recs in stacks.items():
        for rec in recs:
            if rec["age_ns"] < min_age_ns:
                continue
            key = (rec["arena"], rec["szind"])
            if key not in utils:
                continue  # Large size class: no slab waste to attribute.
            per_class[key][stack] += unbias(rec["usize"], sample_interval)
    totals = {
        key: sum(shares.values()) for key, shares in per_class.items()
    }
    return per_class, totals


def fmt_bytes(n):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if abs(n) < 4096 or unit == "GiB":
            return "%.1f %s" % (n, unit) if unit != "B" else "%d B" % n
        n /= 1024.0
    return "%d" % n


# Leading allocator-internal frames to strip (same idea as jeprof's
# --ignore defaults for jemalloc).
_INTERNAL_FRAME = re.compile(
    r"^(je_|jet_|_?prof_|imalloc|malloc_default"
    r"|(m|c|re|d|sd)allocx?$|free$|posix_memalign$|aligned_alloc$"
    r"|operator new)")


def user_frames(stack, sym, depth):
    frames = [sym.name(pc) for pc in stack[:depth]]
    while len(frames) > 1 and _INTERNAL_FRAME.match(frames[0]):
        frames.pop(0)
    return frames


def fmt_stack(stack, sym, depth):
    return ";".join(user_frames(stack, sym, depth))


def report(args, sample_interval, stacks, utils, sym):
    min_age_ns = int(args.min_age * 1e9)
    per_class, totals = attribute(sample_interval, stacks, utils, min_age_ns)

    classes = sorted(
        utils.items(), key=lambda kv: kv[1]["waste"], reverse=True)
    total_waste = sum(u["waste"] for _, u in utils.items())
    attributed_waste = 0.0

    print("sample_interval: %d bytes" % sample_interval)
    print("min_age: %g s" % args.min_age)
    print("total slab waste: %s (upper bound; tcache-cached regions count"
        " as allocated)" % fmt_bytes(total_waste))
    print()

    shown = 0
    for (arena, binind), u in classes:
        if u["waste"] < args.min_waste:
            continue
        if args.top_classes and shown >= args.top_classes:
            break
        shown += 1
        availregs = u["curslabs"] * u["nregs"]
        util_pct = 100.0 * u["curregs"] / availregs if availregs else 100.0
        print("arena %d size-class %d (reg_size %d): waste %s, util %.1f%%,"
            " curslabs %d, curregs %d" % (arena, binind,
                u["reg_size"], fmt_bytes(u["waste"]), util_pct, u["curslabs"],
                u["curregs"]))
        key = (arena, binind)
        shares = per_class.get(key)
        if not shares:
            print("  (no sampled allocation of this class is older than"
                " min_age: waste unattributed; consider lowering"
                " lg_prof_sample or min_age)")
            print()
            continue
        total = totals[key]
        ranked = sorted(shares.items(), key=lambda kv: kv[1], reverse=True)
        for stack, est in ranked[: args.top_stacks]:
            share = est / total
            attributed_waste += u["waste"] * share
            print("  %5.1f%% (~%s, est %.0f old objects) %s"
                % (100.0 * share, fmt_bytes(u["waste"] * share), est,
                    fmt_stack(stack, sym, args.depth)))
        print()

    print("attributed waste: %s of %s"
        % (fmt_bytes(attributed_waste), fmt_bytes(total_waste)))


def collapsed(args, sample_interval, stacks, utils, sym):
    min_age_ns = int(args.min_age * 1e9)
    per_class, totals = attribute(sample_interval, stacks, utils, min_age_ns)

    weights = defaultdict(float)  # stack -> attributed waste bytes
    for key, shares in per_class.items():
        waste = utils[key]["waste"]
        total = totals[key]
        if total <= 0:
            continue
        for stack, est in shares.items():
            weights[stack] += waste * est / total

    for stack, weight in sorted(weights.items(), key=lambda kv: -kv[1]):
        if weight < 1:
            continue
        # Flamegraph convention: root first.
        frames = list(reversed(user_frames(stack, sym, args.depth)))
        print("%s %d" % (";".join(frames), round(weight)))


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dump", help="heap dump file (- for stdin)")
    parser.add_argument("--collapsed", action="store_true",
        help="emit folded stacks for flamegraphs instead of the report")
    parser.add_argument("--min-age", type=float, default=60.0,
        help="only allocations older than this many seconds pin slabs"
        " (default: 60)")
    parser.add_argument("--min-waste", type=int, default=PAGE_HINT,
        help="hide size classes wasting less than this many bytes")
    parser.add_argument("--top-stacks", type=int, default=5,
        help="stacks shown per size class in the report (default: 5)")
    parser.add_argument("--top-classes", type=int, default=0,
        help="max size classes shown, 0 = all")
    parser.add_argument("--depth", type=int, default=16,
        help="max stack frames used (default: 16)")
    parser.add_argument("--no-symbolize", action="store_true",
        help="print raw addresses")
    args = parser.parse_args()

    f = sys.stdin if args.dump == "-" else open(args.dump)
    with f:
        sample_interval, stacks, utils, maps = parse_dump(f)

    sym = Symbolizer(maps, not args.no_symbolize)
    pcs = set()
    for stack, recs in stacks.items():
        if recs:
            pcs.update(stack[: args.depth])
    sym.resolve_many(pcs)

    if args.collapsed:
        collapsed(args, sample_interval, stacks, utils, sym)
    else:
        report(args, sample_interval, stacks, utils, sym)


if __name__ == "__main__":
    main()
