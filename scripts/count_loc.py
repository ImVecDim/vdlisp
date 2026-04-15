#!/usr/bin/env python3
import sys
from pathlib import Path

# 统计 src 下 C/C++ 源文件的“有效代码行数”，会尽量忽略注释与空行。
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
                        # 多行注释跨文件行持续生效，直到遇到 */ 为止。
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
                        # 字符串字面量内部的 // 或 /* 不应该被误判成注释。
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
                        # 同样跳过字符字面量，避免转义字符扰乱扫描。
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
    # 只统计源目录下的 C/C++ 实现，不把测试脚本和构建产物算进去。
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
