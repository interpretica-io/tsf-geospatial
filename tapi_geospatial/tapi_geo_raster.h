/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: rasters (GeoTIFF and the other GDAL formats)
 *
 * Statistics and comparisons run tile by tile on the agent: at most a
 * tile per band per raster is in memory, whatever the image size.
 */

#ifndef __TAPI_GEO_RASTER_H__
#define __TAPI_GEO_RASTER_H__

#include "tapi_geo_dataset.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef tapi_geo_dataset tapi_geo_raster;

extern te_errno tapi_geo_raster_open(rcf_rpc_server *rpcs, const char *path,
                                     const char *params,
                                     tapi_geo_raster **r);

extern te_errno tapi_geo_raster_close(rcf_rpc_server *rpcs,
                                      tapi_geo_raster *r);

/** Raster metadata (band 1 for the per-band fields). */
typedef struct tapi_geo_raster_info {
    char driver[64];
    int width, height, bands;
    char type[24];              /**< Byte, UInt16, Float32... */
    bool has_nodata;
    double nodata;
    char crs[128];
    bool georeferenced;
    double geotransform[6];
    double res_x, res_y;
    bool rotated;
    tapi_geo_bbox extent;
    int block_x, block_y;
    int overviews;
    char compression[32];
} tapi_geo_raster_info;

extern te_errno tapi_geo_raster_info_get(rcf_rpc_server *rpcs,
                                         const tapi_geo_raster *r,
                                         tapi_geo_raster_info *info);

/** Exact statistics of one band. */
typedef struct tapi_geo_raster_stats {
    long long pixels, valid, nodata, nonfinite;
    double min, max, mean, stddev;
    double elapsed_us;
    double mpix_per_s;
} tapi_geo_raster_stats;

/** @p tile: tile edge in pixels, 0 = the band's block. */
extern te_errno tapi_geo_raster_statistics(rcf_rpc_server *rpcs,
                                           const tapi_geo_raster *r,
                                           int band, int tile,
                                           tapi_geo_raster_stats *st);

/**
 * Read a window of a band; @p values (may be @c NULL) gets
 * @p w * @p h doubles, row-major.
 */
extern te_errno tapi_geo_raster_read(rcf_rpc_server *rpcs,
                                     const tapi_geo_raster *r, int band,
                                     int x, int y, int w, int h,
                                     double *values,
                                     tapi_geo_raster_stats *st);

typedef struct tapi_geo_raster_diff {
    bool equal;
    long long metadata_diff;
    long long compared;
    long long pixel_diff;
    long long nodata_mismatch;
    double max_abs_diff;
    double mean_diff;
    double rmse;
    te_string records;  /**< "meta TAB what" / "pixel TAB band TAB x TAB
                             y TAB a TAB b" */
} tapi_geo_raster_diff;

extern te_errno tapi_geo_raster_compare(rcf_rpc_server *rpcs,
                                        const tapi_geo_raster *a,
                                        const tapi_geo_raster *b,
                                        double tolerance, int tile,
                                        tapi_geo_raster_diff *diff);

extern void tapi_geo_raster_diff_free(tapi_geo_raster_diff *diff);

/**
 * Write a synthetic raster. @p params: width, height, bands, type,
 * pattern=gradient|checker|constant|random, a, bx, by (gradient
 * a + bx*x + by*y + 1000*(band-1)), lo, hi, cell, seed, nodata,
 * nodata_every, crs, geotransform="x0 dx 0 y0 0 dy", block, tiled,
 * noise_pixels + noise (perturbation), driver (GTiff, COG, PNG,
 * MBTiles), co.NAME=VALUE.
 */
extern te_errno tapi_geo_raster_write(rcf_rpc_server *rpcs, const char *path,
                                      const char *params);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_GEO_RASTER_H__ */
