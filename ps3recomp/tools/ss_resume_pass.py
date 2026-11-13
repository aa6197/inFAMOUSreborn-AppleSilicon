#!/usr/bin/env python3
"""Save states phase 1: resume points in the lifted PPU code (build-time pass, opt-in).

usage: ss_resume_pass.py <in.cpp> <out.cpp> --raw=<recompiled/ppu_recomp_N.cpp> --elf=<EBOOT.elf>
                         --hdr=<runtime/ppu/ppu_ss_resume.h> [--tu=N] [--stats]

Runs last over each lifted PPU TU that gets compiled (after ppu_regcache.py and ppu_memacc_v243.py),
only with CMake PORT_SAVE_STATES=ON (Apple, default OFF). With it off nothing changes.
<in.cpp> may be the register-cached copy or the plain lifted TU; --raw is always the plain one
(recompiled/ppu_recomp_N.cpp) and is only read, for stack offsets.

What it adds (runtime side: runtime/ppu/ppu_ss_resume.h):
  * bctrl / blrl set the guest LR to their real return address (ctx->lr = RA), as the hardware
    does, so the guest stack holds a return address for every frame. Phase 0 (11-13): with that,
    the chain of every thread was complete in 100/100 checks. The addresses come from the ELF:
    between two known addresses in a function (a label or a bl return address) the n-th bctrl in
    the text is the n-th link-indirect branch in the ELF there.
  * each sc sets ctx->cia to its own address, so a save knows which sc a thread is in.
  * a label after every call (before its trampoline drain) and before and after every sc and HLE
    call.
  * a resume block at each function start (after the _cs_N snapshots): while a load rebuilds a
    thread (g_ss_resume), it refills the _cs_N from the guest stack, calls the next function of
    the saved chain and jumps to the label after that call. The innermost function jumps into
    (or past) its sc / HLE call.
  * a site table (function, address, kind, r1 offset) registered by a static constructor, so a
    save can check every frame of a chain exactly.
A function the pass cannot follow keeps its code, still gets a resume block (that stops the
load), and is marked not resumable in the table, so a save that needs it is refused.
"""
import bisect
import re
import struct
import sys
from collections import Counter

K_NONE, K_CALL, K_SC_IN, K_SC_OUT, K_HLE_IN, K_HLE_OUT, K_TAIL = range(7)
DELTA_DYN = 0x7FFFFFFF

FN_START = re.compile(r'^void (func_([0-9A-Fa-f]{8})(\w*))\(ppu_context\* ctx\) \{\s*(?:/\*[^*]*\*/\s*)?$')  # e.g. '{   /* v245 ... */'
TOK = re.compile(
    r'(?P<lab>^\s*loc_(?P<la>[0-9A-Fa-f]{8}):)'
    r'|ctx->lr = 0x(?P<lr>[0-9A-Fa-f]+);'
    r'|(?P<fc>\bfunc_[0-9A-Fa-f]{8}\w*\(ctx\);)'
    r'|(?P<ic>\bps3_indirect_call\(ctx\);)(?P<icd>\s*DRAIN_TRAMPOLINE\(ctx\);)?'
    r'|(?P<sc>\blv2_syscall\(ctx\);)'
    r'|(?P<hle>\bps3_hle_call\(0x[0-9A-Fa-f]+u?, ctx\);)'
    r'|ctx->gpr\[1\] \+= -0x(?P<stdu>[0-9A-Fa-f]+);'
    r'|ctx->gpr\[1\] = ctx->gpr\[1\] \+ \(int64_t\)\((?P<addi>-?(?:0x)?[0-9A-Fa-f]+)\);'
    r'|ctx->gpr\[1\] = (?P<r1other>[^;]*);')
CS_DECL = re.compile(r'uint64_t _cs_(\d+) = ')
RAW_CS_REG = re.compile(r'uint64_t _cs_(\d+) = ctx->gpr\[(\d+)\];')
RAW_CS_MEM = re.compile(r'uint64_t _cs_(\d+) = vm_read64\(ctx->gpr\[1\] \+ (-?(?:0x)?[0-9A-Fa-f]+)\);')
DECL_LINE = re.compile(r'^    (?:uint64_t|uint32_t|double|float|uint8_t\*|v128|__m128\w*|int64_t) ')


