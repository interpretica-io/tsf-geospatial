/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: geolocation and tracking
 */

#define TE_LGR_USER     "TAPI GEO TRACK"

#include "te_config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "logger_api.h"

#include "tapi_geo_track.h"
#include "tapi_geo_internal.h"

static te_errno
tr_handle(rcf_rpc_server *rpcs, const tapi_geo_track *t, int *h)
{
    if (t == NULL || t->rpcs != rpcs)
        return TE_RC(TE_TAPI, TE_EINVAL);
    *h = t->handle;
    return 0;
}

static te_errno
wrap(rcf_rpc_server *rpcs, tapi_geo_res *r, tapi_geo_track **out)
{
    tapi_geo_track *t = calloc(1, sizeof(*t));

    if (t == NULL)
        return TE_RC(TE_TAPI, TE_ENOMEM);
    t->rpcs = rpcs;
    t->handle = r->handle;
    t->samples = tapi_geo_res_int(r, "samples", 0);
    *out = t;
    return 0;
}

static te_errno
load(rcf_rpc_server *rpcs, const char *kind, int ha, const char *text,
     const char *params, tapi_geo_track **out)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_TRACK_LOAD, ha, 0, kind,
                                text, params, &r);

    *out = NULL;
    if (rc == 0)
        rc = wrap(rpcs, &r, out);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo_track.h */
te_errno
tapi_geo_track_load(rcf_rpc_server *rpcs, const tapi_geo_obs *obs,
                    unsigned int n, const char *crs, tapi_geo_axis axis,
                    tapi_geo_track **track)
{
    te_string t = TE_STRING_INIT;
    te_string p = TE_STRING_INIT;
    unsigned int i;
    te_errno rc;

    for (i = 0; i < n; i++)
    {
        if (isnan(obs[i].z))
            te_string_append(&t, "%.17g %.17g %.17g\n", obs[i].t, obs[i].x,
                             obs[i].y);
        else
            te_string_append(&t, "%.17g %.17g %.17g %.17g\n", obs[i].t,
                             obs[i].x, obs[i].y, obs[i].z);
    }
    te_string_append(&p, "crs=%s\naxis=%s\n", crs != NULL ? crs : "EPSG:4326",
                     tapi_geo_axis2str(axis));
    rc = load(rpcs, "text", 0, t.ptr, p.ptr, track);
    te_string_free(&t);
    te_string_free(&p);
    return rc;
}

/* See description in tapi_geo_track.h */
te_errno
tapi_geo_track_load_dataset(rcf_rpc_server *rpcs, const tapi_geo_dataset *ds,
                            const char *layer, const char *time_field,
                            const char *crs, tapi_geo_axis axis,
                            tapi_geo_track **track)
{
    te_string p = TE_STRING_INIT;
    te_errno rc;

    if (ds == NULL || ds->rpcs != rpcs)
        return TE_RC(TE_TAPI, TE_EINVAL);
    te_string_append(&p, "crs=%s\naxis=%s\ntime_field=%s\n",
                     crs != NULL ? crs : "EPSG:4326", tapi_geo_axis2str(axis),
                     time_field != NULL ? time_field : "t");
    if (layer != NULL)
        te_string_append(&p, "layer=%s\n", layer);
    rc = load(rpcs, "dataset", ds->handle, "", p.ptr, track);
    te_string_free(&p);
    return rc;
}

/* See description in tapi_geo_track.h */
te_errno
tapi_geo_track_generate(rcf_rpc_server *rpcs, const char *params,
                        tapi_geo_track **track)
{
    tapi_geo_gen_item *items = NULL;
    unsigned int n = 0;
    te_string t = TE_STRING_INIT;
    unsigned int i;
    te_errno rc;

    rc = tapi_geo_generate(rpcs, "trajectory", params, &items, &n, NULL);
    if (rc != 0)
        return rc;
    for (i = 0; i < n; i++)
    {
        double x, y;

        if (sscanf(items[i].text, "POINT (%lf %lf)", &x, &y) == 2)
            te_string_append(&t, "%.17g %.17g %.17g\n", items[i].t, x, y);
    }
    tapi_geo_gen_items_free(items, n);
    rc = load(rpcs, "text", 0, t.ptr, "axis=gis", track);
    te_string_free(&t);
    return rc;
}

