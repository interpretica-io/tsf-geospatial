/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial engine: the plain-C core
 *
 * Everything tsf-geospatial does on a Test Agent, with no Test
 * Environment header: GEOS (geometry), PROJ (reference systems,
 * transformations, geodesics) and GDAL/OGR (vector and raster datasets,
 * OGC services) through their C APIs. ta_geo.c puts the TE face on it;
 * ci/core_test.c drives it directly, with the sanitizers, off TE.
 *
 * One entry point, ta_geo_call(), runs one operation. Objects the
 * operations create (geometries, datasets, tracks) stay here, in the
 * process, and the caller holds integer handles to them. A handle names
 * its slot and the slot's generation, so a released handle is refused
 * rather than silently meaning the next object put there.
 *
 * Text in, text out: options as "key=value" lines, a result as
 * "key=value" lines, a report as newline-separated records of
 * tab-separated, percent-escaped fields. Real numbers are printed with
 * 17 significant digits, so they read back exactly.
 */

#ifndef __TA_GEO_CORE_H__
#define __TA_GEO_CORE_H__

#ifdef __cplusplus
extern "C" {
#endif

/** Return codes of the core. */
enum {
    TA_GEO_OK = 0,
    TA_GEO_EINVAL,      /**< Bad argument, unknown option, wrong handle. */
    TA_GEO_ENOENT,      /**< No such file, layer, feature, CRS. */
    TA_GEO_EIO,         /**< I/O or service failure. */
    TA_GEO_ECORRUPT,    /**< Input does not parse. */
    TA_GEO_E2BIG,       /**< A configured limit was exceeded. */
    TA_GEO_ENOMEM,      /**< Out of memory. */
    TA_GEO_ENOTSUP,     /**< Not supported here (driver, dimension...). */
    TA_GEO_ETIMEDOUT,   /**< The operation ran past its deadline. */
    TA_GEO_EDOM,        /**< Outside the domain (CRS area, non-finite). */
    TA_GEO_EACCES,      /**< A service refused the credentials. */
    TA_GEO_EFAIL,       /**< Anything else. */
};

/** Operations. Wire values: the RPCs map onto them one to one. */
typedef enum ta_geo_op {
    TA_GEO_OP_AVAILABLE = 0,
    TA_GEO_OP_CONFIG,
    TA_GEO_OP_STATS,
    TA_GEO_OP_RELEASE,
    TA_GEO_OP_GEOM_PARSE,
    TA_GEO_OP_GEOM_INFO,
    TA_GEO_OP_GEOM_SERIALIZE,
    TA_GEO_OP_GEOM_DERIVE,
    TA_GEO_OP_GEOM_CHECK,
    TA_GEO_OP_GEOM_REPAIR,
    TA_GEO_OP_GEOM_PREDICATE,
    TA_GEO_OP_GEOM_CONSTRUCT,
    TA_GEO_OP_GEOM_MEASURE,
    TA_GEO_OP_CRS_INFO,
    TA_GEO_OP_TRANSFORM,
    TA_GEO_OP_TRANSFORM_COORDS,
    TA_GEO_OP_TRANSFORM_BOUNDS,
    TA_GEO_OP_DS_OPEN,
    TA_GEO_OP_DS_INFO,
    TA_GEO_OP_DS_FETCH,
    TA_GEO_OP_DS_VALIDATE,
    TA_GEO_OP_DS_COMPARE,
    TA_GEO_OP_DS_WRITE,
    TA_GEO_OP_RASTER_INFO,
    TA_GEO_OP_RASTER_READ,
    TA_GEO_OP_RASTER_STATS,
    TA_GEO_OP_RASTER_COMPARE,
    TA_GEO_OP_RASTER_WRITE,
    TA_GEO_OP_TRACK_LOAD,
    TA_GEO_OP_TRACK_EVAL,
    TA_GEO_OP_GEN,
    TA_GEO_OP_OGC,
    TA_GEO_OP_PERF,
    TA_GEO_OP__COUNT,
} ta_geo_op;

/** Feature bits of TA_GEO_OP_AVAILABLE. */
enum {
    TA_GEO_FEAT_GEOS = 1 << 0,      /**< GEOS geometry engine. */
    TA_GEO_FEAT_PROJ = 1 << 1,      /**< PROJ. */
    TA_GEO_FEAT_GDAL = 1 << 2,      /**< GDAL/OGR. */
    TA_GEO_FEAT_M = 1 << 3,         /**< M coordinates (GEOS >= 3.12). */
    TA_GEO_FEAT_GEOJSON = 1 << 4,   /**< GEOS GeoJSON reader. */
    TA_GEO_FEAT_NETWORK = 1 << 5,   /**< PROJ network grids enabled. */
    TA_GEO_FEAT_HTTP = 1 << 6,      /**< GDAL built with curl (OGC). */
    TA_GEO_FEAT_CURL = 1 << 7,      /**< libcurl: OGC requests, HTTP codes. */
};

/**
 * Run one operation.
 *
 * @param op        What to do.
 * @param ha        First handle (most operations), or 0.
 * @param hb        Second handle (binary operations), or 0.
 * @param kind      Sub-operation ("intersects", "wkt", ...), or "".
 * @param text      Input text: WKT, GeoJSON, a path, a CRS, a URL.
 * @param params    Options, "key=value" lines.
 * @param handle    New handle, when the operation makes an object.
 * @param features  Feature bits (TA_GEO_OP_AVAILABLE).
 * @param result    "key=value" lines (heap, never NULL; caller frees).
 * @param report    Records (heap, never NULL; caller frees).
 * @param errmsg    Why it failed (heap, never NULL; caller frees).
 *
 * @return TA_GEO_OK or a TA_GEO_E* code.
 */
extern int ta_geo_call(ta_geo_op op, int ha, int hb, const char *kind,
                       const char *text, const char *params, int *handle,
                       unsigned int *features, char **result, char **report,
                       char **errmsg);

/** Name of an operation ("geom_parse"), or NULL. */
extern const char *ta_geo_op_name(ta_geo_op op);

/** Name of a return code ("EINVAL"). */
extern const char *ta_geo_strerror(int rc);

/** Release every object and the library contexts (leak checking). */
extern void ta_geo_shutdown(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TA_GEO_CORE_H__ */
