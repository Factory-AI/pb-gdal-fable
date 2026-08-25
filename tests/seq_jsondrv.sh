#!/bin/bash
# Sequential side-by-side harness: GeoJSONSeq / ESRIJSON / TopoJSON drivers.
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
mk() { # mk <file> ; content on stdin, written into both dirs
  local f="$1"
  cat > "$W/o/$f"
  cp "$W/o/$f" "$W/c/$f"
}

F1='{"type":"Feature","properties":{"a":1,"s":"x"},"geometry":{"type":"Point","coordinates":[1.123456789,2]}}'
F2='{"type":"Feature","properties":{"a":2,"s":"y"},"geometry":{"type":"LineString","coordinates":[[1,2],[3,4.5]]}}'

# ---------------- GeoJSONSeq reader ----------------
printf '%s\n%s\n' "$F1" "$F2" | mk a.geojsonl
run "seq info text" vector info -f text a.geojsonl
run "seq info json" vector info -f json a.geojsonl
run "seq info features" vector info --features a.geojsonl
run "seq info summary" vector info --summary a.geojsonl

printf '%s\n{"brokenA\n' "$F1" | mk d1.geojsonl
run "seq broken tail text" vector info -f text d1.geojsonl
run "seq broken tail features" vector info --features d1.geojsonl
run "seq broken tail summary" vector info --summary d1.geojsonl
run "seq broken tail fid0" vector info --fid 0 d1.geojsonl
run "seq broken tail limit1" vector info --features --limit 1 d1.geojsonl
run "seq broken tail limit0" vector info --features --limit 0 d1.geojsonl

printf '%s\n{"brokenA\n%s\n' "$F1" "$F2" | mk m1.geojsonl
run "seq broken middle features" vector info --features m1.geojsonl
run "seq broken middle fid1" vector info --fid 1 m1.geojsonl

printf '{"type":"Feature","properties":{"a":1},"geometry":{"type":"Point","coordinates":["x",2]}}\n{"brokenA\n%s\n' "$F2" | mk m2.geojsonl
run "seq badgeom text" vector info -f text m2.geojsonl
run "seq badgeom features" vector info --features m2.geojsonl

printf '%s\n{"foo":1} trailing\n' "$F1" | mk d5.geojsonl
run "seq nonfeature text counted" vector info -f text d5.geojsonl
printf '%s\nnotjson\n' "$F1" | mk d2.geojsonl
run "seq garbage line ->geojson" vector info -f text d2.geojsonl
printf '%s\n[1,2]\n' "$F1" | mk d3.geojsonl
run "seq array line ->geojson" vector info -f text d3.geojsonl
printf '%s\n{"type":"FeatureCollection","features":[]}\n' "$F1" | mk d4.geojsonl
run "seq fc second ->geojson fail" vector info -f text d4.geojsonl
printf '{"foo":1}\n%s\n' "$F1" | mk e1.geojsonl
run "seq nonfeature first" vector info -f text e1.geojsonl
printf '{"brokenA\n%s\n' "$F1" | mk e2.geojsonl
run "seq broken first" vector info -f text e2.geojsonl
printf '%s' "$F1" | mk f1.geojsonl
run "seq single feature ->geojson" vector info -f text f1.geojsonl

printf '\x1e%s\n' "$F1" | mk r1.geojsonl
run "rs single feature" vector info --features r1.geojsonl
printf '\x1e{"type":"FeatureCollection","features":[%s]}\n' "$F1" | mk r2.geojsonl
run "rs fc text counted only" vector info --features r2.geojsonl
printf '\x1e{"foo":1}\n' | mk r3.geojsonl
run "rs nonfeature" vector info -f text r3.geojsonl
printf '\x1e{"brokenA\n' | mk r4.geojsonl
run "rs broken" vector info -f text r4.geojsonl
printf '\x1e{"type":"FeatureCollection","features":[%s]}\n\x1e{"brokenB\n' "$F1" | mk r5.geojsonl
run "rs fc plus broken" vector info -f text r5.geojsonl

