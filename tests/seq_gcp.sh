#!/bin/bash
# Sequential side-by-side harness: GCPs, VRT raster edit, Point-pixel shifts.
# Runs identical command chains against oracle and candidate in twin dirs;
# state carries across steps, every step diffs stdout/stderr/rc and all files.
O=${ORACLE:-/home/agent/oracle/executable}
C=$(dirname "$0")/../executable
C=$(readlink -f "$C")
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/o" "$W/c"
fail=0
pass=0
run() {
  local label="$1"; shift
  ( cd "$W/o" && "$O" "$@" >_out 2>_err; echo $? >_rc )
  ( cd "$W/c" && "$C" "$@" >_out 2>_err; echo $? >_rc )
  if ! diff -r -q "$W/o" "$W/c" >/dev/null 2>&1; then
    echo "FAIL: $label"
    diff -r "$W/o" "$W/c" 2>&1 | head -30
    fail=$((fail+1))
    rm -rf "$W/c"; cp -r "$W/o" "$W/c"
  else
    pass=$((pass+1))
  fi
}

run "create base" raster create -q --size 3,2 --burn 7 a.tif
run "convert to vrt" raster convert -q a.tif a.vrt
run "vrt edit crs" raster edit a.vrt --crs EPSG:32611
run "vrt edit bbox" raster edit a.vrt --bbox 100,200,130,220
run "vrt edit nodata" raster edit a.vrt --nodata 5
run "vrt edit metadata" raster edit a.vrt --metadata FOO=bar,BAZ=1
run "vrt edit unset metadata" raster edit a.vrt --unset-metadata FOO
run "vrt edit gcp" raster edit a.vrt --gcp 0.5,1.5,10,20 --gcp 2,0,30,40,7.5
run "vrt info text" raster info -f text a.vrt
run "vrt info json" raster info a.vrt
run "vrt edit stats" raster edit a.vrt --stats
run "vrt edit stats rerun" raster edit a.vrt --stats
run "vrt edit hist" raster edit a.vrt --hist
run "vrt edit hist rerun" raster edit a.vrt --hist
run "vrt edit stats hist" raster edit a.vrt --stats --hist
run "vrt edit approx" raster edit a.vrt --approx-stats
run "vrt gcp replace" raster edit a.vrt --gcp 1,1,5,6
run "vrt crs after gcp" raster edit a.vrt --crs EPSG:4326
run "vrt convert to vrt" raster convert -q a.vrt a2.vrt
run "vrt2 info text" raster info -f text a2.vrt
run "convert gcp vrt to tif" raster convert -q a.vrt g1.tif
run "gcp tif info" raster info -f text g1.tif
run "gcp tif info json" raster info g1.tif
run "convert gcp tif to tif" raster convert -q g1.tif g2.tif
run "convert gcp tif to vrt" raster convert -q g1.tif g3.vrt
run "gcp tif roundtrip info" raster info -f text g2.tif

run "edit tif gcp" raster edit a.tif --gcp 1,1,10,20 --gcp 0,0,30,40,5
run "tif gcp info" raster info -f text a.tif
run "tif gcp convert tif" raster convert -q a.tif b2.tif
run "tif gcp convert vrt" raster convert -q a.tif b2.vrt
run "b2 info" raster info -f text b2.tif

run "create pt" raster create -q --size 3,2 --burn 7 --crs EPSG:32611 --bbox 0,0,3,2 pt.tif
run "pt edit gcp" raster edit pt.tif --gcp 1,1,10,20 --gcp 0,0,30,40,5
run "pt info" raster info -f text pt.tif
run "pt convert tif" raster convert -q pt.tif pt2.tif
run "pt convert vrt" raster convert -q pt.tif pt.vrt
run "pt2 info" raster info -f text pt2.tif
run "pt2 convert again" raster convert -q pt2.tif pt3.tif

