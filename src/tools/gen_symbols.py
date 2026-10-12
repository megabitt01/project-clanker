"""Builds symbols/swbf2.sym (address -> function name) from a Ghidra export.

Usage: python gen_symbols.py <functions.tsv from ExportAll.java> <out.sym>
The map contains names only (no code) and is used for crash reports.
"""
import sys

def main(src, dst):
    out = []
    for line in open(src, encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        if len(f) < 2:
            continue
        addr = int(f[0], 16)
        name = f[1].replace(' ', '_')
        out.append((addr, name))
    out.sort()
    with open(dst, 'w', encoding='utf-8') as o:
        for a, n in out:
            o.write(f'{a:08x} {n}\n')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
