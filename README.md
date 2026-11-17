# tsf-geospatial

Geospatial testing for the Test Environment (TE): geometry processing,
coordinate reference systems and transformations, spatial analysis,
vector and raster datasets, geolocation tracking, OGC services and
PostGIS, packaged as an external TE repository.

All the processing happens on a Test Agent, in its RPC server, through
the C APIs of GEOS (geometry), PROJ (reference systems, geodesics) and
GDAL/OGR (datasets, rasters, OGC clients). The engine needs none of
them. Geometries, datasets and tracks stay on the agent; a test holds
handles to them and gets back results and bounded reports, so a
dataset far larger than the engine's memory is validated in one
streaming pass on the agent.

## Libraries

| Library | Runs on | What it is |
|---|---|---|
| `ta_geospatial` | Test Agent | The engine: plain C over GEOS, PROJ and GDAL. Its core (`ta_geo_core.h`) includes no TE header and is tested on its own (`ci/`). |
| `rpcs_geospatial` | Agent RPC server | The `geo_*` RPCs (`geospatial.x.m4`, program 47). |
| `tapi_geospatial` | Test Engine | The TAPI: geometries, CRS, datasets, rasters, tracks, generation, OGC, assertions with TE verdicts. |
| `tapi_geospatial_postgis` | Test Engine | PostGIS checks over [tsf-sql](https://github.com/interpretica-io/tsf-sql) connections. Optional. |

## What it does

- **Geometry**: WKT, WKB, EWKB and GeoJSON in and out, every type, XY,
  XYZ, XYM and XYZM (M needs GEOS 3.12; GeoJSON carries Z, not M).
  DE-9IM predicates, overlay, buffer, simplification, derived
  geometries. Validation that names the defect (self-intersection,
  ring self-intersection, hole outside the shell, nested holes,
  disconnected interior, unclosed ring, degenerate, non-finite,
  duplicate vertex, empty, orientation, bounds, dimensionality) and
  its location, and never repairs; repair is a separate, explicit call
  that returns a copy.
- **Measurements** that always say how they were made: `planar` (in
  the CRS's own unit, and flagged when that unit is the degree) or
  `geodesic` (metres on the datum's ellipsoid, Karney's algorithms).
  Planar measures of geographic coordinates are refused when the
  caller needs metres.
- **Reference systems**: lookup (type, axes, unit, datum, area of
  use), validation, comparison (equivalent, axis-swapped, same datum).
  Transformations of points, geometries (Z transformed, M carried) and
  boxes (densified), with the operation PROJ actually used, its
  published accuracy, its grids and whether they are installed, whether
  it is a ballpark, and whether it fell back because a better operation
  needs a missing grid. Only a conversion within one datum is called
  exact. The axis order is always explicit - `gis` or `authority` -
  and a transformation that does not say which is refused. Inputs
  outside the areas of use are counted (or refused, strictly); results
  that are not numbers are refused.
- **Vector datasets** (GeoJSON, GeoJSONSeq, GeoPackage, Shapefile,
  FlatGeobuf, KML, GML, and whatever else the agent's GDAL reads):
  layers, schema, count, extent, CRS, bounded fetches, streaming
  validation (invalid, missing and empty geometries, duplicate ids, CRS
  mismatches and inconsistencies, out-of-range coordinates, schema
  mismatches, unexpected types, attribute ranges and allowed values),
  and comparison by identifier, independent of feature order, with
  geometry and attribute tolerances. Untrusted files do not choose their
  driver: VRT and other indirections are off unless the caller asks.
- **Rasters** (GeoTIFF, COG, PNG, MBTiles...): metadata, windowed
  reads, exact statistics and pixel-wise comparison, tile by tile.
- **Tracking**: normalised observations from text or datasets;
  ordering, gaps, missing samples, jumps, impossible speeds; error
  against a reference track (mean, max, RMSE, percentiles); distance
  to a route; geofences with entry and exit events.
- **Generation**: deterministic points, grids, star-shaped polygons
  (valid by construction, with holes), lines, trajectories with
  modelled receiver noise, jumps and drops, boundary points, outliers,
  and - only on request - invalid geometries of every category; and
  datasets of them with defects planted at known places.
- **OGC services** (WMS 1.3.0, WFS 2.0.0, OGC API - Features), checked
  from the agent: capabilities that parse, at the version asked for,
  with layers, CRS PROJ knows, geographic boxes on the Earth and
  per-CRS boxes that agree with them; GetMap images of the size and
  format asked for, not blank, not an exception sent with 200;
  features that are valid and inside the advertised extent; paging
  that returns the same features whatever the page size; access
  control that is enforced.
- **PostGIS** (with tsf-sql): spatial columns, stored invalid
  geometries with PostGIS's reason, SRID and type consistency, spatial
  index presence and use, predicates evaluated by the database and by
  GEOS side by side, stored geometries against expected ones.
- **Limits** on every input (vertices, nesting depth, input size,
  features per pass, live objects, pixels per read) enforced before the
  libraries allocate, a deadline per operation (GEOS interruption), and
  **performance** loops on the agent that separate the cold first call
  from the warm ones and report the input's complexity.

## Usage

Declare the repository in the external libraries catalog
(`conf/external.yml`), with tsf-sql first when the PostGIS library is
used:

```yaml
repositories:
  - name: tsf_sql
    url: https://github.com/interpretica-io/tsf-sql.git
    ref: main
    libs: [ ta_sql, rpcs_sql, tapi_sql ]
  - name: tsf_geospatial
    url: https://github.com/interpretica-io/tsf-geospatial.git
    ref: main
    libs:
      - ta_geospatial
      - rpcs_geospatial
      - tapi_geospatial
      - tapi_geospatial_postgis
```

Bind it in `builder.conf`: the libraries, the RPC definitions, and the
agent's RPC server application, which links the engine and the system
libraries it needs (as pkg-config names):

```
TE_EXT_REPO_USE([tsf_geospatial], [],
                [ta_geospatial rpcs_geospatial tapi_geospatial])

TE_LIB_PARMS([rpcxdr], [], [],
             [--with-rpcdefs=tarpc_job.x.m4,../rpcs_geospatial/geospatial.x.m4])

TE_TA_APP([ta_rpcprovider], [], [linux], [ta_rpcprovider], [], [],
          [ta_geospatial rpcs_geospatial conf_oid ta_job comm_net_agent \
           rpcs_job rpcserver agentlib rpcxdrta rpc_types rpctransport \
           loggerta tools logger_core netconf],
          [\${EXT_SOURCES}/build.sh --extra-deps='libpcre2-8 geos proj gdal libcurl'],
          [ta_rpcs], [])
```

Then add `tapi_geospatial` to `te_libs` in the suite's `meson.build`.
[tsf-geospatial-ts](https://github.com/interpretica-io/tsf-geospatial-ts)
is a complete example.

```c
tapi_geo_geometry *point;
tapi_geo_geometry *region;
bool inside;

CHECK_RC(tapi_geo_parse_wkt(rpcs, "POINT (-8.61 41.15)", 4326, &point));
CHECK_RC(tapi_geo_parse_wkt(rpcs, region_wkt, 4326, &region));
CHECK_RC(tapi_geo_within(rpcs, point, region, &inside));
if (!inside)
    TEST_VERDICT("Point is outside expected region");

tapi_geo_geometry_free(rpcs, point);
tapi_geo_geometry_free(rpcs, region);
```

Assertions (`tapi_geo_assert.h`) log the evidence as `key: value`
lines, emit a verdict whose text carries names, never measured numbers
(so TRC can match it), and return `TE_EFAIL`; `TAPI_GEO_ASSERT()` stops
the test:

```c
TAPI_GEO_ASSERT(tapi_geo_assert_distance_le(rpcs, fix, depot,
                                            TAPI_GEO_GEODESIC,
                                            TAPI_GEO_AXIS_GIS, 50,
                                            "delivery position"));
```

## Requirements

On the agent: GEOS >= 3.12, PROJ >= 9.1 with its data, GDAL >= 3.6
with its data (Ubuntu 24.04: `libgeos-dev libproj-dev proj-data
libgdal-dev gdal-data`), and libcurl for the OGC checks (optional). On
Ubuntu 24.04 `libmariadb-dev` conflicts with `libgdal-dev`; tsf-sql's
MariaDB driver is then left out. The engine needs nothing beyond TE.

Runtime discovery: `tapi_geo_available()` returns the library versions
and feature bits (M coordinates, network grids, libcurl), and
`tapi_geo_driver_supported()` says whether a GDAL driver can read or
write; a test of something the agent lacks skips.

## Development

`ci/core-test.sh` builds the core with ASan/UBSan and runs
`ci/core_test.c`, contract tests against published references (the
worked examples of IOGP Guidance Note 7-2, GeographicLib's GeodSolve
and Planimeter examples) and closed forms, in about ten seconds.
`ci/geo_cli` runs one engine operation from the command line - for
operations that need no handle (generation, CRS, coordinates, OGC
checks) - and is the entry point for coverage-guided fuzzing with
tsf-fuzz. The RPC boilerplate is generated from one list:
`python3 ci/gen-rpc.py`.

## License

MIT. See [LICENSE](LICENSE).
