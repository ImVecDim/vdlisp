#!/usr/bin/env python3
import sys
from pathlib import Path

def count_file(path: Path) -> int:
    in_block = False
    cnt = 0
    try:
        with path.open('r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                i = 0
                n = len(line)
                out_chars = []
                while i < n:
                    if in_block:
                        idx = line.find('*/', i)
                        if idx == -1:
                            i = n
                            break
                        else:
                            in_block = False
                            i = idx + 2
                            continue
                    ch = line[i]
                    if ch == '"':
                        out_chars.append(ch)
                        i += 1
                        while i < n:
                            if line[i] == '\\':
                                i += 2
                            elif line[i] == '"':
                                out_chars.append('"')
                                i += 1
                                break
                            else:
                                i += 1
                        continue
                    if ch == "'":
                        out_chars.append(ch)
                        i += 1
                        while i < n:
                            if line[i] == '\\':
                                i += 2
                            elif line[i] == "'":
                                out_chars.append("'")
                                i += 1
                                break
                            else:
                                i += 1
                        continue
                    if line.startswith('//', i):
                        break
                    if line.startswith('/*', i):
                        in_block = True
                        i += 2
                        continue
                    out_chars.append(ch)
                    i += 1
                s = ''.join(out_chars).strip()
                if s:
                    cnt += 1
    except Exception as e:
        print(f"Error reading {path}: {e}", file=sys.stderr)
    return cnt


def main():
    root =  Path('src')
    exts = {'.c', '.cpp', '.h', '.hpp'}
    total = 0
    files = sorted([p for p in root.rglob('*') if p.suffix in exts and p.is_file()])
    for p in files:
        c = count_file(p)
        print(f"{c:6d} {p}")
        total += c
    print(f"TOTAL: {total}")

if __name__ == '__main__':
    main()
