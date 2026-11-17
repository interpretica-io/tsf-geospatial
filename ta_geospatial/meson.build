# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# External TE library (see TE_EXT_REPO): agent-side geospatial engine.
# Plain C over the C APIs of GEOS (geometry), PROJ (reference systems,
# geodesics) and GDAL/OGR (datasets, rasters, OGC clients), with libcurl
# for OGC requests when present. It is linked into and called in-process
# by the agent's RPC server: nothing is spawned, and no geometry or
# dataset leaves the agent but results and bounded reports.

headers += files(
    'ta_geo.h',
    'ta_geo_core.h',
)

sources += files(
    'ta_geo.c',
    'ta_geo_util.c',
    'ta_geo_core.c',
    'ta_geo_geom.c',
    'ta_geo_geod.c',
    'ta_geo_crs.c',
    'ta_geo_dataset.c',
    'ta_geo_gen.c',
    'ta_geo_raster.c',
    'ta_geo_track.c',
    'ta_geo_ogc.c',
    'ta_geo_perf.c',
)

te_libs += [
    'logger_core',
    'tools',
]

cc = meson.get_compiler('c')

# Versions this code is written against; older ones lack the API it
# calls (GEOS M coordinates and GeoJSON, PROJ last-used operation and
# ONLY_BEST, GDAL ExportToIsoWkb and FlatGeobuf).
geos_dep = dependency('geos', version: '>=3.12', required: false)
if not geos_dep.found()
    # GEOS before 3.12 ships no geos.pc everywhere; geos-config does.
    geos_config = find_program('geos-config', required: true)
    geos_ver = run_command(geos_config, '--version', check: true).stdout().strip()
    if geos_ver.version_compare('<3.12')
        error('GEOS >= 3.12 is required, found ' + geos_ver)
    endif
    geos_dep = declare_dependency(
        compile_args: run_command(geos_config, '--cflags',
                                  check: true).stdout().strip().split(),
        link_args: run_command(geos_config, '--clibs',
                               check: true).stdout().strip().split())
endif

deps += [ geos_dep ]
deps += [ dependency('proj', version: '>=9.1') ]
deps += [ dependency('gdal', version: '>=3.6') ]
deps += [ cc.find_library('m'), dependency('threads') ]

curl_dep = dependency('libcurl', required: false)
if curl_dep.found()
    deps += [ curl_dep ]
    deps += declare_dependency(compile_args: [ '-DTA_GEO_HAVE_CURL' ])
endif
