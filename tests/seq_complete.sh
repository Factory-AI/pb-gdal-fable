#!/bin/bash
# Compare `gdal completion` output between oracle and candidate.
# Usage: tests/seq_complete.sh [oracle] [mine]
O=${1:-/home/agent/oracle/executable}
M=${2:-/workspace/executable}
T=$(mktemp -d)
trap "rm -rf $T" EXIT

cd "$T"
mkdir -p subdir "sp dir" empty
touch c.tif x.tiff m1.vrt subdir/inner.tif "sp file.tif" epsg.txt noext
touch a.json a.geojson a.geojsonl a.geojsons a.shz a.png a.gpkg .hidden.tif
printf '{"type":"FeatureCollection","features":[{"type":"Feature","properties":{},"geometry":{"type":"Point","coordinates":[1,2]}}]}' > v.geojson
"$O" vector convert -q --of "ESRI Shapefile" v.geojson v.shp 2>/dev/null
"$O" raster create -q --size 2,2 r.tif 2>/dev/null

pass=0; fail=0
run() {
  local eo eo_rc em em_rc
  eo=$("$O" completion "$@" 2>&1); eo_rc=$?
  em=$("$M" completion "$@" 2>&1); em_rc=$?
  if [ "$eo" = "$em" ] && [ "$eo_rc" = "$em_rc" ]; then
    pass=$((pass+1))
  else
    fail=$((fail+1))
    echo "FAIL: completion $*"
    if [ "$eo_rc" != "$em_rc" ]; then echo "  rc: oracle=$eo_rc mine=$em_rc"; fi
    echo "  oracle: $(echo "$eo" | head -c 300)"
    echo "  mine:   $(echo "$em" | head -c 300)"
  fi
}

# --- top level / branch lists ---
run
run gdal
run gdal ra
run gdal xyz
run bash
run zsh gdal
run gdal raster
run gdal raster in
run gdal vector
run gdal dataset
run gdal driver
run gdal vsi
run gdal mdim
run gdal vsi sozip
run gdal raster overview
run gdal convert
run gdal info
run gdal raster info
run gdal vector convert
run gdal raster tile
run gdal completion
run gdal completion b

# --- root options ---
run gdal -
run gdal --
run gdal --c
run gdal --config
run gdal --config CPL
run gdal --config CPL_DEBUG=
run gdal --config CPL_DEBUG=ON
run gdal --config CPL_DEBUG=ON ra
run gdal --version
run gdal --help
run gdal --drivers
run gdal --json-usage

# --- leaf option lists ---
run gdal raster info --
run gdal raster info -
run gdal raster info --ch
run gdal raster info --checksum
run gdal raster info --checksum --
run gdal raster info --checksum --ch
run gdal raster info --zz
run gdal raster info --help
run gdal raster info --config
run gdal raster info --config CPL
run gdal vector info --
run gdal raster convert --
run gdal vector convert --
run gdal raster edit --
run gdal raster create --
run gdal raster reproject --
run gdal vector reproject --
run gdal raster tile --
run gdal raster clean-collar --
run gdal dataset identify --
run gdal dataset copy --
run gdal vsi list --
run gdal vsi copy --
run gdal mdim info --
run gdal mdim convert --
run gdal raster mosaic --
run gdal raster overview add --
run gdal info --
run gdal convert --
run gdal raster pixel-info --
run gdal vector grid --
run gdal vector grid invdist --

# --- choices ---
run gdal raster info --output-format
run gdal raster info --crs-format
run gdal raster info --output-format=
run gdal raster info --output-format=j
run gdal vector info --limit
run gdal vector info --limit=
run gdal vector info --sql
run gdal raster convert --of
run gdal raster convert --of=
run gdal raster convert --of G
run gdal raster convert --of=G
run gdal raster convert -f
run gdal vector convert --of
run gdal raster info --if
run gdal vector info --if
run gdal mdim info --if
run gdal mdim convert --of
run gdal convert --of
run gdal convert --if
run gdal info --if
run gdal dataset copy s d --format
run gdal dataset copy s d -f
run gdal raster reproject --resampling
run gdal raster reproject --r
run gdal raster reproject -r
run gdal raster reproject --re
run gdal raster reproject --res
run gdal raster reproject --resolution
run gdal raster reproject --resampling n
run gdal raster overview add -r
run gdal raster tile --convention
run gdal raster tile --webviewer
run gdal vsi list --of
run gdal raster info --i
run gdal raster info -i

