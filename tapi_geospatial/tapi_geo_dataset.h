/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: vector datasets
 *
 * Datasets (GeoJSON, GeoPackage, Shapefile, KML, GML, FlatGeobuf, and
 * whatever else the agent's GDAL reads; see tapi_geo_driver_supported())
 * are opened on the agent and stay there. Validation and comparison
 * stream them in one pass each, keeping per-feature hashes only, so a
 * dataset far larger than the engine's memory is processed in bounded
 * memory; only counts and bounded reports come back.
 *
 * By default a dataset may be opened with a fixed list of file-format
 * drivers - no VRT, which can point at any file on the agent. Options
 * "drivers=any" or "drivers=GPKG,GeoJSON" change that.
 */

#ifndef __TAPI_GEO_DATASET_H__
#define __TAPI_GEO_DATASET_H__

#include "tapi_geo.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A dataset opened on the agent. */
typedef struct tapi_geo_dataset {
    rcf_rpc_server *rpcs;
    int             handle;
    bool            raster;
    char            driver[64];
    unsigned int    layers;
    unsigned int    bands;
    double          open_us;    /**< time to open on the agent */
} tapi_geo_dataset;

/**
 * Open a vector dataset.
 *
 * @param path    Path on the agent (or a GDAL connection string).
 * @param params  Options: drivers=..., oo.NAME=VALUE open options,
 *                update=1 (may be @c NULL).
 */
extern te_errno tapi_geo_dataset_open(rcf_rpc_server *rpcs, const char *path,
                                      const char *params,
                                      tapi_geo_dataset **ds);

/** Close and release (NULL-safe). */
extern te_errno tapi_geo_dataset_close(rcf_rpc_server *rpcs,
                                       tapi_geo_dataset *ds);

/** A layer. */
typedef struct tapi_geo_layer {
    char name[128];
    char geometry_type[48];
    char crs[128];
    long long count;        /**< as the driver knows it cheaply; -1 */
} tapi_geo_layer;

extern te_errno tapi_geo_dataset_layers(rcf_rpc_server *rpcs,
                                        const tapi_geo_dataset *ds,
                                        tapi_geo_layer **layers,
                                        unsigned int *n,
                                        bool *crs_consistent);

/** A field of a layer. */
typedef struct tapi_geo_field {
    char name[64];
    char type[24];          /**< Integer, Integer64, Real, String, ... */
    int width;
    int precision;
    bool nullable;
} tapi_geo_field;

/** @p layer: name or index ("0"), @c NULL = the first. */
extern te_errno tapi_geo_dataset_schema(rcf_rpc_server *rpcs,
                                        const tapi_geo_dataset *ds,
                                        const char *layer,
                                        tapi_geo_field **fields,
                                        unsigned int *n,
                                        char *geometry_type, size_t gt_len);

/** Feature count; @p exact iterates instead of asking the driver. */
extern te_errno tapi_geo_dataset_count(rcf_rpc_server *rpcs,
                                       const tapi_geo_dataset *ds,
                                       const char *layer, bool exact,
                                       long long *count);

extern te_errno tapi_geo_dataset_extent(rcf_rpc_server *rpcs,
                                        const tapi_geo_dataset *ds,
                                        const char *layer,
                                        tapi_geo_bbox *extent);

/**
 * Fetch at most @p limit features from @p offset as report records
 * "fid TAB WKT TAB attributes-as-JSON" appended to @p out. @p params:
 * where=<OGR SQL filter>, bbox=..., geometry=wkt|geojson|none.
 */
extern te_errno tapi_geo_dataset_fetch(rcf_rpc_server *rpcs,
                                       const tapi_geo_dataset *ds,
                                       const char *layer, long long offset,
                                       long long limit, const char *params,
                                       te_string *out,
                                       unsigned int *returned);

/** One feature's geometry, as a geometry on the agent. */
extern te_errno tapi_geo_dataset_geometry(rcf_rpc_server *rpcs,
                                          const tapi_geo_dataset *ds,
                                          const char *layer, long long fid,
                                          tapi_geo_geometry **geom);

/** What a dataset must be. Unset (@c NULL) checks are not made. */
typedef struct tapi_geo_ds_rules {
    const char *layer;
    const char *id_field;       /**< unique, present */
    const char *expect_types;   /**< "Polygon,MultiPolygon" */
    const char *expect_crs;     /**< "EPSG:4326" */
    const char *bounds;         /**< "minx miny maxx maxy" */
    const char *schema;         /**< "name:Type;..." */
    bool strict_schema;         /**< no fields beyond the schema */
    const char *required;       /**< "a,b": not NULL */
    const char *extra;          /**< more options: range.f=lo:hi,
                                     allowed.f=a|b, validity=0 */
} tapi_geo_ds_rules;

/** What validation found: counts per category, and the records. */
typedef struct tapi_geo_ds_report {
    long long features;
    long long bad_features;
    long long issues;
    bool ok;
    char crs[128];
    long long invalid_geometry;
    long long missing_geometry;
    long long empty_geometry;
    long long duplicate_id;
    long long crs_mismatch;
    long long out_of_range_coordinate;
    long long schema_mismatch;
    long long unexpected_geometry_type;
    long long invalid_attribute;
    long long non_finite;
    long long missing_id;
    te_string records;      /**< "fid TAB id TAB category TAB detail" */
} tapi_geo_ds_report;

extern te_errno tapi_geo_dataset_validate(rcf_rpc_server *rpcs,
                                          const tapi_geo_dataset *ds,
                                          const tapi_geo_ds_rules *rules,
                                          tapi_geo_ds_report *rep);

extern void tapi_geo_ds_report_free(tapi_geo_ds_report *rep);

/** How two datasets are compared. */
typedef struct tapi_geo_ds_cmp_opts {
    const char *layer_a;
    const char *layer_b;
    const char *id_field;       /**< match features by it (else FID) */
    const char *geometry;       /**< topological, exact, hausdorff, none */
    double tolerance;           /**< geometry tolerance, CRS units */
    double float_tolerance;     /**< numeric attributes */
    const char *attributes;     /**< all, none, or "a,b" */
} tapi_geo_ds_cmp_opts;

#define TAPI_GEO_DS_CMP_OPTS_DEFAULT \
    { NULL, NULL, NULL, "topological", 0, 0, "all" }

typedef struct tapi_geo_ds_diff {
    bool crs_compatible;
    bool equal;
    long long count_a, count_b;
    long long matched;
    long long only_a, only_b;
    long long geometry_diff;
    long long attribute_diff;
    long long duplicate_id_a, duplicate_id_b;
    double max_hausdorff;
    te_string records;      /**< "kind TAB id TAB field TAB a TAB b" */
} tapi_geo_ds_diff;

extern te_errno tapi_geo_dataset_compare(rcf_rpc_server *rpcs,
                                         const tapi_geo_dataset *a,
                                         const tapi_geo_dataset *b,
                                         const tapi_geo_ds_cmp_opts *opts,
                                         tapi_geo_ds_diff *diff);

extern void tapi_geo_ds_diff_free(tapi_geo_ds_diff *diff);

/** A planted defect of a generated dataset. */
typedef struct tapi_geo_planted {
    long long position;
    long long id;
    char defect[24];
} tapi_geo_planted;

/**
 * Write a generated dataset on the agent: generator @p gen
 * (tapi_geo_generate() kinds) with @p gen_params, through @p driver,
 * in @p crs, with defects planted ("invalid:3,dup_id:2,missing_geom:1,
 * out_of_range:2,wrong_type:1,bad_attr:1,null_attr:1,empty_geom:1").
 * Fields: id, name, value (0..999; bad_attr = -1), expect, t.
 */
extern te_errno tapi_geo_dataset_generate(rcf_rpc_server *rpcs,
                                          const char *path,
                                          const char *driver,
                                          const char *crs, const char *gen,
                                          const char *gen_params,
                                          const char *defects,
                                          long long *written,
                                          tapi_geo_planted **planted,
                                          unsigned int *n_planted);

/** Copy a dataset into another format (optionally reprojected). */
extern te_errno tapi_geo_dataset_copy(rcf_rpc_server *rpcs,
                                      const tapi_geo_dataset *src,
                                      const char *path, const char *driver,
                                      const char *dst_crs);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_GEO_DATASET_H__ */
