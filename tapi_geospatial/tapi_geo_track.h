/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: geolocation and tracking
 *
 * Normalised observations - time in seconds, position, optional
 * height - from a system under test (a GNSS receiver, a tracker, a
 * navigation app) are loaded into a track on the agent and evaluated
 * there: sanity of the series, error against a reference track,
 * distance to a reference route, geofences. GNSS protocols are not
 * decoded; the test normalises what the device says. Distances are
 * geodesic, on the datum of the track's CRS.
 */

#ifndef __TAPI_GEO_TRACK_H__
#define __TAPI_GEO_TRACK_H__

#include "tapi_geo.h"
#include "tapi_geo_dataset.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One observation. */
typedef struct tapi_geo_obs {
    double t;       /**< seconds (any epoch, the same for all) */
    double x;       /**< longitude / easting, in @p axis order */
    double y;
    double z;       /**< height, NAN when none */
} tapi_geo_obs;

/** A track on the agent. */
typedef struct tapi_geo_track {
    rcf_rpc_server *rpcs;
    int             handle;
    long long       samples;
} tapi_geo_track;

/** Load observations (in @p crs, default EPSG:4326). */
extern te_errno tapi_geo_track_load(rcf_rpc_server *rpcs,
                                    const tapi_geo_obs *obs, unsigned int n,
                                    const char *crs, tapi_geo_axis axis,
                                    tapi_geo_track **track);

/** Load observations from a dataset layer of points with a time field
 * (seconds, or a date-time). */
extern te_errno tapi_geo_track_load_dataset(rcf_rpc_server *rpcs,
                                            const tapi_geo_dataset *ds,
                                            const char *layer,
                                            const char *time_field,
                                            const char *crs,
                                            tapi_geo_axis axis,
                                            tapi_geo_track **track);

/**
 * Load the samples of a generated trajectory (tapi_geo_generate()
 * kind "trajectory" with @p params) as a track.
 */
extern te_errno tapi_geo_track_generate(rcf_rpc_server *rpcs,
                                        const char *params,
                                        tapi_geo_track **track);

extern te_errno tapi_geo_track_free(rcf_rpc_server *rpcs,
                                    tapi_geo_track *track);

/** Limits a series must keep. INFINITY / 0 = not checked. */
typedef struct tapi_geo_track_limits {
    double max_speed;       /**< m/s */
    double max_gap;         /**< s */
    double max_jump;        /**< m between consecutive samples */
    double interval;        /**< expected sampling interval, s */
} tapi_geo_track_limits;

typedef struct tapi_geo_track_sanity {
    long long samples;
    long long out_of_order;
    long long duplicate_time;
    long long gaps;
    long long missing;      /**< samples missing at the interval */
    long long jumps;
    long long impossible;   /**< speed over max_speed */
    double length_m;
    double duration_s;
    double max_speed_ms;
    double mean_speed_ms;
    bool ok;
    te_string events;       /**< "index TAB t TAB kind TAB value" */
} tapi_geo_track_sanity;

extern te_errno tapi_geo_track_check(rcf_rpc_server *rpcs,
                                     const tapi_geo_track *track,
                                     const tapi_geo_track_limits *lim,
                                     tapi_geo_track_sanity *res);

/** Position error statistics, metres. */
typedef struct tapi_geo_track_error {
    long long n;
    long long unmatched;        /**< samples outside the reference */
    long long over;             /**< errors over the threshold */
    double mean, max, rmse;
    double p50, p90, p95, p99;
    double *errors;             /**< per matched sample (first
                                     max_report), heap */
    double *times;
    unsigned int n_errors;
} tapi_geo_track_error;

extern void tapi_geo_track_error_free(tapi_geo_track_error *e);

/** Error of @p track against @p ref, interpolated in time. */
extern te_errno tapi_geo_track_error_vs(rcf_rpc_server *rpcs,
                                        const tapi_geo_track *track,
                                        const tapi_geo_track *ref,
                                        double threshold,
                                        tapi_geo_track_error *err);

/** Distance of every sample to a route (a line geometry). */
extern te_errno tapi_geo_track_route(rcf_rpc_server *rpcs,
                                     const tapi_geo_track *track,
                                     const tapi_geo_geometry *route,
                                     double tolerance,
                                     tapi_geo_track_error *err);

typedef struct tapi_geo_geofence {
    long long samples;
    long long inside;
    long long outside;
    long long transitions;
    long long violations;
    double first_violation_t;   /**< NAN when none */
    bool ok;
    te_string events;           /**< "index TAB t TAB enter|exit" */
} tapi_geo_geofence;

/** @p expect: "inside", "outside" or "any". */
extern te_errno tapi_geo_track_geofence(rcf_rpc_server *rpcs,
                                        const tapi_geo_track *track,
                                        const tapi_geo_geometry *fence,
                                        const char *expect,
                                        tapi_geo_geofence *res);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_GEO_TRACK_H__ */
