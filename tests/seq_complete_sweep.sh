#!/bin/bash
# Exhaustive `gdal completion` differential sweep: replays the captured case
# corpus (tests/data/complete_cases*.txt) against oracle and candidate in a
# scratch fixture directory.
# Usage: tests/seq_complete_sweep.sh [oracle] [mine]
O=${1:-/home/agent/oracle/executable}
M=${2:-/workspace/executable}
D=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap "rm -rf $T" EXIT
cd "$T"

printf '{"type":"FeatureCollection","features":[{"type":"Feature","properties":{"name":"a","val":1},"geometry":{"type":"Point","coordinates":[1,2]}}]}' > fldsrc.geojson
printf '{"type":"FeatureCollection","features":[{"type":"Feature","properties":{},"geometry":{"type":"Point","coordinates":[1,2]}}]}' > t.geojson
cp t.geojson f.geojson
"$O" vector convert -q --of "ESRI Shapefile" t.geojson v2.shp 2>/dev/null
"$O" vector convert -q --of "ESRI Shapefile" fldsrc.geojson fld.shp 2>/dev/null
rm fldsrc.geojson
"$O" raster create -q --size 2,2 r2.tif 2>/dev/null
touch c.tif v.shp v.dbf x.json "sp file.tif"
mkdir -p subx suby
touch subx/in.tif subx/pts.shp
for b in aa bb; do
  for e in shp shx dbf prj; do cp "fld.$e" "suby/$b.$e"; done
done

pass=0; fail=0
for f in "$D"/data/complete_cases*.txt; do
  while IFS=$'\t' read -r -a words; do
    eo=$("$O" completion "${words[@]}" 2>&1)
    em=$("$M" completion "${words[@]}" 2>&1)
    if [ "$eo" = "$em" ]; then
      pass=$((pass+1))
    else
      fail=$((fail+1))
      printf 'FAIL: completion %s\n' "${words[*]}"
      if [ $fail -le 20 ]; then
        echo "  oracle: $(echo "$eo" | head -c 200)"
        echo "  mine:   $(echo "$em" | head -c 200)"
      fi
    fi
  done < "$f"
done
echo "complete_sweep: pass=$pass fail=$fail"
[ $fail -eq 0 ]