printf '{"type":"Point","coordinates":[1,2]}\n{"type":"Point","coordinates":[3,4]}\n' | mk g1.geojsonl
run "seq bare geometries" vector info --features g1.geojsonl
printf '{"type":"Point","coordinates":[1,2]}\n%s\n' "$F2" | mk g2.geojsonl
run "seq geometry plus feature" vector info --features g2.geojsonl

printf '{"type":"Feature","id":5,"properties":{"n":"x"},"geometry":null}\n{"type":"Feature","id":9,"properties":{"n":"y"},"geometry":null}\n' | mk ids.geojsonl
run "seq explicit int ids" vector info --features ids.geojsonl
printf '%s\n{"type":"Feature","id":"one","properties":{"b":2.5},"geometry":null}\n' "$F1" | mk sid.geojsonl
run "seq string id late field" vector info --features sid.geojsonl
run "seq string id json" vector info -f json sid.geojsonl

printf '%s\n%s\n' "$F1" "$F2" | mk t.txt
run "seq txt extension" vector info -f text t.txt

# ---------------- GeoJSON identify prefix rule ----------------
printf '{"brokenA' | mk i1.json
run "identify broken no marker" vector info -f text i1.json
printf '{"type": "Feature"\n' | mk i2.json
run "identify marker then continue" vector info -f text i2.json
printf '{"type":"Feature","brokenA\n{"foo":1}\n' | mk i3.json
run "identify marker before error" vector info -f text i3.json
printf '{"brokenA {"type":"Feature"}\n' | mk i4.json
run "identify marker after error" vector info -f text i4.json
printf '{"a": "b"\n"type": "Feature"}\n' | mk i5.json
run "identify marker second line" vector info -f text i5.json
printf '{"type":"FeatureCollection", "features":[{"brokenA\n' | mk i6.json
run "identify fc unterminated" vector info -f text i6.json

# ---------------- flat top-level attributes ----------------
printf '{"foo":1,"type":"Feature","bar":"x","geometry":{"type":"Point","coordinates":[1,2]},"id":7,"bbox":[1,2,1,2]}\n' | mk fa1.json
run "flat feature root" vector info --features fa1.json
printf '{"type":"FeatureCollection","features":[{"type":"Feature","foo":1,"geometry":null},{"type":"Feature","bar":"y","geometry":null}]}\n' | mk fa2.json
run "flat fc name tiebreak" vector info --features fa2.json
printf '{"type":"Feature","properties":null,"zed":3}\n' | mk fa3.json
run "flat null properties" vector info --features fa3.json
printf '{"type":"Feature","id":true,"foo":1}\n' | mk fa4.json
run "flat bool id" vector info --features fa4.json
printf '{"type":"FeatureCollection","features":[{"type":"Feature","properties":{"id":"p"},"foo":9},{"type":"Feature","id":3}]}\n' | mk fa5.json
run "flat id unifies props id" vector info --features fa5.json
printf '{"type":"FeatureCollection","features":[{"type":"Feature","id":"z","foo":1},{"type":"Feature","properties":{"id":5,"a":2}}]}\n' | mk fa6.json
run "flat id json subtype merge" vector info --features fa6.json

# ---------------- id coercion warnings ----------------
printf '{"type":"FeatureCollection","features":[{"type":"Feature","id":5,"foo":1},{"type":"Feature","id":"12ab","foo":2},{"type":"Feature","id":"42","foo":3}]}\n' | mk co1.json
run "coerce features" vector info --features co1.json
run "coerce text" vector info -f text co1.json
run "coerce summary" vector info --summary co1.json
run "coerce fid hit" vector info --fid 12 co1.json
run "coerce fid miss" vector info --fid 1 co1.json
run "coerce limit" vector info --features --limit 1 co1.json
run "coerce convert" vector convert -q co1.json co1out.json
printf '{"type":"FeatureCollection","features":[{"type":"Feature","properties":{"a":1},"id":5},{"type":"Feature","properties":{"a":2},"id":"12ab"}]}\n' | mk co2.json
run "coerce fid only" vector info --features co2.json

