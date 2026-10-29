import importlib.util
spec = importlib.util.spec_from_file_location("gt", "tools/gen-mp3tables.py")
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)

tabs = g.parse_huffcode('tools/HUFFCODE')
t = tabs[2]
print('table2 linbits', t['linbits'], 'ref', t['ref'])
print('rows:', t['rows'][:12])
bl = {}
for sym, ln, code in t['rows']:
    bl.setdefault(ln, []).append((code, sym))
for ln in sorted(bl):
    print(ln, sorted(bl[ln]))

for n in range(34):
    src = g.resolve(tabs, n)
    rows = tabs[src]['rows']
    quad = n >= 32
    canon = []
    for row in rows:
        if quad:
            v, ln, code = row
            canon.append((v, ln, code))
        else:
            x, y, ln, code = row
            canon.append(((x << 4) | y, ln, code))
    count, base, syms, ok = g.build_canonical(canon)
    if not ok:
        by_len = {}
        for sym, ln, code in canon:
            by_len.setdefault(ln, []).append((code, sym))
        for ln in sorted(by_len):
            lst = sorted(by_len[ln])
            codes = [c for c, _ in lst]
            gaps = [i for i in range(1, len(codes)) if codes[i] != codes[0] + i]
            if gaps:
                print('TABLE', n, 'len', ln, 'codes', codes)
