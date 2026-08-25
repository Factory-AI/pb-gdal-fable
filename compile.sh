#!/bin/sh
set -e
cd "$(dirname "$0")"
make -s -j"$(nproc 2>/dev/null || echo 4)"
