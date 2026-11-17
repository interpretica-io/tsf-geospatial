# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# External TE library (see TE_EXT_REPO): PostGIS checks over tsf-sql
# connections, against geometries of the geospatial engine. Optional:
# build it only when tsf-sql is declared as well (its tapi_sql library
# must come first in the libraries the Builder makes).

if get_option('engine')
    build_lib_shared = true
endif
build_lib_static = false
install_lib = install_dev

headers += files(
    'tapi_geo_postgis.h',
)

sources += files(
    'tapi_geo_postgis.c',
)

te_libs += [
    'tapi_geospatial',
    'tapi_sql',
    'tapi',
    'tapi_rpc',
    'rcfrpc',
    'rpcxdr',
    'logger_core',
    'tools',
]