# ---------------- GeoJSONSeq writer ----------------
printf '{"type":"FeatureCollection","name":"src","features":[%s,%s]}' "$F1" "$F2" | mk in.json
run "write default" vector convert in.json w1.geojsonl
run "write geojsons rs" vector convert -q in.json w2.geojsons
run "write of flag" vector convert -q --of GeoJSONSeq in.json w3.dat
run "write rs lco" vector convert -q --lco RS=YES in.json w4.geojsonl
run "write bbox" vector convert -q --lco WRITE_BBOX=YES in.json w5.geojsonl
run "write id field" vector convert -q --lco ID_FIELD=a in.json w6.geojsonl
run "write id string" vector convert -q --lco ID_FIELD=a --lco ID_TYPE=String in.json w7.geojsonl
run "write id integer" vector convert -q --lco ID_FIELD=s --lco ID_TYPE=Integer in.json w8.geojsonl
run "write precision" vector convert -q --lco COORDINATE_PRECISION=2 in.json w9.geojsonl
run "write unsupported lco" vector convert -q --lco RFC7946=YES in.json w10.geojsonl
run "write exists" vector convert -q in.json w1.geojsonl
run "write overwrite" vector convert -q --overwrite in.json w1.geojsonl
run "write append newlayer" vector convert -q --append in.json w1.geojsonl
run "write update newlayer" vector convert -q --update in.json w1.geojsonl
run "write upsert" vector convert -q --upsert in.json w1.geojsonl
run "write missing dir" vector convert -q in.json nodir/w.geojsonl
run "of esrijson" vector convert -q --of ESRIJSON in.json we.x
run "of topojson" vector convert -q --of TopoJSON in.json wt.x

run "write src samename" vector convert -q in.json src.geojsonl
run "append samename" vector convert -q --append in.json src.geojsonl
run "append samename lco" vector convert -q --append --lco RS=YES in.json src.geojsonl
run "update samename" vector convert -q --update in.json src.geojsonl
run "overwrite-layer samename" vector convert -q --overwrite-layer in.json src.geojsonl

printf '{"type":"FeatureCollection","crs":{"type":"name","properties":{"name":"EPSG:32631"}},"features":[{"type":"Feature","properties":{"a":1},"geometry":{"type":"Point","coordinates":[500000,4649776]}}]}' | mk utm.json
run "write reprojects wgs84" vector convert -q utm.json wu.geojsonl
printf '{"type":"FeatureCollection","features":[{"type":"Feature","properties":{"a":1},"geometry":{"type":"Point","coordinates":[1,2,9]}}]}' | mk z.json
run "write keeps z" vector convert -q z.json wz.geojsonl
printf '{"type":"FeatureCollection","features":[]}' | mk empty.json
run "write empty" vector convert -q empty.json wy.geojsonl

run "copy trailing error json" vector convert d1.geojsonl c1.json
run "copy trailing error shp" vector convert -q d1.geojsonl c2.shp
run "copy trailing error skip" vector convert -q --skip-errors d1.geojsonl c3.json
run "copy middle error ok" vector convert -q m1.geojsonl c4.json
run "seq roundtrip" vector convert -q a.geojsonl c5.geojsonl

# ---------------- ESRIJSON reader ----------------
printf '%s' '{"objectIdFieldName":"OBJECTID","geometryType":"esriGeometryPoint","spatialReference":{"wkid":4326},"fields":[{"name":"OBJECTID","type":"esriFieldTypeOID"},{"name":"NAME","type":"esriFieldTypeString","alias":"Name","length":60},{"name":"POP","type":"esriFieldTypeInteger","alias":"Population"}],"features":[{"attributes":{"OBJECTID":1,"NAME":"alpha","POP":100},"geometry":{"x":1.5,"y":2.5}},{"attributes":{"OBJECTID":2,"NAME":"beta","POP":200},"geometry":{"x":3,"y":4}}]}' | mk es1.json
run "esri full text" vector info --features es1.json
run "esri full json" vector info -f json --features es1.json
run "esri full summary" vector info --summary es1.json
run "esri full autoinfo" info es1.json
run "esri convert" vector convert -q es1.json esc1.json

