/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: geometries, predicates, measurements
 *
 * Engine-side API of tsf-geospatial. Every operation runs on a Test
 * Agent, in the RPC server that links ta_geospatial (GEOS, PROJ, GDAL):
 * the engine needs none of those libraries. Geometries live on the
 * agent; a #tapi_geo_geometry is a handle to one, owned by the RPC
 * server that made it, released with tapi_geo_geometry_free() and
 * invalid once that RPC server is restarted.
 *
 * Failures are returned, never jumped on: an input the engine refuses
 * is often what a test is about. The engine's reason is kept and can
 * be read with tapi_geo_last_error().
 *
 * Topology is two-dimensional (DE-9IM, GEOS): Z and M are carried
 * through parsing, serialisation and transformation, and results that
 * ignored them say so (#tapi_geo_measure::ignored).
 */

#ifndef __TAPI_GEO_H__
#define __TAPI_GEO_H__

#include <stdbool.h>
#include <stdint.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "rcf_rpc.h"

#include "tapi_geo_rpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- engine ----------------------------------------------------- */

/** Feature bits of tapi_geo_available(). */
enum {
    TAPI_GEO_FEAT_GEOS = 1 << 0,
    TAPI_GEO_FEAT_PROJ = 1 << 1,
    TAPI_GEO_FEAT_GDAL = 1 << 2,
    TAPI_GEO_FEAT_M = 1 << 3,           /**< M coordinates */
    TAPI_GEO_FEAT_GEOJSON = 1 << 4,
    TAPI_GEO_FEAT_NETWORK = 1 << 5,     /**< PROJ network grids */
    TAPI_GEO_FEAT_HTTP = 1 << 6,        /**< GDAL WFS/OAPIF drivers */
    TAPI_GEO_FEAT_CURL = 1 << 7,        /**< OGC requests from the agent */
};

/** What the agent's engine is. */
typedef struct tapi_geo_engine {
    unsigned int features;      /**< TAPI_GEO_FEAT_* */
    char geos[64];
    char proj[64];
    char gdal[64];
    unsigned int drivers;       /**< GDAL drivers registered */
} tapi_geo_engine;

/**
 * Is the geospatial engine in this agent's RPC server, and what is it?
 *
 * @param rpcs    RPC server.
 * @param engine  Versions and features (may be @c NULL).
 *
 * @return 0, or TE_EOPNOTSUPP-like failures when the RPCs are missing.
 */
extern te_errno tapi_geo_available(rcf_rpc_server *rpcs,
                                   tapi_geo_engine *engine);

/**
 * Is a GDAL driver available with a capability?
 *
 * @param rpcs    RPC server.
 * @param driver  Short name ("GPKG", "GTiff").
 * @param create  @c true: it must be able to write.
 */
extern bool tapi_geo_driver_supported(rcf_rpc_server *rpcs,
                                      const char *driver, bool create);

/**
 * Set the engine's limits for the following operations ("key=value"
 * lines: max_vertices, max_depth, max_features, max_input, max_report,
 * max_handles, timeout_ms, max_pixels, proj_network, proj_data).
 */
extern te_errno tapi_geo_config(rcf_rpc_server *rpcs, const char *params);

/** Engine counters: live objects, operations, process memory. */
typedef struct tapi_geo_stats {
    unsigned int session;       /**< Random per RPC server process. */
    unsigned int live;
    unsigned int geometries;
    unsigned int datasets;
    unsigned int tracks;
    uint64_t created;
    uint64_t released;
    uint64_t ops;
    long rss_kb;
    long maxrss_kb;
} tapi_geo_stats;

extern te_errno tapi_geo_stats_get(rcf_rpc_server *rpcs,
                                   tapi_geo_stats *stats);

/** The engine's reason for the last failure in this process. */
extern const char *tapi_geo_last_error(void);

/* ---- generic calls ---------------------------------------------- */

/** Result of a generic call (an RPC result). */
typedef tapi_geo_rpc_res tapi_geo_res;
#define TAPI_GEO_RES_INIT TAPI_GEO_RPC_RES_INIT

/**
 * Run any engine operation. Lower level than the typed calls, for
 * options they do not expose. The failure reason is kept for
 * tapi_geo_last_error() and is in @p res->errmsg.
 */
extern te_errno tapi_geo_call(rcf_rpc_server *rpcs, tapi_geo_rpc_op op,
                              int ha, int hb, const char *kind,
                              const char *text, const char *params,
                              tapi_geo_res *res);

/** A value of the result, or @c NULL (pointer into @p buf). */
extern const char *tapi_geo_res_str(const tapi_geo_res *res,
                                    const char *key, char *buf, size_t len);
/** A number of the result, or NAN when absent. */
extern double tapi_geo_res_dbl(const tapi_geo_res *res, const char *key);
/** An integer of the result, or @p def when absent. */
extern long long tapi_geo_res_int(const tapi_geo_res *res, const char *key,
                                  long long def);

/**
 * Split the next report record into fields (percent-decoded in place).
 *
 * @param cursor  In/out: position in the report (start: report.ptr);
 *                the report is modified.
 * @param fields  Field pointers out.
 * @param max     Room in @p fields.
 *
 * @return Number of fields, 0 at the end.
 */
extern unsigned int tapi_geo_report_next(char **cursor, char **fields,
                                         unsigned int max);

/* ---- geometries ------------------------------------------------- */

/** A geometry on the agent. */
typedef struct tapi_geo_geometry {
    rcf_rpc_server *rpcs;       /**< Owner. */
    int             handle;     /**< Engine handle. */
    char            crs[128];   /**< "EPSG:4326", or "" when unknown. */
    char            type[32];   /**< "Polygon"... */
    char            dims[8];    /**< "xy", "xyz", "xym", "xyzm". */
} tapi_geo_geometry;

/** Serialisation formats. */
typedef enum tapi_geo_format {
    TAPI_GEO_FMT_WKT,
    TAPI_GEO_FMT_WKB,       /**< ISO WKB, as hex */
    TAPI_GEO_FMT_EWKB,      /**< PostGIS extended WKB with SRID, as hex */
    TAPI_GEO_FMT_GEOJSON,
} tapi_geo_format;

/**
 * Parse WKT (or EWKT with "SRID=n;").
 *
 * @param rpcs  RPC server.
 * @param wkt   Text.
 * @param srid  EPSG code of its CRS, 0 = unknown.
 * @param geom  New geometry (free with tapi_geo_geometry_free()).
 */
extern te_errno tapi_geo_parse_wkt(rcf_rpc_server *rpcs, const char *wkt,
                                   int srid, tapi_geo_geometry **geom);

/** Parse hex WKB or EWKB (an EWKB SRID wins over @p srid = 0). */
extern te_errno tapi_geo_parse_wkb(rcf_rpc_server *rpcs, const char *hex,
                                   int srid, tapi_geo_geometry **geom);

/** Parse a GeoJSON geometry, Feature or FeatureCollection. */
extern te_errno tapi_geo_parse_geojson(rcf_rpc_server *rpcs,
                                       const char *json, int srid,
                                       tapi_geo_geometry **geom);

/**
 * Parse with full control: @p fmt is "wkt", "wkb", "geojson", "auto";
 * @p crs any definition PROJ reads (or @c NULL); @p params extra
 * options (limits). On a parse failure @p category (if not @c NULL)
 * gets the defect category ("unclosed_ring", "parse_error"...).
 */
extern te_errno tapi_geo_parse(rcf_rpc_server *rpcs, const char *fmt,
                               const char *text, const char *crs,
                               const char *params, tapi_geo_geometry **geom,
                               te_string *category);

/** Release a geometry on the agent and the local handle (NULL-safe). */
extern te_errno tapi_geo_geometry_free(rcf_rpc_server *rpcs,
                                       tapi_geo_geometry *geom);

/** Serialise; the text is appended to @p out. */
extern te_errno tapi_geo_serialize(rcf_rpc_server *rpcs,
                                   const tapi_geo_geometry *geom,
                                   tapi_geo_format fmt, int precision,
                                   te_string *out);

/** Bounding box. */
typedef struct tapi_geo_bbox {
    double minx, miny, maxx, maxy;
} tapi_geo_bbox;

/** Facts about a geometry. */
typedef struct tapi_geo_info {
    char type[32];
    char dims[8];
    char crs[128];
    long long npoints;
    long long ngeoms;
    bool empty;
    int dimension;          /**< 0 points, 1 lines, 2 areas */
    tapi_geo_bbox bbox;     /**< NAN when empty */
    double minz, maxz;      /**< NAN without Z */
    double minm, maxm;      /**< NAN without M */
    long long nonfinite;    /**< NaN/infinite coordinates */
} tapi_geo_info;

extern te_errno tapi_geo_info_get(rcf_rpc_server *rpcs,
                                  const tapi_geo_geometry *geom,
                                  tapi_geo_info *info);

extern te_errno tapi_geo_bounds(rcf_rpc_server *rpcs,
                                const tapi_geo_geometry *geom,
                                tapi_geo_bbox *bbox);

/** The centroid (a new point geometry). */
extern te_errno tapi_geo_centroid(rcf_rpc_server *rpcs,
                                  const tapi_geo_geometry *geom,
                                  tapi_geo_geometry **out);

/** The envelope (a new polygon, or point for a point). */
extern te_errno tapi_geo_envelope(rcf_rpc_server *rpcs,
                                  const tapi_geo_geometry *geom,
                                  tapi_geo_geometry **out);

/**
 * Any derivation: "centroid", "envelope", "boundary", "convex_hull",
 * "point_on_surface", "clone", "normalize", "part" (params index=n).
 */
extern te_errno tapi_geo_derive(rcf_rpc_server *rpcs,
                                const tapi_geo_geometry *geom,
                                const char *what, const char *params,
                                tapi_geo_geometry **out);

/* ---- validation (never repairs) --------------------------------- */

/** One defect found by validation. */
typedef struct tapi_geo_issue {
    char category[40];      /**< self_intersection, hole_outside, ... */
    double x, y;            /**< Where, or NAN. */
    char diagnostic[256];
} tapi_geo_issue;

/** What a validation found. */
typedef struct tapi_geo_validity {
    bool valid;             /**< OGC validity (GEOS). */
    bool ok;                /**< No issue of any requested check. */
    unsigned int issues;
    char category[40];      /**< The first issue's category, or "". */
    char reason[256];
    double x, y;            /**< Its location, or NAN. */
    char id[64];            /**< The geometry identifier given. */
    tapi_geo_issue *list;   /**< The issues (first max_report). */
    unsigned int n_list;
} tapi_geo_validity;

extern void tapi_geo_validity_free(tapi_geo_validity *v);

/**
 * Validate: OGC validity, non-finite coordinates, emptiness,
 * degenerate parts.
 *
 * @param id  Identifier to carry into the report (may be @c NULL).
 */
extern te_errno tapi_geo_check_validity(rcf_rpc_server *rpcs,
                                        const tapi_geo_geometry *geom,
                                        const char *id,
                                        tapi_geo_validity *v);

/**
 * Run checks by name ("all", "validity", "finite", "empty",
 * "duplicates", "orientation", "bounds", "dims", "type") with their
 * options (orientation=ccw, bounds=..., allow_dims=xy,xyz,
 * expect_type=Polygon,MultiPolygon, tolerance=...).
 */
extern te_errno tapi_geo_validate(rcf_rpc_server *rpcs,
                                  const tapi_geo_geometry *geom,
                                  const char *checks, const char *params,
                                  tapi_geo_validity *v);

/** Rings turning the wrong way (shells @p ccw, holes the other way). */
extern te_errno tapi_geo_check_orientation(rcf_rpc_server *rpcs,
                                           const tapi_geo_geometry *geom,
                                           bool ccw,
                                           unsigned int *misoriented);

/** Consecutive vertices closer than @p tolerance. */
extern te_errno tapi_geo_check_duplicates(rcf_rpc_server *rpcs,
                                          const tapi_geo_geometry *geom,
                                          double tolerance,
                                          unsigned int *duplicates);

/** Vertices outside @p bbox. */
extern te_errno tapi_geo_check_bounds(rcf_rpc_server *rpcs,
                                      const tapi_geo_geometry *geom,
                                      const tapi_geo_bbox *bbox,
                                      unsigned int *outside);

/** Repair methods (only ever applied when asked for). */
typedef enum tapi_geo_repair_method {
    TAPI_GEO_REPAIR_MAKE_VALID,         /**< GEOS MakeValid, linework */
    TAPI_GEO_REPAIR_MAKE_VALID_STRUCTURE,
    TAPI_GEO_REPAIR_BUFFER0,
    TAPI_GEO_REPAIR_ORIENT_CCW,
    TAPI_GEO_REPAIR_REMOVE_DUPLICATES,
} tapi_geo_repair_method;

/** A repaired copy; the original is untouched. */
extern te_errno tapi_geo_repair(rcf_rpc_server *rpcs,
                                const tapi_geo_geometry *geom,
                                tapi_geo_repair_method method,
                                tapi_geo_geometry **out);

/* ---- predicates (DE-9IM, 2D) ------------------------------------ */

extern te_errno tapi_geo_intersects(rcf_rpc_server *rpcs,
                                    const tapi_geo_geometry *a,
                                    const tapi_geo_geometry *b, bool *res);
extern te_errno tapi_geo_contains(rcf_rpc_server *rpcs,
                                  const tapi_geo_geometry *a,
                                  const tapi_geo_geometry *b, bool *res);
extern te_errno tapi_geo_within(rcf_rpc_server *rpcs,
                                const tapi_geo_geometry *a,
                                const tapi_geo_geometry *b, bool *res);
extern te_errno tapi_geo_touches(rcf_rpc_server *rpcs,
                                 const tapi_geo_geometry *a,
                                 const tapi_geo_geometry *b, bool *res);
extern te_errno tapi_geo_crosses(rcf_rpc_server *rpcs,
                                 const tapi_geo_geometry *a,
                                 const tapi_geo_geometry *b, bool *res);
extern te_errno tapi_geo_overlaps(rcf_rpc_server *rpcs,
                                  const tapi_geo_geometry *a,
                                  const tapi_geo_geometry *b, bool *res);
extern te_errno tapi_geo_disjoint(rcf_rpc_server *rpcs,
                                  const tapi_geo_geometry *a,
                                  const tapi_geo_geometry *b, bool *res);
extern te_errno tapi_geo_equals(rcf_rpc_server *rpcs,
                                const tapi_geo_geometry *a,
                                const tapi_geo_geometry *b, bool *res);

/**
 * Any predicate by name ("covers", "covered_by", "equals_exact" with
 * tolerance=, "dwithin" with distance=, "relate" with pattern=), and
 * the DE-9IM matrix (10 bytes, may be @c NULL).
 */
extern te_errno tapi_geo_predicate(rcf_rpc_server *rpcs, const char *name,
                                   const tapi_geo_geometry *a,
                                   const tapi_geo_geometry *b,
                                   const char *params, bool *res,
                                   char *de9im);

/* ---- constructive operations ------------------------------------ */

extern te_errno tapi_geo_intersection(rcf_rpc_server *rpcs,
                                      const tapi_geo_geometry *a,
                                      const tapi_geo_geometry *b,
                                      tapi_geo_geometry **out);
extern te_errno tapi_geo_union(rcf_rpc_server *rpcs,
                               const tapi_geo_geometry *a,
                               const tapi_geo_geometry *b,
                               tapi_geo_geometry **out);
extern te_errno tapi_geo_difference(rcf_rpc_server *rpcs,
                                    const tapi_geo_geometry *a,
                                    const tapi_geo_geometry *b,
                                    tapi_geo_geometry **out);
extern te_errno tapi_geo_symmetric_difference(rcf_rpc_server *rpcs,
                                              const tapi_geo_geometry *a,
                                              const tapi_geo_geometry *b,
                                              tapi_geo_geometry **out);

/** Buffer by @p distance (CRS units), @p quad_segs per quarter circle. */
extern te_errno tapi_geo_buffer(rcf_rpc_server *rpcs,
                                const tapi_geo_geometry *geom,
                                double distance, unsigned int quad_segs,
                                tapi_geo_geometry **out);

/** Douglas-Peucker, topology-preserving or not. */
extern te_errno tapi_geo_simplify(rcf_rpc_server *rpcs,
                                  const tapi_geo_geometry *geom,
                                  double tolerance, bool preserve_topology,
                                  tapi_geo_geometry **out);

/** Any construction by name with options (see the engine). */
extern te_errno tapi_geo_construct(rcf_rpc_server *rpcs, const char *name,
                                   const tapi_geo_geometry *a,
                                   const tapi_geo_geometry *b,
                                   const char *params,
                                   tapi_geo_geometry **out);

/* ---- measurements ----------------------------------------------- */

/** How a measurement is made: always said, never assumed. */
typedef enum tapi_geo_mode {
    TAPI_GEO_PLANAR,        /**< Cartesian, in the CRS's own unit */
    TAPI_GEO_GEODESIC,      /**< On the ellipsoid of the datum, metres */
} tapi_geo_mode;

/**
 * Axis order of coordinates: always said, never assumed. GIS order is
 * x = easting / longitude; authority order is the CRS's own (latitude
 * first for EPSG:4326).
 */
typedef enum tapi_geo_axis {
    TAPI_GEO_AXIS_GIS,
    TAPI_GEO_AXIS_AUTHORITY,
} tapi_geo_axis;

extern const char *tapi_geo_axis2str(tapi_geo_axis axis);

/** A measured value and what it is. */
typedef struct tapi_geo_measure {
    double value;
    char unit[64];          /**< "m", "m2", "degree", "US survey foot" */
    tapi_geo_mode mode;
    char method[128];
    bool geographic;        /**< planar on lon/lat: the unit is degrees */
    char ignored[8];        /**< "z", "m", "z,m" or "" */
} tapi_geo_measure;

extern te_errno tapi_geo_distance(rcf_rpc_server *rpcs,
                                  const tapi_geo_geometry *a,
                                  const tapi_geo_geometry *b,
                                  tapi_geo_mode mode, tapi_geo_axis axis,
                                  tapi_geo_measure *m);
extern te_errno tapi_geo_length(rcf_rpc_server *rpcs,
                                const tapi_geo_geometry *geom,
                                tapi_geo_mode mode, tapi_geo_axis axis,
                                tapi_geo_measure *m);
extern te_errno tapi_geo_area(rcf_rpc_server *rpcs,
                              const tapi_geo_geometry *geom,
                              tapi_geo_mode mode, tapi_geo_axis axis,
                              tapi_geo_measure *m);
extern te_errno tapi_geo_perimeter(rcf_rpc_server *rpcs,
                                   const tapi_geo_geometry *geom,
                                   tapi_geo_mode mode, tapi_geo_axis axis,
                                   tapi_geo_measure *m);

/**
 * Any measurement ("distance", "hausdorff", "frechet", "length",
 * "area", "perimeter") with extra options (require_linear=1).
 */
extern te_errno tapi_geo_measure_any(rcf_rpc_server *rpcs, const char *what,
                                     const tapi_geo_geometry *a,
                                     const tapi_geo_geometry *b,
                                     tapi_geo_mode mode, tapi_geo_axis axis,
                                     const char *params,
                                     tapi_geo_measure *m);

/* ---- generation ------------------------------------------------- */

/** A generated input and what it is promised to be. */
typedef struct tapi_geo_gen_item {
    long long id;
    char fmt[8];            /**< "wkt" or "wkb" (hex) */
    char expect[32];        /**< "valid" or the defect category */
    double t;               /**< trajectories: time, s */
    char *text;
} tapi_geo_gen_item;

/**
 * Generate deterministic inputs on the agent ("points", "grid",
 * "polygons", "lines", "boundary", "outliers", "trajectory",
 * "invalid" - the last only with allow_invalid=1).
 *
 * @param digest  Order-sensitive digest of the texts (may be @c NULL).
 */
extern te_errno tapi_geo_generate(rcf_rpc_server *rpcs, const char *kind,
                                  const char *params,
                                  tapi_geo_gen_item **items,
                                  unsigned int *n, te_string *digest);

extern void tapi_geo_gen_items_free(tapi_geo_gen_item *items,
                                    unsigned int n);

/* ---- performance ------------------------------------------------ */

/** A timed loop on the agent. */
typedef struct tapi_geo_perf {
    long long iterations;
    double cold_us;         /**< the first call */
    double warm_us;         /**< mean of the others */
    double ops_per_s;
    double items_per_s;     /**< vertices or points per second */
    long long work_items;   /**< complexity of one call */
} tapi_geo_perf;

/**
 * Measure ("parse" of @p text, "predicate", "prepared_predicate",
 * "buffer" of handles, "transform" with src/dst/count).
 */
extern te_errno tapi_geo_perf_run(rcf_rpc_server *rpcs, const char *what,
                                  const tapi_geo_geometry *a,
                                  const tapi_geo_geometry *b,
                                  const char *text, const char *params,
                                  tapi_geo_perf *perf);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_GEO_H__ */
