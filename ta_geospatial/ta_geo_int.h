/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial engine: internals shared by the core's units
 *
 * Not installed. No Test Environment header here either: the core
 * builds without TE (ci/build-core.sh).
 */

#ifndef __TA_GEO_INT_H__
#define __TA_GEO_INT_H__

#define GEOS_USE_ONLY_R_API
#include <geos_c.h>
#include <proj.h>
#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>
#include <cpl_error.h>
#include <cpl_conv.h>
#include <cpl_string.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ta_geo_core.h"

#if defined(__GNUC__)
#define GEO_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define GEO_PRINTF(a, b)
#endif

#define GEOS_AT_LEAST(maj, min) \
    (GEOS_VERSION_MAJOR > (maj) || \
     (GEOS_VERSION_MAJOR == (maj) && GEOS_VERSION_MINOR >= (min)))

/* ---- growing string --------------------------------------------- */

typedef struct sbuf {
    char   *p;
    size_t  len;
    size_t  cap;
    bool    oom;
} sbuf;

#define SBUF_INIT { NULL, 0, 0, false }

extern void sb_printf(sbuf *b, const char *fmt, ...) GEO_PRINTF(2, 3);
extern void sb_vprintf(sbuf *b, const char *fmt, va_list ap);
extern void sb_put(sbuf *b, const char *s, size_t n);
extern void sb_puts(sbuf *b, const char *s);
/** Append a percent-escaped report field (%, TAB, LF, CR escaped). */
extern void sb_field(sbuf *b, const char *s);
/** Append a real number that reads back exactly ("%.17g", nan, inf). */
extern void sb_num(sbuf *b, double v);
/** "key=value\n" with a number. */
extern void sb_kv_num(sbuf *b, const char *key, double v);
extern void sb_kv_int(sbuf *b, const char *key, long long v);
/** "key=value\n"; newlines in value are replaced by spaces. */
extern void sb_kv_str(sbuf *b, const char *key, const char *v);
/** Take the string (never NULL) and reset the buffer. */
extern char *sb_take(sbuf *b);
extern void sb_free(sbuf *b);

/* ---- options ---------------------------------------------------- */

typedef struct kv_pair {
    char *key;
    char *val;
    bool  used;
} kv_pair;

typedef struct kv {
    kv_pair *v;
    size_t   n;
} kv;

/** Parse "key=value" lines; EINVAL on a line without '='. */
extern int kv_parse(const char *text, kv *out, char **err);
extern void kv_free(kv *k);
/** The value or NULL; marks the key used. */
extern const char *kv_get(kv *k, const char *key);
extern const char *kv_str(kv *k, const char *key, const char *def);
/** Numbers: EINVAL with a reason when the value is not one. */
extern int kv_dbl(kv *k, const char *key, double def, double *out,
                  char **err);
extern int kv_long(kv *k, const char *key, long long def, long long *out,
                   char **err);
extern int kv_bool(kv *k, const char *key, bool def, bool *out, char **err);
/** EINVAL naming the first option nobody read: no option is ignored. */
extern int kv_unused(kv *k, char **err);

/* ---- errors ------------------------------------------------------ */

/** Set *err (once) printf-style; returns rc for "return seterr(...)". */
extern int seterr(char **err, int rc, const char *fmt, ...)
    GEO_PRINTF(3, 4);

/* ---- library contexts -------------------------------------------- */

extern GEOSContextHandle_t geo_geos(void);
/** Last GEOS error message ("" when none); cleared by geo_geos_clear. */
extern const char *geo_geos_err(void);
extern void geo_geos_clear(void);
extern PJ_CONTEXT *geo_proj(void);
/** Last PROJ error text of the context. */
extern const char *geo_proj_err(void);
/** Last GDAL/CPL error message; cleared by geo_gdal_clear. */
extern const char *geo_gdal_err(void);
extern void geo_gdal_clear(void);
extern void geo_gdal_init(void);

/* ---- limits and deadline ---------------------------------------- */

typedef struct geo_limits {
    long long max_vertices;     /**< Per geometry. */
    long long max_depth;        /**< Collection / JSON nesting. */
    long long max_features;     /**< Per dataset pass. */
    long long max_input;        /**< Bytes of one input text. */
    long long max_report;       /**< Records of one report. */
    long long max_handles;      /**< Live objects. */
    long long timeout_ms;       /**< Per operation, 0 = none. */
    long long max_pixels;       /**< One raster read window. */
} geo_limits;

extern geo_limits geo_lim;

