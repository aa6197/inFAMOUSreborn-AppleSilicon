#!/usr/bin/env python3
"""Check the save-state phase 0 probe log (runtime/ppu/ppu_ss_probe.cpp).

usage: ss_probe_check.py <stderr.log> <game binary> [--examples N]

For every thread of every probe it compares the host call chain (native
frames of lifted functions, innermost first) with the guest chain (LR words
found by walking the guest back chain). A host frame "matches" when a guest
return address, in order, points just after a call instruction inside that
function. The call site must be exact for a resume: a host link through
ps3_indirect_call (IC) needs a guest bctrl there, a direct link a bl to the
callee.

Thread verdicts:
  ok        every host frame matched, call kinds right
  stale     an indirect link has a wrong or no guest return address: bctrl
            does not set the guest LR, so the callee saved an older one (0 at
            a thread entry). Fixed if bctrl set the guest LR.
  missing   a host frame reached by a direct call has no guest return address
  extra     the guest chain has frames between matches that the host has not
            (not counted right after a stale indirect link)
  nolifted  no lifted frame on the host stack
"""
import bisect
import re
import statistics
import subprocess
import sys
from collections import Counter

LOOSE = 0x40000   # largest distance from a function start to one of its call sites


def load_syms(binary):
    out = subprocess.run(["nm", binary], capture_output=True, text=True, check=True).stdout
    addrs = set()
    for line in out.splitlines():
        if "spu_func_" in line:
            continue
        m = re.search(r"func_([0-9A-F]{8})P11ppu_context$", line)
        if m:
            addrs.add(int(m.group(1), 16))
    return sorted(addrs)


def containing(syms, a):
    i = bisect.bisect_right(syms, a) - 1
    return syms[i] if i >= 0 else None


def parse(path):
    probes, mems = [], []
    cur = None
    thr = None
    pr = re.compile(r"\[ss\] probe (\d+) at ([\d.]+) s: (\d+) threads, (\d+) safe, (all safe|NOT all safe) after ([\d.]+) ms; "
                    r"suspended ([\d.]+), read ([\d.]+), released ([\d.]+) ms")
    tr = re.compile(r"\[ss\] t=(\d+) st=(\S+) num=(\S+) sc=(\d+) hle=(\S+) dsc=(-?\d+) dhle=(-?\d+) rs=(-?\d+) susp=(\d) "
                    r"sp=(\S+) lr=(\S+) gend=(-?\d+)")
    with open(path, errors="replace") as f:
        for line in f:
            if not line.startswith("[ss]"):
                continue
            m = pr.match(line)
            if m:
                cur = dict(idx=int(m.group(1)), t=float(m.group(2)), n=int(m.group(3)), safe=int(m.group(4)),
                           all_safe=m.group(5) == "all safe", stop_ms=float(m.group(6)), susp_ms=float(m.group(7)),
                           read_ms=float(m.group(8)), rel_ms=float(m.group(9)), threads=[])
                probes.append(cur)
                continue
            m = tr.match(line)
            if m and cur is not None:
                thr = dict(t=int(m.group(1)), st=m.group(2), sc=int(m.group(4)), hle=m.group(5), rs=int(m.group(8)),
                           susp=m.group(9) == "1", sp=int(m.group(10), 16), gend=int(m.group(12)), h=[], g=[])
                cur["threads"].append(thr)
                continue
            if line.startswith("[ss]  h") and thr is not None:
                thr["h"] = line.split()[2:]
                continue
            if line.startswith("[ss]  g") and thr is not None:
                g = []
                for tok in line.split()[2:]:
                    p = tok.split(":")
                    g.append((int(p[0], 16), p[1], int(p[2], 16) if len(p) > 2 else None))
                thr["g"] = g
                continue
            if line.startswith("[ss] mem"):
                mems.append(line.strip()[5:])
    return probes, mems


def host_frames(tokens):
    """[(func, markers between this frame and the inner one)] innermost first."""
    frames, marks = [], []
    for tok in tokens:
        if tok.startswith("F:"):
            fn = int(tok[2:], 16)
            if frames and frames[-1][0] == fn and not marks:
                continue
            frames.append((fn, tuple(marks)))
            marks = []
        else:
            marks.append(tok)
    return frames