printf '%s' '{"geometryType":"esriGeometryPoint","fields":[{"name":"a","type":"esriFieldTypeSmallInteger"},{"name":"b","type":"esriFieldTypeBigInteger"},{"name":"c","type":"esriFieldTypeSingle"},{"name":"d","type":"esriFieldTypeDate"},{"name":"g","type":"esriFieldTypeGlobalID","length":38},{"name":"u","type":"esriFieldTypeSomething"}],"features":[{"attributes":{"a":70000,"b":"98765432109876","c":0.123456789,"d":1400000000123,"g":"guid","u":9},"geometry":{"x":1,"y":2}},{"attributes":{"a":-70000,"b":5,"c":0.1,"d":"2014-05-13T10:00:00Z","g":null,"u":true},"geometry":null}]}' | mk es2.json
run "esri types text" vector info --features es2.json
run "esri types json" vector info -f json --features es2.json
run "esri types convert" vector convert -q es2.json esc2.json

printf '%s' '{"fields":[{"name":"OBJECTID","type":"esriFieldTypeOID"},{"name":"t","type":"esriFieldTypeString"}],"features":[{"attributes":{"OBJECTID":2,"t":"f0"}},{"attributes":{"OBJECTID":1,"t":"f1"}},{"attributes":{"OBJECTID":2,"t":"f2"}},{"attributes":{"OBJECTID":9,"t":"f3"}},{"attributes":{"t":"f4"}},{"attributes":{"OBJECTID":null,"t":"f5"}},{"attributes":{"OBJECTID":3000000000,"t":"f6"}},{"attributes":{"OBJECTID":"7cd","t":"f7"}}]}' | mk es3.json
run "esri oid dedup order" vector info --features es3.json
run "esri oid dedup limit" vector info --features --limit 3 es3.json
run "esri oid fid lookup" vector info --fid 9 es3.json
run "esri oid convert" vector convert -q es3.json esc3.json

printf '%s' '{"features":[{"attributes":{"b":1,"a":2,"dt":"2014-05-13T10:00:00Z"}},{"attributes":{"c":true,"a":"x","n":null}}]}' | mk es4.json
run "esri attr schema" vector info --features es4.json
run "esri attr schema json" vector info -f json --features es4.json

printf '%s' '{"fieldAliases":{"a":"AA","b":"b","c":5,"d":""},"features":[{"attributes":{"a":1,"b":2.5}}]}' | mk es5.json
run "esri fieldaliases" vector info --features es5.json
run "esri fieldaliases json" vector info -f json --features es5.json

printf '%s' '{"geometryType":"esriGeometryPolygon","hasZ":true,"features":[{"attributes":{"a":1},"geometry":{"rings":[[[0,0],[0,10],[10,10],[10,0],[0,0]],[[2,2],[4,2],[4,4],[2,4],[2,2]],[[6,6],[7,6],[7,7],[6,7],[6,6]]]}},{"attributes":{"a":2},"geometry":{"rings":[[[0,0],[0,10],[10,10],[10,0],[0,0]],[[2,2],[8,2],[8,8],[2,8],[2,2]],[[4,4],[5,4],[5,5],[4,5],[4,4]]]}},{"attributes":{"a":3},"geometry":{"rings":[[[0,0,1],[0,10,1],[10,10,1],[10,0,1],[0,0,1]]]}},{"attributes":{"a":4},"geometry":{"rings":[[[0,0],[0,10],[10,10],[10,0]]]}}]}' | mk es6.json
run "esri rings organize" vector info --features es6.json
run "esri rings convert" vector convert -q es6.json esc6.json

printf '%s' '{"geometryType":"esriGeometryPolyline","hasM":true,"features":[{"attributes":{"a":1},"geometry":{"paths":[[[1,2,3,9],[4,5,6,9]]]}},{"attributes":{"a":2},"geometry":{"paths":[[[1,2],[3,4]],[[5,6],[7,8]]]}},{"attributes":{"a":3},"geometry":{"paths":[[["1","2"],[3,4]]]}}]}' | mk es7.json
run "esri paths zm" vector info --features es7.json
run "esri paths zm convert" vector convert es7.json esc7.json

printf '%s' '{"geometryType":"esriGeometryMultipoint","features":[{"attributes":{"a":1},"geometry":{"points":[[1,2],[3,4,5]]}},{"attributes":{"a":2},"geometry":{"x":"NaN","y":2}}]}' | mk es8.json
run "esri multipoint" vector info --features es8.json