/** Apply the per-call overrides of the limits in params. */
extern int geo_limits_override(kv *k, geo_limits *l, char **err);
/** Arm the deadline of the current call (0 = none). */
extern void geo_deadline_arm(long long timeout_ms);
extern bool geo_deadline_passed(void);
extern uint64_t geo_now_ns(void);

/* ---- objects ------------------------------------------------------ */

typedef enum geo_otype {
    GEO_O_FREE = 0,
    GEO_O_GEOM,
    GEO_O_DS,
    GEO_O_TRACK,
} geo_otype;

/** A geometry and what it is in. */
typedef struct geo_geom {
    GEOSGeometry *g;
    char          crs[256];     /**< "EPSG:4326", a definition, or "". */
} geo_geom;

/** A GDAL dataset. */
typedef struct geo_ds {
    GDALDatasetH ds;
    bool         raster;
    char        *path;
    char         driver[64];
} geo_ds;

/** A time-ordered series of positions. */
typedef struct geo_track {
    size_t  n;
    double *t;
    double *x;
    double *y;
    double *z;
    bool    has_z;
    char    crs[256];
    char    axis[16];       /**< "gis" or "authority" */
} geo_track;

/** Register a new object; returns its handle or seterr(E2BIG). */
extern int geo_obj_add(geo_otype type, void *obj, int *handle, char **err);
/** Look a handle up; NULL (with err) when it is not of the type. */
extern void *geo_obj_get(int handle, geo_otype type, char **err);
extern int geo_obj_release(int handle, char **err);
extern void geo_obj_counts(unsigned int counts[4]);
extern void geo_obj_release_all(void);

extern void geo_geom_free(geo_geom *g);
extern void geo_ds_free(geo_ds *d);
extern void geo_track_free(geo_track *t);

/* ---- helpers shared across units -------------------------------- */

/** Wrap a GEOS geometry into a new handle (takes ownership). */
extern int geo_geom_new(GEOSGeometry *g, const char *crs, int *handle,
                        char **err);
/** Geometry type name in OGC spelling ("MultiPolygon"). */
extern const char *geo_type_name(int geos_type);
/** "xy", "xyz", "xym", "xyzm". */
extern const char *geo_dims_name(const GEOSGeometry *g);
/** WKT of a geometry (malloc), with full precision unless prec >= 0. */
extern char *geo_wkt(const GEOSGeometry *g, int prec);
/** Number of coordinates. */
extern long long geo_npoints(const GEOSGeometry *g);
/** Planar bounds; false for an empty geometry. */
extern bool geo_bounds(const GEOSGeometry *g, double b[4]);
/** "EPSG:n" for a bare number, the text otherwise. */
extern void geo_crs_norm(const char *in, char *out, size_t len);
/** Create a CRS object from a definition, with a reason on failure. */
extern PJ *geo_crs_create(const char *def, char **err);
/** Is it geographic (2D or 3D)? */
extern bool geo_crs_is_geographic(PJ *crs);
/** Linear unit name of a CRS ("metre", "degree", "US survey foot"). */
extern void geo_crs_unit(PJ *crs, char *out, size_t len);
/** Does x = longitude (gis order) or x = latitude (authority order)
 * apply to a geographic CRS? true when the first axis is latitude. */
extern bool geo_crs_lat_first(PJ *crs);

/** Parse a geometry from text; kind = wkt|wkb|ewkb|geojson|auto. */
extern int geo_parse_text(const char *kind, const char *text,
                          const geo_limits *l, GEOSGeometry **out,
                          int *srid_out, char **err);

/**
 * Transform a geometry with an operation (Z is carried when the
 * operation and the geometry have it). Non-finite results are EDOM.
 */
extern int geo_transform_geom(PJ *op, const GEOSGeometry *in,
                              GEOSGeometry **out, long long *n_bad,
                              char **err);

/**
 * A geometry as (longitude, latitude) degrees on the geodetic CRS of
 * @p crs: a geographic CRS is taken as is (swapped first when
 * @p axis is "authority" and its first axis is latitude), a projected
 * one is converted to its own geodetic CRS. The caller frees both.
 */
extern int geo_lonlat(const GEOSGeometry *g, const char *crs,
                      const char *axis, GEOSGeometry **out, PJ **geodetic,
                      char **err);

/** Geodesics on the ellipsoid of a geodetic CRS (PROJ's geodesic.h). */
typedef struct geo_geod_ctx geo_geod_ctx;
extern geo_geod_ctx *geo_geod_new(PJ *geodetic, char **err);
extern void geo_geod_free(geo_geod_ctx *c);
/** Distance in metres between (lon1, lat1) and (lon2, lat2), degrees. */
extern double geo_geod_dist(geo_geod_ctx *c, double lon1, double lat1,
                            double lon2, double lat2);