run "vrt gcplist parse" bash -c 'cat > gl.vrt <<EOF
<VRTDataset rasterXSize="3" rasterYSize="2">
  <GCPList Projection="EPSG:32611" dataAxisToSRSAxisMapping="1,2">
    <GCP Id="a" Info="hello" Pixel="0.5" Line="0.5" X="440720.5" Y="3751320.5"/>
    <GCP Id="b" Pixel="2.5" Line="1.5" X="440722.5" Y="3751318.5" Z="12"/>
  </GCPList>
  <VRTRasterBand dataType="Byte" band="1">
    <SimpleSource>
      <SourceFilename relativeToVRT="1">a.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>
EOF'
run "gcplist info text" raster info -f text gl.vrt
run "gcplist info json" raster info gl.vrt
run "gcplist convert vrt" raster convert -q gl.vrt gl2.vrt
run "gcplist convert tif" raster convert -q gl.vrt gl.tif
run "gcplist tif info" raster info -f text gl.tif
run "gcplist edit stats" raster edit gl.vrt --stats

run "domain order file" bash -c 'cat > dm.vrt <<EOF
<VRTDataset rasterXSize="3" rasterYSize="2">
  <Metadata domain="IMAGE_STRUCTURE">
    <MDI key="INTERLEAVE">BAND</MDI>
  </Metadata>
  <Metadata>
    <MDI key="ZZZ">9</MDI>
    <MDI key="AAA">2</MDI>
  </Metadata>
  <Metadata domain="other">
    <MDI key="K">v</MDI>
  </Metadata>
  <VRTRasterBand dataType="Byte" band="1">
    <SimpleSource>
      <SourceFilename relativeToVRT="1">a.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>
EOF'
run "domain hoist convert" raster convert -q dm.vrt dm2.vrt
run "domain hoist edit" raster edit dm.vrt --metadata NEW=x
run "domain hoist rewrite" raster edit dm.vrt --stats
run "domain unset" raster edit dm.vrt --unset-metadata AAA

run "create i16nd" raster create -q --size 3,2 --datatype Int16 --burn 5 --nodata 5 i16nd.tif
run "convert i16nd vrt" raster convert -q i16nd.tif i16nd.vrt
run "i16nd vrt stats fail" raster edit i16nd.vrt --stats
run "i16nd vrt hist fail" raster edit i16nd.vrt --hist
run "i16nd vrt info" raster info -f text i16nd.vrt

run "create m1" raster create -q --size 2,2 --burn 3 m1.tif
run "create m2" raster create -q --size 2,2 --burn 9 m2.tif
run "mosaic vrt file" bash -c 'cat > mo.vrt <<EOF
<VRTDataset rasterXSize="4" rasterYSize="2">
  <VRTRasterBand dataType="Byte" band="1">
    <SimpleSource>
      <SourceFilename relativeToVRT="1">m1.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="2" ySize="2" />
      <DstRect xOff="0" yOff="0" xSize="2" ySize="2" />
    </SimpleSource>
    <SimpleSource>
      <SourceFilename relativeToVRT="1">m2.tif</SourceFilename>
      <SourceBand>1</SourceBand>
      <SrcRect xOff="0" yOff="0" xSize="2" ySize="2" />
      <DstRect xOff="2" yOff="0" xSize="2" ySize="2" />
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>
EOF'
run "mosaic edit stats" raster edit mo.vrt --stats
run "mosaic edit stats rerun" raster edit mo.vrt --stats
run "mosaic edit hist" raster edit mo.vrt --hist

run "create big" raster create -q --size 100,200 --burn 7 big.tif
run "convert big vrt" raster convert -q big.tif big.vrt
run "big vrt nodata mismatch stats" raster edit big.vrt --nodata 9 --stats
run "big vrt stats rerun" raster edit big.vrt --stats
run "big vrt hist" raster edit big.vrt --hist

echo "$pass passed, $fail failed"
exit $((fail > 0))
