#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved.
#
# Build the tsf-geospatial core (ta_geospatial/ta_geo_*.c except the TE
# glue ta_geo.c), ci/core_test.c and ci/geo_cli.c without Test Environment, with
# AddressSanitizer and UndefinedBehaviorSanitizer. GEOS, PROJ and GDAL
# come from pkg-config / geos-config; libcurl is used when present.
# Output: ${OUT:-build-ci}/core_test and geo_cli.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${OUT:-build-ci}"
SAN="${SAN--fsanitize=address,undefined -fno-omit-frame-pointer}"
mkdir -p "$OUT"
CFLAGS="-std=c11 -D_GNU_SOURCE -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror \
        -Wno-unused-parameter $SAN $(geos-config --cflags) \
        $(pkg-config --cflags proj gdal)"
LIBS="$(geos-config --clibs) $(pkg-config --libs proj gdal) -lm -lpthread"
if pkg-config --exists libcurl; then
    CFLAGS="$CFLAGS -DTA_GEO_HAVE_CURL $(pkg-config --cflags libcurl)"
    LIBS="$LIBS $(pkg-config --libs libcurl)"
fi
objs=()
for f in ta_geospatial/ta_geo_{util,core,geom,geod,crs,dataset,gen,raster,track,ogc,perf}.c; do
    o="$OUT/$(basename "${f%.c}").o"
    cc $CFLAGS -Ita_geospatial -c "$f" -o "$o"
    objs+=("$o")
done
for t in core_test geo_cli; do
    cc $CFLAGS -Ita_geospatial -c "ci/$t.c" -o "$OUT/$t.o"
    cc $SAN "${objs[@]}" "$OUT/$t.o" $LIBS -o "$OUT/$t"
done
echo "built $OUT/core_test $OUT/geo_cli"
