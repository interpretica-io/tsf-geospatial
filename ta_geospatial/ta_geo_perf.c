/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial engine: throughput and latency
 *
 * The loop runs on the agent, so an RPC round trip is not what gets
 * measured. Every measurement separates the cold first call (contexts,
 * caches, the PROJ database, operation selection) from the warm ones
 * after it, and reports the input's complexity next to the time.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ta_geo_int.h"

static void
put_times(sbuf *res, long long n, double cold_us, double warm_total_us,
          long long work)
{
    double warm = n > 1 ? warm_total_us / (double)(n - 1) : NAN;

    sb_kv_int(res, "iterations", n);
    sb_kv_num(res, "cold_us", cold_us);
    sb_kv_num(res, "warm_us", warm);
    sb_kv_num(res, "ops_per_s", warm > 0 ? 1e6 / warm : 0);
    sb_kv_num(res, "items_per_s", warm > 0 ? (double)work * 1e6 / warm : 0);
    sb_kv_int(res, "work_items", work);
}

static int
perf_parse(geo_req *r, long long n)
{
    GEOSContextHandle_t h = geo_geos();
    long long i;
    double cold = 0, warm = 0;
    long long verts = 0;
    int rc;

    for (i = 0; i < n; i++)
    {
        GEOSGeometry *g = NULL;
        int srid;
        uint64_t t0 = geo_now_ns();

        rc = geo_parse_text(*r->kind != '\0' && strcmp(r->kind, "parse") != 0 ?
                            r->kind : "auto", r->text, &geo_lim, &g, &srid,
                            r->err);
        if (rc != 0)
            return rc;
        if (i == 0)
        {
            cold = geo_us_since(t0);
            verts = geo_npoints(g);
        }
        else
            warm += geo_us_since(t0);
        GEOSGeom_destroy_r(h, g);
    }
    put_times(r->res, n, cold, warm, verts);
    sb_kv_int(r->res, "vertices", verts);
    return TA_GEO_OK;
}

static int
perf_predicate(geo_req *r, long long n, bool prepared)
{
    GEOSContextHandle_t h = geo_geos();
    geo_geom *a = geo_obj_get(r->ha, GEO_O_GEOM, r->err);
    geo_geom *b = a != NULL ? geo_obj_get(r->hb, GEO_O_GEOM, r->err) : NULL;
    const char *pred = kv_str(r->opt, "predicate", "intersects");
    const GEOSPreparedGeometry *pg = NULL;
    double cold = 0, warm = 0;
    long long i;
    long long hits = 0;

    if (a == NULL || b == NULL)
        return TA_GEO_EINVAL;
    if (strcmp(pred, "intersects") != 0 && strcmp(pred, "contains") != 0)
        return seterr(r->err, TA_GEO_EINVAL, "perf predicate is intersects "
                      "or contains");
    for (i = 0; i < n; i++)
    {
        uint64_t t0 = geo_now_ns();
        char v;

        if (prepared)
        {
            if (pg == NULL)
                pg = GEOSPrepare_r(h, a->g);
            v = strcmp(pred, "intersects") == 0 ?
                GEOSPreparedIntersects_r(h, pg, b->g) :
                GEOSPreparedContains_r(h, pg, b->g);
        }
        else
            v = strcmp(pred, "intersects") == 0 ?
                GEOSIntersects_r(h, a->g, b->g) :
                GEOSContains_r(h, a->g, b->g);
        if (v == 2)
        {
            if (pg != NULL)
                GEOSPreparedGeom_destroy_r(h, pg);
            return seterr(r->err, TA_GEO_EFAIL, "%s: %s", pred,
                          geo_geos_err());
        }
        hits += v;
        if (i == 0)
            cold = geo_us_since(t0);
        else
            warm += geo_us_since(t0);
    }
    if (pg != NULL)
        GEOSPreparedGeom_destroy_r(h, pg);
    put_times(r->res, n, cold, warm, 1);
    sb_kv_int(r->res, "vertices", geo_npoints(a->g) + geo_npoints(b->g));
    sb_kv_int(r->res, "hits", hits);
    sb_kv_int(r->res, "prepared", prepared);
    return TA_GEO_OK;
}

/* Transform "count" generated points per iteration; the first
 * iteration includes creating the operation (the cold cost). */