def parse_elf(path):
    """Link-indirect branches (bcctrl/bclrl with LK) and sc instructions, sorted."""
    elf = open(path, 'rb').read()
    if elf[:4] != b'\x7fELF' or elf[4] != 2 or elf[5] != 2:
        raise SystemExit('ss_resume_pass: %s is not a big-endian ELF64' % path)
    phoff = struct.unpack('>Q', elf[0x20:0x28])[0]
    phes, phnum = struct.unpack('>HH', elf[0x36:0x3a])
    ic, sc = [], []
    for i in range(phnum):
        typ, flags, off, va, _pa, fsz, _msz, _al = struct.unpack(
            '>IIQQQQQQ', elf[phoff + i * phes: phoff + (i + 1) * phes])
        if typ != 1 or not fsz or not (flags & 1):
            continue
        words = struct.unpack('>%dI' % (fsz // 4), elf[off: off + (fsz // 4) * 4])
        for k, w in enumerate(words):
            if (w >> 26) == 19 and (w & 1) and ((w >> 1) & 0x3FF) in (528, 16):
                ic.append(va + 4 * k)
            elif w == 0x44000002:
                sc.append(va + 4 * k)
    return sorted(ic), sorted(sc)


def split_functions(lines):
    """[(name, addr, suffix, i_open, i_close)] for each lifted function."""
    out = []
    i, n = 0, len(lines)
    while i < n:
        m = FN_START.match(lines[i].rstrip('\r\n'))
        if not m:
            i += 1
            continue
        j = i + 1
        while j < n and not lines[j].startswith('}'):
            j += 1
        if j >= n:
            raise SystemExit('ss_resume_pass: unterminated function at line %d' % (i + 1))
        out.append((m.group(1), int(m.group(2), 16), m.group(3), i, j))
        i = j + 1
    return out


def scan(body):
    """Sites and known addresses of one function body, in text order.
    sites: dicts kind ('call','ic','tj','sc','hle'), line, span, ra (call), delta.
    events: ('a', addr) known addresses, ('s', site index) sites to place from the ELF."""
    sites, events = [], []
    delta, dyn = 0, False
    last_lr = None
    bad = None
    for li, l in enumerate(body):
        last_lr = None
        for m in TOK.finditer(l):
            g = m.lastgroup
            if m.group('la'):
                events.append(('a', int(m.group('la'), 16)))
            elif m.group('lr') is not None:
                last_lr = int(m.group('lr'), 16)
                events.append(('a', last_lr))
            elif m.group('fc'):
                if last_lr is None:
                    bad = bad or 'call without lr'
                sites.append(dict(kind='call', line=li, span=m.span('fc'), ra=last_lr,
                                  delta=DELTA_DYN if dyn else delta))
                last_lr = None
            elif m.group('ic'):
                kind = 'ic' if m.group('icd') else 'tj'
                sites.append(dict(kind=kind, line=li, span=m.span('ic'), ra=None,
                                  delta=DELTA_DYN if dyn else delta))
                if kind == 'ic':
                    events.append(('s', len(sites) - 1))
            elif m.group('sc'):
                sites.append(dict(kind='sc', line=li, span=m.span('sc'), ra=None,
                                  delta=DELTA_DYN if dyn else delta))
                events.append(('s', len(sites) - 1))
            elif m.group('hle'):
                sites.append(dict(kind='hle', line=li, span=m.span('hle'), ra=None,
                                  delta=DELTA_DYN if dyn else delta))
            elif m.group('stdu'):
                delta -= int(m.group('stdu'), 16)
            elif m.group('addi') is not None:
                v = int(m.group('addi'), 0)
                if v < 0:
                    delta += v          # an allocation; positive = a frame pop before a return
            elif m.group('r1other') is not None:
                src = m.group('r1other')
                if not (src.startswith('vm_read') or re.fullmatch(r'ctx->gpr\[\d+\] \| ctx->gpr\[\d+\]', src)
                        or src.startswith('rc_g')):
                    dyn = True          # stdux / add: frame size known only at run time
    return sites, events, bad


def place(sites, events, fn, ic_addrs, sc_addrs):
    """Give each bctrl its return address and each sc its address from the ELF.
    Returns None or the reason it failed."""
    lo = fn
    pend = []
    for e in events + [('a', None)]:
        if e[0] == 's':
            pend.append(e[1])
            continue
        hi = e[1]
        if pend:
            for kind, addrs in (('ic', ic_addrs), ('sc', sc_addrs)):
                idx = [k for k in pend if sites[k]['kind'] == kind]
                if not idx:
                    continue
                j = bisect.bisect_left(addrs, lo)
                if hi is not None and hi > lo:
                    cand = addrs[j:bisect.bisect_left(addrs, hi)]
                else:
                    cand = [a for a in addrs[j:j + len(idx)] if a < lo + 0x4000]
                if len(cand) != len(idx):
                    return '%s count %d vs %d in ELF at 0x%X' % (kind, len(idx), len(cand), lo)
                for k, a in zip(idx, cand):
                    sites[k]['addr'] = a
            pend = []
        if hi is not None and hi > lo:
            lo = hi
    return None


def raw_cs_refill(raw_body):
    """_cs_N refill expressions (relative to the entry r1 `_sse`) from the plain lifted body,
    or (None, reason)."""
    refill = {}
    # the lifter puts the _cs_N lines at the top, sometimes after a hand-added hook line
    cs_idx = [n for n, l in enumerate(raw_body) if 'uint64_t _cs_' in l]
    prefix = cs_idx[-1] + 1 if cs_idx else 0
    regs = []
    for l in (raw_body[n] for n in cs_idx):
        m = RAW_CS_REG.search(l)
        if m:
            regs.append(int(m.group(1)))
            continue
        m = RAW_CS_MEM.search(l)
        if m:
            refill[int(m.group(1))] = int(m.group(2), 0)
            continue
        return None, 'unknown _cs form'
    if not regs:
        return refill, None
    delta, dyn = 0, False
    defined = set()
    want = set(regs)
    for l in raw_body[prefix:]:
        for m in re.finditer(r'vm_write64\(ctx->gpr\[1\] \+ (-?(?:0x)?[0-9A-Fa-f]+), ctx->gpr\[(\d+)\]\);', l):
            r = int(m.group(2))
            if r in want and r not in defined and r not in refill:
                if dyn:
                    return None, 'save after a dynamic frame'
                refill[r] = delta + int(m.group(1), 0)
        for m in TOK.finditer(l):
            if m.group('stdu'):
                delta -= int(m.group('stdu'), 16)
            elif m.group('addi') is not None and int(m.group('addi'), 0) < 0:
                delta += int(m.group('addi'), 0)
            elif m.group('r1other') is not None:
                src = m.group('r1other')
                if not (src.startswith('vm_read') or re.fullmatch(r'ctx->gpr\[\d+\] \| ctx->gpr\[\d+\]', src)):
                    dyn = True
        for m in re.finditer(r'ctx->gpr\[(\d+)\] (?:=|\+=|-=|\|=|&=|\^=)(?!=)', l):
            defined.add(int(m.group(1)))
        if all(r in refill for r in regs):
            break
    missing = [r for r in regs if r not in refill]
    if missing:
        return None, 'no stack save for _cs_%d' % missing[0]
    return refill, None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = dict(a.split('=', 1) if '=' in a else (a, '1') for a in sys.argv[1:] if a.startswith('--'))
    if len(args) != 2 or '--raw' not in opts or '--elf' not in opts or '--hdr' not in opts:
        raise SystemExit(__doc__)
    src, dst = args
    tu = opts.get('--tu', '0')
    ic_addrs, sc_addrs = parse_elf(opts['--elf'])
    lines = open(src, 'r', encoding='latin-1', newline='').read().splitlines(True)
    raw_lines = open(opts['--raw'], 'r', encoding='latin-1', newline='').read().splitlines(True)
    raw_fns = {name: (i, j) for name, _a, _s, i, j in split_functions(raw_lines)}
    st = Counter()
    bad_why = Counter()
    table = []
    edits = {}      # line index -> list of (pos, text) insertions
    blocks = {}     # line index -> resume block line to put after it
    cs_rewrite = set()   # lines whose "uint64_t _cs_N =" become "_cs_N ="
    seen_fn = set()
    for name, fn, suffix, i, j in split_functions(lines):
        st['functions'] += 1
        body = lines[i + 1:j]
        # The resume block goes after the register-cache declarations (plain reads of ctx), before
        # anything else, so a resume skips all entry code (hooks too). The _cs_N snapshots are
        # declared with the block and their lines become assignments: a goto may not skip an
        # initialised declaration, and the block refills them from the guest stack.
        k = 0
        while k < len(body) and DECL_LINE.match(body[k]) and '_cs_' not in body[k]:
            k += 1
        cs_names, cs_lines = [], []
        for n, l in enumerate(body):
            if 'uint64_t _cs_' in l:
                cs_names += [int(x) for x in CS_DECL.findall(l)]
                cs_lines.append(i + 1 + n)
        at = i + k            # insert the block after line `at` (the function line when k == 0)
        why = None
        if suffix or fn in seen_fn:
            why = 'twin or duplicate function'
        seen_fn.add(fn)
        sites, events, why2 = scan(body)
        why = why or why2
        refill = {}
        if not why:
            why = place(sites, events, fn, ic_addrs, sc_addrs)
        if not why:
            if name not in raw_fns:
                why = 'not in the plain TU'
            else:
                ri, rj = raw_fns[name]
                raw_body = raw_lines[ri + 1:rj]
                rsites, _rev, rwhy = scan(raw_body)
                if rwhy:
                    why = rwhy
                elif [s['kind'] for s in rsites] != [s['kind'] for s in sites] or \
                        [s['ra'] for s in rsites] != [s['ra'] for s in sites]:
                    why = 'plain and final sites differ'
                else:
                    for s, r in zip(sites, rsites):
                        s['delta'] = r['delta']
                    refill, rwhy = raw_cs_refill(raw_body)
                    why = rwhy
                    if not why and sorted(refill) != sorted(cs_names):
                        why = 'plain and final _cs differ'
        if not why and sum(1 for s in sites if s['kind'] == 'hle') > 1:
            why = 'two HLE calls'
        cases = []
        if not why:
            keys = set()
            for n, s in enumerate(sites):
                kind = s['kind']
                if kind in ('call', 'ic'):
                    ra = s['ra'] if kind == 'call' else s['addr'] + 4
                    key = (K_CALL, ra)
                    if key in keys:
                        why = 'same return address twice'
                        break
                    keys.add(key)
                    cases.append((key, 'ss_resume_call(ctx, _ssf); goto ss_r%d;' % n))
                    table.append((fn, ra, K_CALL, s['delta']))
                    s['edit'] = [(s['span'][1], ' ss_r%d:;' % n)]
                    if kind == 'ic':
                        s['edit'].append((s['span'][0], 'ctx->lr = 0x%08X; ' % ra))
                elif kind == 'tj':
                    if (K_TAIL, fn) not in keys:
                        keys.add((K_TAIL, fn))
                        cases.append(((K_TAIL, fn), 'ss_resume_call(ctx, _ssf); return;'))
                        table.append((fn, fn, K_TAIL, 0))
                    s['edit'] = []
                elif kind == 'sc':
                    a = s['addr']
                    if (K_SC_IN, a) in keys:
                        why = 'same sc twice'
                        break
                    keys.add((K_SC_IN, a))
                    cases.append(((K_SC_IN, a), 'goto ss_s%d;' % n))
                    cases.append(((K_SC_OUT, a), 'goto ss_x%d;' % n))
                    table.append((fn, a, K_SC_IN, s['delta']))
                    table.append((fn, a, K_SC_OUT, s['delta']))
                    s['edit'] = [(s['span'][0], 'ctx->cia = 0x%08X; ss_s%d:; ' % (a, n)),
                                 (s['span'][1], ' ss_x%d:;' % n)]
                elif kind == 'hle':
                    cases.append(((K_HLE_IN, fn), 'goto ss_s%d;' % n))
                    cases.append(((K_HLE_OUT, fn), 'goto ss_x%d;' % n))
                    table.append((fn, fn, K_HLE_IN, s['delta']))
                    table.append((fn, fn, K_HLE_OUT, s['delta']))
                    s['edit'] = [(s['span'][0], 'ss_s%d:; ' % n), (s['span'][1], ' ss_x%d:;' % n)]
        if why:
            # drop this function's table rows added before the failure
            while table and table[-1][0] == fn and table[-1][2] != K_NONE:
                table.pop()
            table.append((fn, 0, K_NONE, 0))
            bad_why[why.split(' at ')[0]] += 1
            st['not resumable'] += 1
            blocks[at] = ('        if (__builtin_expect(g_ss_resume != 0, 0)) { const ss_rframe* _ssf = '
                          'ss_resume_frame(ctx, 0x%08Xu); if (_ssf) ss_resume_fail(ctx, _ssf); }\n' % fn)
            continue
        st['resumable'] += 1
        cs_rewrite.update(cs_lines)
        for s in sites:
            st['site ' + s['kind']] += 1
            for pos, text in s.get('edit', []):
                edits.setdefault(i + 1 + s['line'], []).append((pos, text))
        fill = ''.join(' _cs_%d = vm_read64((uint32_t)(_sse + (%d)));' % (r, off) for r, off in sorted(refill.items()))
        sw = ' '.join('case 0x%016XULL: %s' % ((kind << 32) | addr, act) for (kind, addr), act in cases)
        blocks[at] = ((('    uint64_t %s;   /* ss_resume_pass.py: _cs_N declared before the resume block */\n'
                        % ', '.join('_cs_%d' % r for r in sorted(set(cs_names)))) if cs_names else '') +
                      '        if (__builtin_expect(g_ss_resume != 0, 0)) { const ss_rframe* _ssf = '
                      'ss_resume_frame(ctx, 0x%08Xu); if (_ssf) { const uint32_t _sse = _ssf->entry_sp; (void)_sse;%s '
                      'switch (_ssf->key) { %s default: break; } ss_resume_fail(ctx, _ssf); } }\n'
                      % (fn, fill, sw))
    o = ['/* ss_resume_pass.py: save-state resume points -- generated at build time, do not edit. */\n']
    inc_done = False
    for li, l in enumerate(lines):
        if not inc_done and l.startswith('#include "ppu_recomp.h"'):
            o.append(l)
            o.append('#include "%s"   /* ss_resume_pass.py */\n' % opts['--hdr'].replace('\\', '/'))
            inc_done = True
            continue
        if li in edits:
            nl = l
            for pos, text in sorted(edits[li], key=lambda e: -e[0]):
                nl = nl[:pos] + text + nl[pos:]
            l = nl
        if li in cs_rewrite:
            l = l.replace('uint64_t _cs_', '_cs_')
        o.append(l)
        if li in blocks:
            o.append(blocks[li])
    if not inc_done:
        raise SystemExit('ss_resume_pass: no #include "ppu_recomp.h" line in ' + src)
    o.append('\n/* ss_resume_pass.py: site table (function, address, kind, r1 at the site minus r1 at entry) */\n')
    o.append('static const ss_site ss_sites_%s[] = {\n' % tu)
    for fn, addr, kind, delta in table:
        o.append('    { 0x%08Xu, 0x%08Xu, %du, %d },\n' % (fn, addr, kind, delta))
    o.append('};\n')
    o.append('namespace { struct ss_sites_reg_%s { ss_sites_reg_%s() { ss_register_sites(ss_sites_%s, %du); } } '
             'ss_sites_reg_obj_%s; }\n' % (tu, tu, tu, len(table), tu))
    open(dst, 'w', encoding='latin-1', newline='').write(''.join(o))
    if '--stats' in opts:
        print('ss_resume_pass %s: %s' % (src.replace('\\', '/').split('/')[-1],
                                        ', '.join('%s %d' % kv for kv in sorted(st.items()))))
        if bad_why:
            print('  not resumable: ' + ', '.join('%s %d' % kv for kv in bad_why.most_common()))


if __name__ == '__main__':
    main()
