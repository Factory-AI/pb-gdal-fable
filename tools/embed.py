#!/usr/bin/env python3
"""Build-time: embed spec/ files into the binary via .incbin."""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEC = os.path.join(ROOT, "spec")
BUILD = os.path.join(ROOT, "build")


def main():
    os.makedirs(BUILD, exist_ok=True)
    files = []
    for dirpath, _, names in os.walk(SPEC):
        for n in sorted(names):
            full = os.path.join(dirpath, n)
            rel = os.path.relpath(full, SPEC).replace(os.sep, "/")
            files.append((rel, full))
    files.sort()

    with open(os.path.join(BUILD, "embedded.S"), "w") as s:
        s.write('.section .rodata\n')
        for i, (rel, full) in enumerate(files):
            s.write(f'.global emb_{i}_start\n.global emb_{i}_end\n')
            s.write(f'emb_{i}_start:\n.incbin "{full}"\nemb_{i}_end:\n')

    with open(os.path.join(BUILD, "embedded_index.cpp"), "w") as c:
        c.write('#include "../src/embedded.h"\n')
        for i in range(len(files)):
            c.write(f'extern "C" const char emb_{i}_start[], emb_{i}_end[];\n')
        c.write("static const EmbeddedFile g_files[] = {\n")
        for i, (rel, _) in enumerate(files):
            c.write(f'  {{"{rel}", emb_{i}_start, emb_{i}_end}},\n')
        c.write("};\n")
        c.write(
            "const EmbeddedFile* embeddedFiles(size_t& count) {"
            " count = sizeof(g_files)/sizeof(g_files[0]); return g_files; }\n"
        )


if __name__ == "__main__":
    main()
