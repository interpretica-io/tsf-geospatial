/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: reference systems and transformations
 *
 * CRS are given as anything PROJ reads: "EPSG:4326", a bare EPSG code,
 * WKT2, a PROJ string, "OGC:CRS84", "urn:ogc:def:crs:EPSG::3857".
 *
 * Every transformation reports the operation PROJ actually used and
 * what is known of its accuracy (#tapi_geo_xform_info). It is "exact"
 * only for a conversion without a datum change; a result is never
 * called exact just because a number came back.
 */

#ifndef __TAPI_GEO_CRS_H__
#define __TAPI_GEO_CRS_H__

#include "tapi_geo.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What a CRS is. */
typedef struct tapi_geo_crs_info {
    char name[128];
    char type[32];          /**< geographic2d, projected, compound... */
    char auth[16];
    char code[16];
    bool deprecated;
    bool geographic;
    bool lat_first;         /**< authority order starts with latitude
                                 (or northing) */
    char unit[64];          /**< of the first axis */
    unsigned int axes;
    char axis[3][96];       /**< "name|abbrev|direction|unit" */
    char datum[128];
    char ellipsoid[64];
    double semi_major;
    double inv_flattening;
    tapi_geo_bbox area_of_use;  /**< lon/lat degrees; NAN when none */
    char area_name[128];
} tapi_geo_crs_info;

extern te_errno tapi_geo_crs_lookup(rcf_rpc_server *rpcs, const char *crs,
                                    tapi_geo_crs_info *info,
                                    te_string *wkt2);

/**
 * Validate a definition. A definition PROJ does not accept is a
 * result (@p valid = @c false with @p reason), not a failure.
 */
extern te_errno tapi_geo_crs_validate(rcf_rpc_server *rpcs, const char *crs,
                                      bool *valid, te_string *reason);

/** How two CRS relate. */
typedef struct tapi_geo_crs_cmp {
    bool strict;                    /**< identical definitions */
    bool equivalent;                /**< same coordinates */
    bool equivalent_except_axis;    /**< same but for the axis order */
    bool axis_swapped;
    bool same_geodetic;             /**< same datum */
} tapi_geo_crs_cmp;

extern te_errno tapi_geo_crs_compare(rcf_rpc_server *rpcs, const char *a,
                                     const char *b, tapi_geo_crs_cmp *cmp);

/** What a transformation did. */
typedef struct tapi_geo_xform_info {
    char src[128];
    char dst[128];
    char axis[16];
    char operation[256];    /**< PROJ's name of the operation used */
    char operation_id[64];  /**< "EPSG:1314", or "" */
    double accuracy;        /**< metres; < 0 when unknown */
    bool ballpark;          /**< a ballpark (no datum shift) */
    bool exact;             /**< a pure conversion */
    bool fallback;          /**< a better operation needs a missing grid */
    char grids[256];        /**< grids used ("name(missing)") */
    unsigned int grids_missing;
    unsigned int candidates;
    double best_accuracy;   /**< best of all candidates; < 0 unknown */
    unsigned int out_of_domain; /**< inputs outside the areas of use */
    unsigned int nonfinite;     /**< outputs that are not numbers */
} tapi_geo_xform_info;

/** Options of a transformation. */
typedef struct tapi_geo_xform_opts {
    bool allow_ballpark;    /**< default true */
    bool only_best;         /**< refuse anything but the best operation */
    double accuracy;        /**< required accuracy, m; < 0 = any */
    bool strict_domain;     /**< inputs outside the area of use fail */
    bool allow_partial;     /**< geometry: keep non-finite results */
} tapi_geo_xform_opts;

#define TAPI_GEO_XFORM_OPTS_DEFAULT \
    { .allow_ballpark = true, .only_best = false, .accuracy = -1,   \
      .strict_domain = false, .allow_partial = false }

/**
 * Transform a geometry into @p dst. @p src overrides the geometry's own
 * CRS (@c NULL: the geometry's). Z is transformed, M carried.
 */
extern te_errno tapi_geo_transform(rcf_rpc_server *rpcs,
                                   const tapi_geo_geometry *geom,
                                   const char *src, const char *dst,
                                   tapi_geo_axis axis,
                                   const tapi_geo_xform_opts *opts,
                                   tapi_geo_geometry **out,
                                   tapi_geo_xform_info *info);

/**
 * Transform coordinates: @p n points of @p dims (2 or 3) doubles each
 * from @p in to @p out (may be the same array).
 */
extern te_errno tapi_geo_transform_coords(rcf_rpc_server *rpcs,
                                          const char *src, const char *dst,
                                          tapi_geo_axis axis,
                                          const tapi_geo_xform_opts *opts,
                                          const double *in, unsigned int n,
                                          unsigned int dims, double *out,
                                          tapi_geo_xform_info *info);

/** Transform a bounding box, densifying its edges. */
extern te_errno tapi_geo_transform_bounds(rcf_rpc_server *rpcs,
                                          const char *src, const char *dst,
                                          tapi_geo_axis axis,
                                          const tapi_geo_bbox *in,
                                          tapi_geo_bbox *out,
                                          tapi_geo_xform_info *info);

/** Log an operation's metadata at RING. */
extern void tapi_geo_xform_info_log(const tapi_geo_xform_info *info);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_GEO_CRS_H__ */