/* See description in tapi_geo_track.h */
te_errno
tapi_geo_track_free(rcf_rpc_server *rpcs, tapi_geo_track *track)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc;
    int h;

    if (track == NULL)
        return 0;
    rc = tr_handle(rpcs, track, &h);
    if (rc == 0)
        rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_RELEASE, h, 0, "", "", "", &r);
    tapi_geo_rpc_res_free(&r);
    free(track);
    return rc;
}

static te_errno
eval(rcf_rpc_server *rpcs, const tapi_geo_track *t, int hb, const char *kind,
     const char *params, tapi_geo_res *r)
{
    te_errno rc;
    int h;

    if ((rc = tr_handle(rpcs, t, &h)) != 0)
        return rc;
    return tapi_geo_call(rpcs, TAPI_GEO_RPC_TRACK_EVAL, h, hb, kind, "",
                         params, r);
}

/* See description in tapi_geo_track.h */
te_errno
tapi_geo_track_check(rcf_rpc_server *rpcs, const tapi_geo_track *track,
                     const tapi_geo_track_limits *l,
                     tapi_geo_track_sanity *s)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    te_errno rc;

    memset(s, 0, sizeof(*s));
    s->events = (te_string)TE_STRING_INIT;
    if (l != NULL)
    {
        if (isfinite(l->max_speed))
            te_string_append(&p, "max_speed=%.17g\n", l->max_speed);
        if (isfinite(l->max_gap))
            te_string_append(&p, "max_gap=%.17g\n", l->max_gap);
        if (isfinite(l->max_jump))
            te_string_append(&p, "max_jump=%.17g\n", l->max_jump);
        if (l->interval > 0)
            te_string_append(&p, "interval=%.17g\n", l->interval);
    }
    rc = eval(rpcs, track, 0, "sanity", p.ptr, &r);
    if (rc == 0)
    {
        s->samples = tapi_geo_res_int(&r, "samples", 0);
        s->out_of_order = tapi_geo_res_int(&r, "out_of_order", 0);
        s->duplicate_time = tapi_geo_res_int(&r, "duplicate_time", 0);
        s->gaps = tapi_geo_res_int(&r, "gaps", 0);
        s->missing = tapi_geo_res_int(&r, "missing", 0);
        s->jumps = tapi_geo_res_int(&r, "jumps", 0);
        s->impossible = tapi_geo_res_int(&r, "impossible", 0);
        s->length_m = tapi_geo_res_dbl(&r, "length_m");
        s->duration_s = tapi_geo_res_dbl(&r, "duration_s");
        s->max_speed_ms = tapi_geo_res_dbl(&r, "max_speed_ms");
        s->mean_speed_ms = tapi_geo_res_dbl(&r, "mean_speed_ms");
        s->ok = tapi_geo_res_int(&r, "ok", 0) != 0;
        if (r.report.ptr != NULL)
            te_string_append(&s->events, "%s", r.report.ptr);
    }
    te_string_free(&p);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

void
tapi_geo_track_error_free(tapi_geo_track_error *e)
{
    free(e->errors);
    free(e->times);
    e->errors = e->times = NULL;
    e->n_errors = 0;
}