# --- CRS completion ---
run gdal raster reproject --dst-crs
run gdal raster reproject --dst-crs E
run gdal raster reproject --dst-crs EPSG
run gdal raster reproject --dst-crs epsg
run gdal raster reproject --dst-crs EPSG:
run gdal raster reproject --dst-crs epsg:
run gdal raster reproject --dst-crs EPSG:43
run gdal raster reproject --dst-crs EPSG:4326
run gdal raster reproject --dst-crs=EPSG:43
run gdal raster reproject --dst-crs ESRI:
run gdal raster reproject --dst-crs IAU_2015:
run gdal raster reproject --dst-crs IGNF:
run gdal raster reproject --dst-crs NKG:
run gdal raster reproject --dst-crs OGC:
run gdal raster reproject --dst-crs OGC:CRS8
run gdal raster reproject --dst-crs FOO:
run gdal raster reproject --dst-crs urn:
run gdal raster reproject --src-crs
run gdal raster reproject --bbox-crs
run gdal raster create --crs
run gdal raster edit --crs
run gdal vector edit --crs
run gdal vector create --crs
run gdal raster pixel-info --position-crs
run gdal vector concat --dst-crs
run gdal raster clip --bbox-crs
run gdal raster update --geometry-crs
run gdal vector grid average --crs
run gdal raster footprint --dst-crs

# --- filename completion ---
run gdal raster info ""
run gdal raster info c
run gdal raster info s
run gdal raster info sp
run gdal raster info .h
run gdal raster info subdir/
run gdal raster info subdir/i
run gdal raster info empty/
run gdal raster info nosuchdir/
run gdal raster info xyz
run gdal vector info ""
run gdal info ""
run gdal mdim info ""
run gdal dataset identify ""
run gdal dataset copy ""
run gdal dataset copy c.tif ""
run gdal vsi copy ""
run gdal vsi list ""
run gdal raster convert ""
run gdal raster convert c.tif ""
run gdal raster convert c.tif out.tif ""
run gdal raster mosaic ""
run gdal raster info --input
run gdal raster info --input c
run gdal raster info --input=c
run gdal raster info --input=
run gdal raster convert -i
run gdal raster convert --input c.tif ""
run gdal raster reproject --like
run gdal raster info --checksum c

# --- creation/open option catalogs ---
run gdal raster convert --co
run gdal raster convert --co C
run gdal raster convert --creation-option
run gdal raster convert --of GTiff --co
run gdal raster convert --of gtiff --co
run gdal raster convert --of GTiff --co COMPRESS=
run gdal raster convert --of GTiff --co compress=
run gdal raster convert --of GTiff --co COMPRESS=L
run gdal raster convert --of GTiff --co TILED=
run gdal raster convert --of GTiff --co ZLEVEL=
run gdal raster convert --of GTiff --co FOO=
run gdal raster convert --of COG --co
run gdal raster convert --of COG --co COMPRESS=
run gdal raster convert --of VRT --co
run gdal raster convert --of MEM --co
run gdal raster convert --of GDALG --co
run gdal raster convert c.tif out.tif --co
run gdal raster convert c.tif out.png --co
run gdal raster convert c.tif out.vrt --co
run gdal raster convert c.tif --co
run gdal raster create --co
run gdal raster create out.tif --co
run gdal raster create --of GTiff --co
run gdal vector convert --co
run gdal vector convert --of GeoJSON --co
run gdal vector convert --of GeoJSON --lco
run gdal vector convert --of GeoJSONSeq --lco
run gdal vector convert --lco
run gdal vector convert v.geojson out.shp --lco
run gdal vector convert v.geojson out.geojsonl --lco
run gdal vector convert v.geojson out.json --lco
run gdal vector convert --of MEM --lco
run gdal raster info --oo
run gdal raster info c.tif --oo
run gdal raster info --if GTiff --oo
run gdal raster convert --if GTiff --oo
run gdal raster convert --if COG --oo
run gdal raster convert --if VRT --oo
run gdal raster convert --if MEM --oo
run gdal vector info v.shp --oo
run gdal vector info a.geojson --oo
run gdal vector info --if TopoJSON --oo
run gdal vector info --if ESRIJSON --oo
run gdal vector info --if GeoJSONSeq --oo
run gdal mdim info m1.vrt --oo
run gdal mdim convert --of VRT --co
run gdal raster convert --of GTiff --co COMPRESS=LZW --co
run gdal raster convert --of=GTiff --co

# --- description hints ---
run gdal raster reproject --resolution
run gdal raster reproject --bbox
run gdal vector info --where
run gdal raster info --metadata-domain
run gdal raster info --subdataset
run gdal raster convert c.tif out.tif --co PHOTOMETRIC=
run gdal raster edit --metadata