printf '%s' '{"geometryType":"esriGeometryEnvelope","spatialReference":{"wkid":102100},"features":[{"attributes":{"a":1},"geometry":{"xmin":0,"ymin":0,"xmax":1,"ymax":1}}]}' | mk es9.json
run "esri envelope 102100" vector info --features es9.json
run "esri envelope convert" vector convert -q es9.json esc9.json

printf '%s' '{"spatialReference":{"wkid":999999},"features":[{"attributes":{"a":1}}]}' | mk es10.json
run "esri invalid wkid" vector info es10.json
printf '%s' '{"spatialReference":{"latestWkid":3857,"wkid":102100},"features":[]}' | mk es11.json
run "esri latestwkid" vector info es11.json
printf '%s' '{"spatialReference":{"wkt":"GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\",0.0174532925199433]]"},"features":[]}' | mk es12.json
run "esri wkt sr" vector info es12.json

printf '%s' '{"fieldAliases":{ b' | mk es13.json
run "esri broken json" vector info es13.json
printf '%s' '{"spatialReference":{"wkid":4326},"fieldAliases":{}}' | mk es14.json
run "esri missing features" vector info es14.json
printf '%s' '{"fields":[{"name":"a"}],"features":[{"attributes":{"a":1}}]}' | mk es15.json
run "esri schema fail" vector info es15.json
run "esri schema fail autoinfo" info es15.json
printf '%s' '{"features":[{"attributes":{"a":1}}],"extra":"x' | mk es16.json
run "esri broken trailing" vector info es16.json
printf '%s' '{"features":[{"attributes":{"a":1}}]}' | mk es17.json
run "esri attrs only" vector info --features es17.json
run "esri attrs only autoinfo" info es17.json
printf '%s' '{"features":[{"geometry":null,"attributes":{"a":1}}]}' | mk es18.json
run "weak geojson claims" vector info es18.json
printf '%s' '{"features":[]}' | mk es19.json
run "weak features empty" vector info es19.json
printf '%s' '{"coordinates":[1,2]}' | mk es20.json
run "weak coordinates" vector info es20.json
printf '%s' '[{"type":"Feature","properties":{},"geometry":null}]' | mk es21.json
run "root array not recognized" vector info es21.json

printf '%s' '{"geometryType":"esriGeometryFoo","features":[{"attributes":{"a":1},"geometry":{"paths":[[[1,2],[3,4]]]}}]}' | mk es22.json
run "esri unknown geomtype infer" vector info --features es22.json

# ---------------- TopoJSON reader ----------------
printf '%s' '{"type":"Topology","objects":{"pts":{"type":"GeometryCollection","geometries":[{"type":"Point","coordinates":[10,20],"id":"a","properties":{"n":1}},{"type":"Point","coordinates":[30,40]}]}},"arcs":[]}' | mk tp1.json
run "topo gc layer text" vector info --features tp1.json
run "topo gc layer json" vector info -f json --features tp1.json
run "topo gc layer summary" vector info --summary tp1.json
run "topo gc autoinfo" info tp1.json
run "topo convert" vector convert -q tp1.json tpc1.json

printf '%s' '{"type":"Topology","transform":{"scale":[0.5,2],"translate":[100,200]},"objects":{"lines":{"type":"GeometryCollection","geometries":[{"type":"LineString","arcs":[0]},{"type":"LineString","arcs":[-1]},{"type":"LineString","arcs":[0,0]},{"type":"LineString","arcs":[5]}]}},"arcs":[[[0,0],[10,5],[2,-3]]]}' | mk tp2.json
run "topo transform arcs" vector info --features tp2.json
run "topo transform convert" vector convert -q tp2.json tpc2.json

