# gdal-fable

A cleanroom recreation of the GDAL 3.13 `gdal` command line interface, written
by an AI model that could run the real thing but never see its source.

Not affiliated with OSGeo or GDAL, and not a substitute for either. If you need
geospatial tooling, use [GDAL](https://github.com/OSGeo/gdal).

## What this is

Produced autonomously by Fable 5 on ProgramBench task `osgeo__gdal.0847f12`,
over 8 days and 10 review passes. The only reference available was an
execute-only binary: GDAL `3.13.0dev-cbc00e6b71`, trimmed to 11 drivers with no
GEOS. No source, no headers, no build system, no tests, no network. Every
behavior here was recovered by running that binary and comparing bytes.

Write-up: [Rebuilding Software from the Outside In](https://factory.ai/news/rebuilding-software-from-the-outside-in)

## Result

| | |
| --- | --- |
| Held-out behavioral suite | **1,184 / 1,319 (89.8%)**, 90.3% on the official metric |
| Same model, single agent, no validation system | 35.8% |
| Size | 115,163 lines of C++17 in 104 files, 125 command paths |
| Upstream surface reached through this CLI | ~600k lines of GDAL's 1.9M |

## It shares no code with GDAL

Measured across all 104 files here against all 2,952 upstream C/C++ files at
commit `0847f12`:

| Measure | Result |
| --- | --- |
| Verbatim shared line runs (4+ non-trivial lines) | **0** |
| Code 7-gram overlap, string literals stripped | 4.18%, all generic C++ idiom |
| Shared string literals (12+ chars) | 21.2%, i.e. GDAL's error messages, reproduced on purpose |

No GDAL API or convention appears anywhere: no `CPLError`, no `GDALDataset` as a
type, no `poDS`/`papsz`. Where those names occur, they are inside string
literals, because the reference prints them.

Four libraries are linked rather than rewritten, because byte-identical output is
impossible without them and the reference links the same ones: PROJ 8.2.1 (all
CRS text comes from the system `proj.db`), libdeflate, zstd, libwebp. libtiff,
libgeotiff, libjpeg, libpng and expat were all available and went unused; the
TIFF container, GeoTIFF keys, JPEG codec, XML and JSON parsers are original.

## Layout

| Path | |
| --- | --- |
| `src/` | the implementation; `engine.cpp` parses, `handlers_*.cpp` implement, `dataset.h` is the raster model |
| `spec/` | 6.2 MB of output captured from the reference, embedded into the binary at build time. Required to compile. |
| `tests/` | the model's own differential harness plus ~10,000 pinned cases; runs the reference and this build in twin sandboxes and byte-compares |
| `NOTES.md` | 5,000 lines of derived behavioral laws and their evidence. The most interesting file here. |
| `README.task.md` | the product description handed to the model, harness-supplied |

## Building

Linux x86_64 with AVX, and nothing else without edits:

```sh
./compile.sh   # make + g++ -std=c++17 -> ./executable
```

`Makefile` hardcodes absolute paths for libproj, libdeflate, libzstd and
libwebp; `src/handlers_vector_grid.cpp` uses AVX intrinsics (matching upstream's
grid path requires `_mm256_rcp_ps` and its 12-bit reciprocal error); PROJ must be
8.2.1 or CRS text drifts. `tests/run_diff.py` needs the reference binary, which
is not distributable, so the differential suite cannot run outside the benchmark
container. The pinned cases still read as a behavioral specification.

Known gaps: multidimensional (`mdim info` modes, `mdim convert` subsetting),
verbs used as steps inside `pipeline`, and hidden option aliases such as
`--z-field` that appear in no help output and so cannot be probed. Geometry verbs
(`buffer`, `dissolve`, `simplify`, `clip` by geometry) refuse with `ERROR 6: GEOS
support not enabled.`, which is what the reference does.

## Provenance

The first commit, `task scaffold`, contains everything the task placed in the
workspace before the model started: six files byte-identical to GDAL at
`0847f12` plus a paraphrased product description. Everything else is in the
second commit. `LICENSE.TXT` stays at its original path because the reference's
`--license` reads it at runtime; it describes GDAL, not this code. `spec/`
contains bytes captured from an MIT-licensed GDAL build.

Two license files sit in this repository, covering different things:

| File | Covers |
| --- | --- |
| `COPYING.md` | MIT. The original code in `src/`, `tests/` and `tools/`. |
| `LICENSE.TXT` | GDAL's own terms, from the task scaffold. Kept at this path because the reference reads it at runtime for `--license`. |