def check_thread(th, syms):
    fr = host_frames(th["h"])
    if not fr:
        return "nolifted", {}
    g = th["g"]
    j = 0
    info = Counter()
    # Guest entries inside the innermost function are its own (an older bl).
    inner = fr[0][0]
    while j < len(g) and 0 <= g[j][0] - 4 - inner < LOOSE and containing(syms, g[j][0] - 4) is not None \
            and containing(syms, g[j][0] - 4) >= inner and not any(
                0 <= g[j][0] - 4 - f < LOOSE and f > inner for f, _ in fr[1:2]):
        j += 1
    verdict = "ok"
    ic_slack = 0   # frames after a stale indirect link where its older LR may show up
    for k in range(1, len(fr)):
        fn, marks = fr[k]
        callee = fr[k - 1][0]
        nested = "HLE" in marks
        if nested:
            info["nested"] += 1
        found = None
        for jj in range(j, len(g)):
            ra = g[jj][0]
            if 0 <= ra - 4 - fn < LOOSE:
                c = containing(syms, ra - 4)
                if c is not None and c >= fn:
                    found = jj
                    break
        if found is None:
            if "IC" in marks:
                # fn called its callee through bctrl, which leaves the guest LR
                # as it was: the callee saved an older LR (0 at a thread entry).
                info["ic-missing"] += 1
                ic_slack = 2
                if verdict == "ok":
                    verdict = "stale"
            else:
                info["missing"] += 1
                verdict = "missing"
            continue
        if found > j:
            if ic_slack > 0:
                # the older LR saved after an IC link shows up as one more frame
                info["ic-extra"] += found - j
            else:
                info["extra"] += found - j
                if verdict in ("ok", "stale"):
                    verdict = "extra"
        ic_slack = max(0, ic_slack - 1)
        ra, kind, tgt = g[found]
        info["exact" if containing(syms, ra - 4) == fn else "in-fragment"] += 1
        if "IC" in marks:
            if kind == "c":
                info["ic-ok"] += 1
            else:
                info["ic-stale"] += 1
                if verdict == "ok":
                    verdict = "stale"
        else:
            if kind == "b" and tgt == callee:
                info["bl-ok"] += 1
            elif kind == "b":
                info["bl-other-target"] += 1
            else:
                info["direct-not-bl:" + kind] += 1
        j = found + 1
    return verdict, info


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    ex_n = int(sys.argv[sys.argv.index("--examples") + 1]) if "--examples" in sys.argv else 3
    syms = load_syms(sys.argv[2])
    probes, mems = parse(sys.argv[1])
    if not probes:
        print("no probes in log")
        sys.exit(1)
    stop = [p["stop_ms"] for p in probes]
    read = [p["read_ms"] for p in probes]
    print(f"probes: {len(probes)} ({probes[0]['t']:.0f}-{probes[-1]['t']:.0f} s), all threads safe in "
          f"{sum(p['all_safe'] for p in probes)}")
    q = statistics.quantiles(stop, n=10) if len(stop) > 1 else stop * 9
    print(f"stop time ms: median {statistics.median(stop):.1f}, p90 {q[-1]:.1f}, max {max(stop):.1f}; "
          f"snapshot (suspend+read) median {statistics.median(read):.2f} ms")
    st = Counter()
    unsafe = Counter()
    verdicts = Counter()
    infos = Counter()
    good_probes = good_probes_fix = 0
    examples = {}
    for p in probes:
        pv = Counter()
        for th in p["threads"]:
            st[th["st"]] += 1
            if th["st"] == "guest":
                unsafe[th["t"]] += 1
            v, info = check_thread(th, syms)
            verdicts[v] += 1
            pv[v] += 1
            infos.update(info)
            if v not in ("ok", "nolifted") and len(examples.setdefault(v, [])) < ex_n:
                examples[v].append((p["idx"], th))
        if p["all_safe"] and pv["missing"] == pv["extra"] == pv["stale"] == 0:
            good_probes += 1
        if p["all_safe"] and pv["missing"] == pv["extra"] == 0:
            good_probes_fix += 1
    print("thread states: " + ", ".join(f"{k} {v}" for k, v in st.most_common()))
    if unsafe:
        print("threads not at a safe point (by id): " + ", ".join(f"t{k} x{v}" for k, v in unsafe.most_common()))
    print("chain verdicts: " + ", ".join(f"{k} {v}" for k, v in verdicts.most_common()))
    print("frame links: " + ", ".join(f"{k} {v}" for k, v in sorted(infos.items())))
    print(f"probes where a save would work now: {good_probes}/{len(probes)}; "
          f"with bctrl setting the guest LR: {good_probes_fix}/{len(probes)}")
    for m in mems:
        print(m)
    for v, lst in examples.items():
        for idx, th in lst:
            print(f"\nexample {v}: probe {idx} t={th['t']} st={th['st']} gend={th['gend']}")
            print("  h: " + " ".join(th["h"][:40]))
            print("  g: " + " ".join(f"{a:08X}:{k}" + (f":{t:08X}" if t is not None else "") for a, k, t in th["g"][:40]))


if __name__ == "__main__":
    main()