static void
err_read(tapi_geo_res *r, tapi_geo_track_error *e)
{
    char *cur = r->report.ptr;
    char *f[4];
    unsigned int cap = 0;

    e->n = tapi_geo_res_int(r, "n", 0);
    e->unmatched = tapi_geo_res_int(r, "unmatched", 0);
    e->over = tapi_geo_res_int(r, "over_threshold",
                               tapi_geo_res_int(r, "off_route", 0));
    e->mean = tapi_geo_res_dbl(r, "mean");
    e->max = tapi_geo_res_dbl(r, "max");
    e->rmse = tapi_geo_res_dbl(r, "rmse");
    e->p50 = tapi_geo_res_dbl(r, "p50");
    e->p90 = tapi_geo_res_dbl(r, "p90");
    e->p95 = tapi_geo_res_dbl(r, "p95");
    e->p99 = tapi_geo_res_dbl(r, "p99");
    while (tapi_geo_report_next(&cur, f, 4) == 3)
    {
        if (e->n_errors == cap)
        {
            double *ne, *nt;

            cap = cap == 0 ? 256 : cap * 2;
            ne = realloc(e->errors, cap * sizeof(*ne));
            if (ne == NULL)
                break;
            e->errors = ne;
            nt = realloc(e->times, cap * sizeof(*nt));
            if (nt == NULL)
                break;
            e->times = nt;
        }
        e->times[e->n_errors] = strtod(f[1], NULL);
        e->errors[e->n_errors] = strtod(f[2], NULL);
        e->n_errors++;
    }
}

/* See description in tapi_geo_track.h */
te_errno
tapi_geo_track_error_vs(rcf_rpc_server *rpcs, const tapi_geo_track *track,
                        const tapi_geo_track *ref, double threshold,
                        tapi_geo_track_error *e)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    char p[64] = "";
    te_errno rc;
    int hr;

    memset(e, 0, sizeof(*e));
    if ((rc = tr_handle(rpcs, ref, &hr)) != 0)
        return rc;
    if (isfinite(threshold))
        snprintf(p, sizeof(p), "threshold=%.17g", threshold);
    rc = eval(rpcs, track, hr, "error", p, &r);
    if (rc == 0)
        err_read(&r, e);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo_track.h */
te_errno
tapi_geo_track_route(rcf_rpc_server *rpcs, const tapi_geo_track *track,
                     const tapi_geo_geometry *route, double tolerance,
                     tapi_geo_track_error *e)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    char p[64] = "";
    te_errno rc;
    int hr;

    memset(e, 0, sizeof(*e));
    if ((rc = tgeo_handle(rpcs, route, &hr)) != 0)
        return rc;
    if (isfinite(tolerance))
        snprintf(p, sizeof(p), "tolerance=%.17g", tolerance);
    rc = eval(rpcs, track, hr, "route", p, &r);
    if (rc == 0)
        err_read(&r, e);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo_track.h */
te_errno
tapi_geo_track_geofence(rcf_rpc_server *rpcs, const tapi_geo_track *track,
                        const tapi_geo_geometry *fence, const char *expect,
                        tapi_geo_geofence *g)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    char p[64];
    te_errno rc;
    int hf;

    memset(g, 0, sizeof(*g));
    g->events = (te_string)TE_STRING_INIT;
    if ((rc = tgeo_handle(rpcs, fence, &hf)) != 0)
        return rc;
    snprintf(p, sizeof(p), "expect=%s", expect != NULL ? expect : "inside");
    rc = eval(rpcs, track, hf, "geofence", p, &r);
    if (rc == 0)
    {
        g->samples = tapi_geo_res_int(&r, "samples", 0);
        g->inside = tapi_geo_res_int(&r, "inside", 0);
        g->outside = tapi_geo_res_int(&r, "outside", 0);
        g->transitions = tapi_geo_res_int(&r, "transitions", 0);
        g->violations = tapi_geo_res_int(&r, "violations", 0);
        g->first_violation_t = tapi_geo_res_dbl(&r, "first_violation_t");
        g->ok = tapi_geo_res_int(&r, "ok", 0) != 0;
        if (r.report.ptr != NULL)
            te_string_append(&g->events, "%s", r.report.ptr);
    }
    tapi_geo_rpc_res_free(&r);
    return rc;
}