static int
perf_transform(geo_req *r, long long n)
{
    const char *src = kv_get(r->opt, "src");
    const char *dst = kv_get(r->opt, "dst");
    const char *axis = kv_str(r->opt, "axis", "gis");
    long long count;
    double b[4];
    const char *bs = kv_str(r->opt, "bounds", "-9 37 -6 42");
    double *x, *y;
    double cold = 0, warm = 0;
    geo_rng rng;
    PJ *op = NULL;
    long long i, k;
    int rc;

    if (src == NULL || dst == NULL)
        return seterr(r->err, TA_GEO_EINVAL, "transform perf needs src and "
                      "dst");
    if ((rc = kv_long(r->opt, "count", 10000, &count, r->err)) != 0)
        return rc;
    if (count < 1 || count > 100000000 ||
        sscanf(bs, "%lf %lf %lf %lf", &b[0], &b[1], &b[2], &b[3]) != 4)
        return seterr(r->err, TA_GEO_EINVAL, "bad count or bounds");
    x = malloc((size_t)count * sizeof(*x));
    y = malloc((size_t)count * sizeof(*y));
    if (x == NULL || y == NULL)
    {
        free(x);
        free(y);
        return seterr(r->err, TA_GEO_ENOMEM, "out of memory");
    }
    for (i = 0; i < n; i++)
    {
        uint64_t t0;

        geo_rng_seed(&rng, 1);
        for (k = 0; k < count; k++)
        {
            x[k] = geo_rng_range(&rng, b[0], b[2]);
            y[k] = geo_rng_range(&rng, b[1], b[3]);
        }
        t0 = geo_now_ns();
        if (op == NULL)
        {
            char s1[256], s2[256];
            PJ *raw;

            geo_crs_norm(src, s1, sizeof(s1));
            geo_crs_norm(dst, s2, sizeof(s2));
            raw = proj_create_crs_to_crs(geo_proj(), s1, s2, NULL);
            if (raw == NULL)
            {
                free(x);
                free(y);
                return seterr(r->err, TA_GEO_ENOENT, "no operation %s -> "
                              "%s: %s", s1, s2, geo_proj_err());
            }
            op = strcmp(axis, "gis") == 0 ?
                 proj_normalize_for_visualization(geo_proj(), raw) :
                 proj_clone(geo_proj(), raw);
            proj_destroy(raw);
        }
        proj_trans_generic(op, PJ_FWD, x, sizeof(double), (size_t)count, y,
                           sizeof(double), (size_t)count, NULL, 0, 0, NULL,
                           0, 0);
        if (i == 0)
            cold = geo_us_since(t0);
        else
            warm += geo_us_since(t0);
    }
    proj_destroy(op);
    free(x);
    free(y);
    put_times(r->res, n, cold, warm, count);
    sb_kv_int(r->res, "points", count);
    return TA_GEO_OK;
}

static int
perf_buffer(geo_req *r, long long n)
{
    GEOSContextHandle_t h = geo_geos();
    geo_geom *a = geo_obj_get(r->ha, GEO_O_GEOM, r->err);
    double dist;
    double cold = 0, warm = 0;
    long long i;
    long long out = 0;
    int rc;

    if (a == NULL)
        return TA_GEO_EINVAL;
    if ((rc = kv_dbl(r->opt, "distance", 1, &dist, r->err)) != 0)
        return rc;
    for (i = 0; i < n; i++)
    {
        uint64_t t0 = geo_now_ns();
        GEOSGeometry *g = GEOSBuffer_r(h, a->g, dist, 8);

        if (g == NULL)
            return seterr(r->err, geo_deadline_passed() ? TA_GEO_ETIMEDOUT :
                          TA_GEO_EFAIL, "buffer: %s", geo_geos_err());
        if (i == 0)
        {
            cold = geo_us_since(t0);
            out = geo_npoints(g);
        }
        else
            warm += geo_us_since(t0);
        GEOSGeom_destroy_r(h, g);
    }
    put_times(r->res, n, cold, warm, geo_npoints(a->g));
    sb_kv_int(r->res, "vertices", geo_npoints(a->g));
    sb_kv_int(r->res, "output_vertices", out);
    return TA_GEO_OK;
}

int
geo_op_perf(geo_req *r)
{
    long long n;
    int rc;
    const char *what = kv_str(r->opt, "what", "parse");

    if ((rc = kv_long(r->opt, "iterations", 100, &n, r->err)) != 0)
        return rc;
    if (n < 1 || n > 10000000)
        return seterr(r->err, TA_GEO_EINVAL, "1 <= iterations <= 1e7");
    if (strcmp(what, "parse") == 0)
        return perf_parse(r, n);
    if (strcmp(what, "predicate") == 0)
        return perf_predicate(r, n, false);
    if (strcmp(what, "prepared_predicate") == 0)
        return perf_predicate(r, n, true);
    if (strcmp(what, "transform") == 0)
        return perf_transform(r, n);
    if (strcmp(what, "buffer") == 0)
        return perf_buffer(r, n);
    return seterr(r->err, TA_GEO_EINVAL, "unknown measurement '%s' (parse, "
                  "predicate, prepared_predicate, transform, buffer)", what);
}
