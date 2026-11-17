# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# External TE library (see TE_EXT_REPO): engine-side geospatial TAPI.
# Built as a shared library on the engine platform; it needs no GEOS,
# PROJ or GDAL - everything runs on the agent. It needs the geo_* RPCs
# in rpcxdr, so add ../rpcs_geospatial/geospatial.x.m4 to the rpcxdr
# definitions of the engine platform.

if get_option('engine')
    build_lib_shared = true
endif
# Engine-side only: it calls tapi and tapi_rpc, which have no static
# agent variant.
build_lib_static = false
install_lib = install_dev

headers += files(
    'tapi_geo.h',
    'tapi_geo_assert.h',
    'tapi_geo_crs.h',
    'tapi_geo_dataset.h',
    'tapi_geo_ogc.h',
    'tapi_geo_raster.h',
    'tapi_geo_rpc.h',
    'tapi_geo_rpc_ops.h',
    'tapi_geo_track.h',
)

# tapi_geo_internal.h and tapi_geo_rpc_gen.h are internal.
sources += files(
    'tapi_geo.c',
    'tapi_geo_assert.c',
    'tapi_geo_crs.c',
    'tapi_geo_dataset.c',
    'tapi_geo_ogc.c',
    'tapi_geo_rpc.c',
    'tapi_geo_track.c',
)

te_libs += [
    'tapi',
    'tapi_rpc',
    'rcfrpc',
    'confapi',
    'rcfapi',
    'rpcxdr',
    'logger_core',
    'tools',
]