# --- pipeline ---
run gdal pipeline
run gdal pipeline --
run gdal pipeline --input
run gdal pipeline read
run gdal pipeline rea
run gdal pipeline read ""
run gdal pipeline read --
run gdal pipeline read --input
run gdal pipeline read --input-format
run gdal pipeline read --if
run gdal pipeline read --input ""
run gdal pipeline concat --
run gdal pipeline calc --
run gdal pipeline create --
run gdal pipeline external --
run gdal pipeline mosaic --
run gdal pipeline stack --
run gdal pipeline read c.tif !
run gdal pipeline read r.tif !
run gdal pipeline read v.shp !
run gdal pipeline read nosuch !
run gdal pipeline read c.tif ! rep
run gdal pipeline read c.tif ! reproject
run gdal pipeline read c.tif ! reproject --
run gdal pipeline read v.shp ! reproject --
run gdal pipeline read nosuch ! reproject --
run gdal pipeline read c.tif ! reproject --resampling
run gdal pipeline read c.tif ! reproject -r
run gdal pipeline read c.tif ! reproject --dst-crs
run gdal pipeline read v.shp ! reproject --dst-crs
run gdal pipeline read c.tif ! edit --crs
run gdal pipeline read c.tif ! reproject ! write --
run gdal pipeline read c.tif ! write --
run gdal pipeline read v.shp ! write --
run gdal pipeline read c.tif ! write --of
run gdal pipeline read v.shp ! write --of
run gdal pipeline read c.tif ! write --co
run gdal pipeline read c.tif ! write --of COG --co
run gdal pipeline read r.tif ! write --of COG --co
run gdal pipeline read nosuch ! write --of COG --co
run gdal pipeline read v.shp ! write --of COG --co
run gdal pipeline read r.tif ! tile
run gdal pipeline read r.tif ! tile ""
run gdal pipeline read r.tif ! tile --
run gdal pipeline read r.tif ! tile --convention
run gdal pipeline c.tif
run gdal pipeline rea
run gdal raster pipeline rea
run gdal pipeline read r.tif ""
run gdal pipeline read v.shp ""
run gdal pipeline read --input=
run gdal pipeline read --input x
run gdal raster tile ""
run gdal raster tile c
run gdal pipeline read c.tif ! write --of GTiff --co COMPRESS=
run gdal pipeline read v.shp ! write --lco
run gdal pipeline read v.shp ! write --of GeoJSON --lco
run gdal pipeline read c.tif ! write ""
run gdal pipeline read c.tif ! write --output
run gdal pipeline read c.tif ! tee --
run gdal pipeline read c.tif ! materialize --
run gdal pipeline read c.tif ! info --
run gdal pipeline read v.shp ! filter --
run gdal pipeline read v.shp ! filter --where
run gdal pipeline read c.tif ! clip --
run gdal pipeline read c.tif ! clip --bbox
run gdal pipeline read c.tif ! scale --
run gdal pipeline read c.tif ! tile --
run gdal pipeline read c.tif ! aspect --convention
run gdal pipeline read c.tif ! blend --operator
run gdal pipeline !
run gdal raster pipeline
run gdal raster pipeline --
run gdal raster pipeline read c.tif !
run gdal raster pipeline read nosuch !
run gdal raster pipeline read c.tif ! reproject --
run gdal raster pipeline read c.tif ! write --of
run gdal vector pipeline
run gdal vector pipeline --
run gdal vector pipeline read v.shp !
run gdal vector pipeline read nosuch !
run gdal vector pipeline read v.shp ! reproject --
run gdal vector pipeline read v.shp ! write --of
run gdal vector pipeline read v.shp ! buffer --
run gdal pipeline read c.tif ! unknownstep --
run gdal pipeline read c.tif ! reproject --zz

# --- misc / fallbacks ---
run gdal vsi sozip create --
run gdal vsi sozip ""
run gdal vsi delete --
run gdal vsi move ""
run gdal vsi sync --
run gdal driver gtiff
run gdal driver --
run gdal raster --
run gdal vector --
run gdal mdim --
run gdal dataset --
run gdal vsi --
run gdal nosuch nosuch2 --
run gdal raster nosuchleaf --
run gdal raster info nosuch.tif ""
run gdal raster info -- ""
run gdal completion gdal
run gdal help
run gdal raster help

echo "complete: pass=$pass fail=$fail"
[ "$fail" = 0 ]
