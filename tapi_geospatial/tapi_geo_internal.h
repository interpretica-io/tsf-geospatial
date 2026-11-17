/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: internals shared by its units (not installed)
 */

#ifndef __TAPI_GEO_INTERNAL_H__
#define __TAPI_GEO_INTERNAL_H__

#include "tapi_geo.h"

/** Copy a result value into a fixed buffer ("" when absent). */
#define TGEO_STR(_res, _key, _buf) \
    do {                                                              \
        if (tapi_geo_res_str((_res), (_key), (_buf),                  \
                             sizeof(_buf)) == NULL)                   \
            (_buf)[0] = '\0';                                         \
    } while (0)

/** Wrap a new handle from a result into a geometry. */
extern te_errno tgeo_wrap(rcf_rpc_server *rpcs, const tapi_geo_res *res,
                          tapi_geo_geometry **out);

/** The handle of a geometry, refusing one from another RPC server. */
extern te_errno tgeo_handle(rcf_rpc_server *rpcs,
                            const tapi_geo_geometry *g, int *h);

/** Parse a validity result and its report. */
extern void tgeo_validity_read(tapi_geo_res *res, tapi_geo_validity *v);

#endif /* !__TAPI_GEO_INTERNAL_H__ */