printf '%s' '{"type":"Topology","objects":{"o1":{"type":"Point","coordinates":[1,2]},"o2":{"type":"LineString","arcs":[0],"id":7,"properties":{"p":1}},"gc":{"type":"GeometryCollection","geometries":[{"type":"Point","coordinates":[5,6]}]},"gce":{"type":"GeometryCollection","geometries":[]}},"arcs":[[[0,0],[1,1]]]}' | mk tp3.json
run "topo grouped layers" vector info --features tp3.json
run "topo grouped json" vector info -f json --features tp3.json
run "topo grouped select layer" vector convert -q --input-layer TopoJSON tp3.json tpc3.json
run "topo multilayer convert error" vector convert tp3.json tpc3b.json

printf '%s' '{"type":"Topology","arcs":[[[0,0],[10,0],[10,10]],[[10,10],[0,10],[0,0]],[[0,0],[0,20],[20,20],[20,0],[0,0]],[[5,5],[8,5],[8,8],[5,8],[5,5]],[[0,0],[5,5]]],"objects":{"polys":{"type":"GeometryCollection","geometries":[{"type":"Polygon","arcs":[[0,1]]},{"type":"Polygon","arcs":[[2],[3]]},{"type":"Polygon","arcs":[[4]]},{"type":"MultiPolygon","arcs":[[[4]],[[2]]]},{"type":"MultiLineString","arcs":[[0],[-1],[9]]},{"type":"MultiPoint","coordinates":[[1,2],["x",2],[3]]}]}}}' | mk tp4.json
run "topo polys rings" vector info --features tp4.json
run "topo polys json" vector info -f json --features tp4.json
run "topo polys convert" vector convert -q tp4.json tpc4.json

printf '%s' '{"type":"Topology","arcs":[],"objects":{"gc":{"type":"GeometryCollection","geometries":[{"type":"Point","coordinates":[1,1],"id":5,"properties":{"q":1,"a":2}},{"type":"Point","coordinates":[2,2],"id":6.5,"properties":{"m":3}},{"type":"Point","coordinates":[1,2,3],"id":true},{"type":"Point","coordinates":[4,5],"id":null,"properties":{"id":9,"n":null,"d":"2014-05-13T10:11:12Z","b":true}},{"type":"GeometryCollection","geometries":[]},{"type":"Foo","coordinates":[3,3]},{"type":"Point"},{"type":"Point","coordinates":5}]}}}' | mk tp5.json
run "topo ids fields dag" vector info --features tp5.json
run "topo ids fields json" vector info -f json --features tp5.json
run "topo ids convert" vector convert -q tp5.json tpc5.json
run "topo fid lookup" vector info --fid 1 tp5.json
run "topo limit" vector info --features --limit 2 tp5.json

printf '%s' '{"type":"Topology","arcs":[],"crs":{"type":"name","properties":{"name":"EPSG:32631"}},"objects":{"gc":{"type":"GeometryCollection","geometries":[{"type":"Point","coordinates":[500000,4500000]}]}}}' | mk tp6.json
run "topo crs member" vector info tp6.json
run "topo crs convert" vector convert -q tp6.json tpc6.json

printf '%s' '{"type":"Topology","objects":{},"arcs":[]}' | mk tp7.json
run "topo empty objects" vector info tp7.json
printf '%s' '{"type":"Topology","objects":{"gc":{"type":"GeometryCollection","geometries":[{"type":"Point","coordinates":[1,1]}]}}}' | mk tp8.json
run "topo missing arcs" vector info tp8.json
printf '%s' '{"type":"Topology","objects":{' | mk tp9.json
run "topo broken json" vector info tp9.json
run "topo broken autoinfo" info tp9.json
printf '%s' '{"type":"Topology","transform":{"scale":[0.5],"translate":[10]},"arcs":[[[2,2],[2,2]]],"objects":{"gc":{"type":"GeometryCollection","geometries":[{"type":"LineString","arcs":[0]}]}}}' | mk tp10.json
run "topo invalid transform" vector info --features tp10.json
printf '%s' '{"type":"Topology","transform":{"scale":["0.5","2"],"translate":[10,10]},"arcs":[[[2,2],[2,2]]],"objects":{"gc":{"type":"GeometryCollection","geometries":[{"type":"LineString","arcs":[0]}]}}}' | mk tp11.json
run "topo string scale" vector info --features tp11.json

echo "$pass passed, $fail failed"
exit $((fail > 0))