/** Destination from (lon, lat) after @p dist metres on @p azi degrees. */
extern void geo_geod_direct(geo_geod_ctx *c, double lon, double lat,
                            double azi, double dist, double *lon2,
                            double *lat2);
/** Distance from (lon, lat) to a lon/lat geometry: exact to its
 * vertices, via an azimuthal equidistant projection centred on
 * (lon, lat) to the interior of segments. */
extern int geo_geod_point_dist(geo_geod_ctx *c, double lon, double lat,
                               const GEOSGeometry *g, double *out,
                               char **err);
/** Length (lines) / perimeter (polygons) and area on the ellipsoid. */
extern int geo_geod_measure(geo_geod_ctx *c, const GEOSGeometry *g,
                            double *length, double *area, char **err);

/** Deterministic PRNG (xoshiro256**, seeded by splitmix64). */
typedef struct geo_rng {
    uint64_t s[4];
} geo_rng;

extern void geo_rng_seed(geo_rng *r, uint64_t seed);
extern uint64_t geo_rng_next(geo_rng *r);
/** Uniform in [0, 1). */
extern double geo_rng_unit(geo_rng *r);
extern double geo_rng_range(geo_rng *r, double lo, double hi);
/** Standard normal (Box-Muller). */
extern double geo_rng_normal(geo_rng *r);

/** Generated item: what the generator promises about it. */
typedef struct geo_gen_item {
    long long id;
    char     *text;         /**< WKT, or hex WKB for non-finite values */
    char      fmt[8];       /**< "wkt" or "wkb" */
    char      expect[32];   /**< "valid" or the defect category */
    double    t;            /**< trajectory samples: time */
} geo_gen_item;

/** Run a generator; items are malloc'd, the caller frees them. */
extern int geo_generate(const char *kind, kv *opts, geo_gen_item **items,
                        size_t *n, char **err);
extern void geo_gen_items_free(geo_gen_item *items, size_t n);

/** OGR <-> GEOS through ISO WKB (Z and M carried). */
extern GEOSGeometry *geo_ogr2geos(OGRGeometryH og, char **err);
extern OGRGeometryH geo_geos2ogr(const GEOSGeometry *g);

/** "EPSG:n" of an OGR spatial reference (best EPSG match when it has
 * no code of its own), its WKT2 otherwise, "" for none. */
extern void geo_srs_name(OGRSpatialReferenceH srs, char *out, size_t len);

/** GEOS validity reason -> category ("self_intersection", ...). */
extern const char *geo_validity_category(const char *reason);

/** FNV-1a 64-bit hash. */
extern uint64_t geo_hash(const void *p, size_t n);

/** Open-addressing map from 64-bit hashes to 64-bit values. */
typedef struct geo_hmap {
    uint64_t *keys;     /* 0 = empty; a zero hash is stored as 1 */
    int64_t  *vals;
    size_t    cap;
    size_t    n;
} geo_hmap;

/** Insert; returns 1 when new, 0 when present (*old set), -1 on ENOMEM. */
extern int geo_hmap_put(geo_hmap *m, uint64_t key, int64_t val,
                        int64_t *old);
/** Look up; true when present. */
extern bool geo_hmap_get(const geo_hmap *m, uint64_t key, int64_t *val);
extern void geo_hmap_free(geo_hmap *m);

/** Elapsed microseconds since @p t0 (geo_now_ns()). */
extern double geo_us_since(uint64_t t0);

/* ---- the operations, one per unit ------------------------------- */

typedef struct geo_req {
    int         ha;
    int         hb;
    const char *kind;
    const char *text;
    kv         *opt;
    int        *handle;
    unsigned int *features;
    sbuf       *res;
    sbuf       *rep;
    char      **err;
} geo_req;

extern int geo_op_geom(ta_geo_op op, geo_req *r);
extern int geo_op_crs(ta_geo_op op, geo_req *r);
extern int geo_op_ds(ta_geo_op op, geo_req *r);
extern int geo_op_raster(ta_geo_op op, geo_req *r);
extern int geo_op_track(ta_geo_op op, geo_req *r);
extern int geo_op_gen(geo_req *r);
extern int geo_op_ogc(geo_req *r);
extern int geo_op_perf(geo_req *r);

/** Shared by ds_write and the generator op. */
extern int geo_ds_write_items(const char *path, const char *driver,
                              const char *crs, const char *layer,
                              geo_gen_item *items, size_t n, kv *opt,
                              sbuf *res, sbuf *rep, char **err);

#endif /* !__TA_GEO_INT_H__ */
