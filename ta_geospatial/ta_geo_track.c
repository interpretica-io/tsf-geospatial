/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial engine: location observations
 *
 * A track is a series of normalised observations - time in seconds,
 * longitude and latitude in degrees, optional height - loaded from text
 * or from a dataset layer. GNSS protocols are not decoded here: the
 * system under test's output is normalised before it comes in.
 *
 * Evaluations: sanity (ordering, gaps, jumps, impossible speeds),
 * error against a reference track (interpolated in time), distance to a
 * reference route, geofence. All distances are geodesic on the track's
 * own datum.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ta_geo_int.h"

static int
track_push(geo_track *t, size_t *cap, double tt, double x, double y,
           double z, char **err)
{
    if (t->n == *cap)
    {
        size_t nc = *cap == 0 ? 1024 : *cap * 2;
        double *a = realloc(t->t, nc * sizeof(double));
        double *b = a != NULL ? realloc(t->x, nc * sizeof(double)) : NULL;
        double *c = b != NULL ? realloc(t->y, nc * sizeof(double)) : NULL;
        double *d = c != NULL ? realloc(t->z, nc * sizeof(double)) : NULL;

        if (a != NULL) t->t = a;
        if (b != NULL) t->x = b;
        if (c != NULL) t->y = c;
        if (d != NULL) t->z = d;
        if (d == NULL)
            return seterr(err, TA_GEO_ENOMEM, "out of memory");
        *cap = nc;
    }
    t->t[t->n] = tt;
    t->x[t->n] = x;
    t->y[t->n] = y;
    t->z[t->n] = z;
    t->n++;
    if ((long long)t->n > geo_lim.max_features)
        return seterr(err, TA_GEO_E2BIG, "more than max_features samples");
    return TA_GEO_OK;
}

/* Bring the positions to (lon, lat) degrees of the CRS's datum. */
static int
track_normalise(geo_track *t, const char *crs, const char *axis, char **err)
{
    GEOSContextHandle_t h = geo_geos();
    GEOSCoordSequence *s;
    GEOSGeometry *line, *ll = NULL;
    PJ *geod = NULL;
    const GEOSCoordSequence *o;
    size_t i;
    int rc;

    if (t->n == 0)
        return TA_GEO_OK;
    /* A multipoint would do, a coordinate sequence in a linestring is
     * cheaper; one sample makes a degenerate line, which is fine here. */
    s = GEOSCoordSeq_create_r(h, (unsigned int)(t->n < 2 ? 2 : t->n), 2);
    for (i = 0; i < t->n; i++)
        GEOSCoordSeq_setXY_r(h, s, (unsigned int)i, t->x[i], t->y[i]);
    if (t->n < 2)
        GEOSCoordSeq_setXY_r(h, s, 1, t->x[0], t->y[0]);
    line = GEOSGeom_createLineString_r(h, s);
    rc = geo_lonlat(line, crs, axis, &ll, &geod, err);
    GEOSGeom_destroy_r(h, line);
    proj_destroy(geod);
    if (rc != 0)
        return rc;
    o = GEOSGeom_getCoordSeq_r(h, ll);
    for (i = 0; i < t->n; i++)
        GEOSCoordSeq_getXY_r(h, o, (unsigned int)i, &t->x[i], &t->y[i]);
    GEOSGeom_destroy_r(h, ll);
    return TA_GEO_OK;
}

static int
track_load(geo_req *r)
{
    geo_track *t = calloc(1, sizeof(*t));
    const char *crs = kv_str(r->opt, "crs", "EPSG:4326");
    const char *axis = kv_get(r->opt, "axis");
    const char *k = *r->kind != '\0' ? r->kind : "text";
    size_t cap = 0;
    int rc = TA_GEO_OK;

    if (t == NULL)
        return seterr(r->err, TA_GEO_ENOMEM, "out of memory");
    if (axis == NULL)
    {
        free(t);
        return seterr(r->err, TA_GEO_EINVAL, "say axis=gis or "
                      "axis=authority for the positions");
    }
    if (strcmp(k, "text") == 0)
    {
        const char *p = r->text;
        long long line = 0;

        while (*p != '\0' && rc == 0)
        {
            const char *eol = strchr(p, '\n');
            double v[4] = { 0, 0, 0, NAN };
            int got;

            line++;
            got = sscanf(p, "%lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3]);
            if (got >= 3)
            {
                rc = track_push(t, &cap, v[0], v[1], v[2], v[3], r->err);
                if (got == 4)
                    t->has_z = true;
            }
            else if (strspn(p, " \t\r") != (size_t)(eol != NULL ? eol - p :
                                                     (long)strlen(p)))
                rc = seterr(r->err, TA_GEO_ECORRUPT, "line %lld is not "
                            "'t x y [z]'", line);
            if (eol == NULL)
                break;
            p = eol + 1;
        }
    }
    else if (strcmp(k, "dataset") == 0)
    {
        geo_ds *d = geo_obj_get(r->ha, GEO_O_DS, r->err);
        const char *tf = kv_str(r->opt, "time_field", "t");
        const char *ln = kv_get(r->opt, "layer");
        OGRLayerH lay = NULL;
        OGRFeatureH f;
        int ti;

        if (d == NULL)
            rc = TA_GEO_EINVAL;
        else
        {
            lay = ln != NULL ? GDALDatasetGetLayerByName(d->ds, ln) :
                               GDALDatasetGetLayer(d->ds, 0);
            if (lay == NULL)
                rc = seterr(r->err, TA_GEO_ENOENT, "no layer");
        }
        if (rc == 0)
        {
            ti = OGR_FD_GetFieldIndex(OGR_L_GetLayerDefn(lay), tf);
            if (ti < 0)
                rc = seterr(r->err, TA_GEO_ENOENT, "no time field '%s'", tf);
            OGR_L_ResetReading(lay);
            while (rc == 0 && (f = OGR_L_GetNextFeature(lay)) != NULL)
            {
                OGRGeometryH g = OGR_F_GetGeometryRef(f);
                OGRFieldType ft = OGR_Fld_GetType(
                    OGR_FD_GetFieldDefn(OGR_F_GetDefnRef(f), ti));
                double tt;

                if (ft == OFTDateTime || ft == OFTDate)
                {
                    int y, mo, dd, hh, mi, tz;
                    float sec;
                    struct tm tmv;

                    OGR_F_GetFieldAsDateTimeEx(f, ti, &y, &mo, &dd, &hh, &mi,
                                               &sec, &tz);
                    memset(&tmv, 0, sizeof(tmv));
                    tmv.tm_year = y - 1900;
                    tmv.tm_mon = mo - 1;
                    tmv.tm_mday = dd;
                    tmv.tm_hour = hh;
                    tmv.tm_min = mi;
                    tt = (double)timegm(&tmv) + sec;
                    /* tz: 100 = UTC, 1 = local (taken as UTC), else
                     * 100 + quarter hours east. */
                    if (tz > 1 && tz != 100)
                        tt -= (tz - 100) * 900.0;
                }
                else
                    tt = OGR_F_GetFieldAsDouble(f, ti);
                if (g != NULL && wkbFlatten(OGR_G_GetGeometryType(g)) ==
                                 wkbPoint && !OGR_G_IsEmpty(g))
                {
                    rc = track_push(t, &cap, tt, OGR_G_GetX(g, 0),
                                    OGR_G_GetY(g, 0),
                                    OGR_G_Is3D(g) ? OGR_G_GetZ(g, 0) : NAN,
                                    r->err);
                    t->has_z = t->has_z || OGR_G_Is3D(g);
                }
                OGR_F_Destroy(f);
            }
        }
    }
    else
        rc = seterr(r->err, TA_GEO_EINVAL, "load a track from text or "
                    "dataset");
    if (rc == 0)
        rc = track_normalise(t, crs, axis, r->err);
    if (rc == 0)
    {
        geo_crs_norm(crs, t->crs, sizeof(t->crs));
        snprintf(t->axis, sizeof(t->axis), "%s", axis);
        rc = geo_obj_add(GEO_O_TRACK, t, r->handle, r->err);
    }
    if (rc != 0)
    {
        geo_track_free(t);
        return rc;
    }
    sb_kv_int(r->res, "samples", (long long)t->n);
    sb_kv_str(r->res, "crs", t->crs);
    sb_kv_int(r->res, "has_z", t->has_z);
    if (t->n > 0)
    {
        sb_kv_num(r->res, "t_first", t->t[0]);
        sb_kv_num(r->res, "t_last", t->t[t->n - 1]);
    }
    return TA_GEO_OK;
}

/* ---- statistics ---------------------------------------------------- */

static int
cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;

    return x < y ? -1 : x > y;
}

/* Error statistics: mean, max, RMSE and percentiles (nearest rank). */
static void
err_stats(sbuf *res, double *e, size_t n, const char *pct)
{
    double sum = 0, sq = 0, mx = 0;
    size_t i;
    char buf[256];
    char *tok;
    char *save = NULL;

    for (i = 0; i < n; i++)
    {
        sum += e[i];
        sq += e[i] * e[i];
        if (e[i] > mx)
            mx = e[i];
    }
    sb_kv_int(res, "n", (long long)n);
    sb_kv_num(res, "mean", n > 0 ? sum / (double)n : NAN);
    sb_kv_num(res, "max", n > 0 ? mx : NAN);
    sb_kv_num(res, "rmse", n > 0 ? sqrt(sq / (double)n) : NAN);
    qsort(e, n, sizeof(*e), cmp_dbl);
    snprintf(buf, sizeof(buf), "%s", pct);
    for (tok = strtok_r(buf, ",", &save); tok != NULL;
         tok = strtok_r(NULL, ",", &save))
    {
        double p = atof(tok);
        char key[32];
        size_t rank;

        if (!(p > 0 && p <= 100) || n == 0)
            continue;
        rank = (size_t)ceil(p / 100.0 * (double)n);
        if (rank < 1)
            rank = 1;
        snprintf(key, sizeof(key), "p%s", tok);
        sb_kv_num(res, key, e[rank - 1]);
    }
}

static geo_geod_ctx *
track_geod(geo_track *t, char **err)
{
    PJ *crs = geo_crs_create(t->crs, err);
    PJ *g;
    geo_geod_ctx *c;

    if (crs == NULL)
        return NULL;
    g = geo_crs_is_geographic(crs) ? crs :
        proj_crs_get_geodetic_crs(geo_proj(), crs);
    c = g != NULL ? geo_geod_new(g, err) : NULL;
    if (g != crs)
        proj_destroy(g);
    proj_destroy(crs);
    return c;
}

static int
eval_sanity(geo_req *r, geo_track *t, geo_geod_ctx *c)
{
    double max_speed, max_gap, max_jump, interval;
    long long out_of_order = 0, dup_t = 0, gaps = 0, missing = 0,
              jumps = 0, impossible = 0, events = 0;
    double length = 0, vmax = 0;
    size_t i;
    int rc;

#define EV(_i, _k, _v) \
    do {                                                            \
        if (events++ < geo_lim.max_report)                          \
        {                                                           \
            sb_printf(r->rep, "%zu\t", (size_t)(_i));               \
            sb_num(r->rep, t->t[_i]);                               \
            sb_printf(r->rep, "\t%s\t", (_k));                      \
            sb_num(r->rep, (_v));                                   \
            sb_puts(r->rep, "\n");                                  \
        }                                                           \
    } while (0)

    if ((rc = kv_dbl(r->opt, "max_speed", INFINITY, &max_speed,
                     r->err)) != 0 ||
        (rc = kv_dbl(r->opt, "max_gap", INFINITY, &max_gap, r->err)) != 0 ||
        (rc = kv_dbl(r->opt, "max_jump", INFINITY, &max_jump, r->err)) != 0 ||
        (rc = kv_dbl(r->opt, "interval", 0, &interval, r->err)) != 0)
        return rc;
    for (i = 1; i < t->n; i++)
    {
        double dt = t->t[i] - t->t[i - 1];
        double d = geo_geod_dist(c, t->x[i - 1], t->y[i - 1], t->x[i],
                                 t->y[i]);
        double v;

        length += d;
        if (dt < 0)
        {
            out_of_order++;
            EV(i, "out_of_order", dt);
            continue;
        }
        if (dt == 0)
        {
            dup_t++;
            EV(i, "duplicate_time", d);
            continue;
        }
        v = d / dt;
        if (v > vmax)
            vmax = v;
        if (dt > max_gap)
        {
            gaps++;
            EV(i, "gap", dt);
        }
        if (interval > 0 && dt > 1.5 * interval)
            missing += (long long)llround(dt / interval) - 1;
        if (d > max_jump)
        {
            jumps++;
            EV(i, "jump", d);
        }
        if (v > max_speed)
        {
            impossible++;
            EV(i, "impossible_speed", v);
        }
    }
#undef EV
    sb_kv_int(r->res, "samples", (long long)t->n);
    sb_kv_int(r->res, "out_of_order", out_of_order);
    sb_kv_int(r->res, "duplicate_time", dup_t);
    sb_kv_int(r->res, "gaps", gaps);
    sb_kv_int(r->res, "missing", missing);
    sb_kv_int(r->res, "jumps", jumps);
    sb_kv_int(r->res, "impossible", impossible);
    sb_kv_num(r->res, "length_m", length);
    sb_kv_num(r->res, "duration_s", t->n > 1 ? t->t[t->n - 1] - t->t[0] : 0);
    sb_kv_num(r->res, "max_speed_ms", vmax);
    sb_kv_num(r->res, "mean_speed_ms", t->n > 1 &&
              t->t[t->n - 1] > t->t[0] ?
              length / (t->t[t->n - 1] - t->t[0]) : 0);
    sb_kv_int(r->res, "ok", out_of_order + dup_t + gaps + jumps +
                            impossible == 0);
    return TA_GEO_OK;
}

/* Reference position at time tt, by linear interpolation in lon/lat
 * between the bracketing samples (they are close together). */
static bool
ref_at(const geo_track *ref, double tt, size_t *hint, double *x, double *y)
{
    size_t i = *hint;
    double f;

    if (ref->n == 0 || tt < ref->t[0] || tt > ref->t[ref->n - 1])
        return false;
    if (i >= ref->n || ref->t[i] > tt)
        i = 0;
    while (i + 1 < ref->n && ref->t[i + 1] < tt)
        i++;
    *hint = i;
    if (i + 1 >= ref->n || ref->t[i + 1] == ref->t[i])
    {
        *x = ref->x[i];
        *y = ref->y[i];
        return true;
    }
    f = (tt - ref->t[i]) / (ref->t[i + 1] - ref->t[i]);
    *x = ref->x[i] + f * (ref->x[i + 1] - ref->x[i]);
    *y = ref->y[i] + f * (ref->y[i + 1] - ref->y[i]);
    return true;
}

static int
eval_error(geo_req *r, geo_track *t, geo_geod_ctx *c)
{
    geo_track *ref = geo_obj_get(r->hb, GEO_O_TRACK, r->err);
    double *e;
    size_t n = 0, i, hint = 0;
    long long unmatched = 0;
    double thr;
    long long over = 0;
    int rc;

    if (ref == NULL)
        return TA_GEO_EINVAL;
    if ((rc = kv_dbl(r->opt, "threshold", INFINITY, &thr, r->err)) != 0)
        return rc;
    e = malloc((t->n > 0 ? t->n : 1) * sizeof(*e));
    if (e == NULL)
        return seterr(r->err, TA_GEO_ENOMEM, "out of memory");
    for (i = 0; i < t->n; i++)
    {
        double rx, ry;

        if (!ref_at(ref, t->t[i], &hint, &rx, &ry))
        {
            unmatched++;
            continue;
        }
        e[n] = geo_geod_dist(c, t->x[i], t->y[i], rx, ry);
        if (e[n] > thr)
            over++;
        if ((long long)n < geo_lim.max_report)
        {
            sb_printf(r->rep, "%zu\t", i);
            sb_num(r->rep, t->t[i]);
            sb_puts(r->rep, "\t");
            sb_num(r->rep, e[n]);
            sb_puts(r->rep, "\n");
        }
        n++;
    }
    err_stats(r->res, e, n, kv_str(r->opt, "percentiles", "50,90,95,99"));
    free(e);
    sb_kv_int(r->res, "unmatched", unmatched);
    sb_kv_int(r->res, "over_threshold", over);
    return TA_GEO_OK;
}

static int
eval_route(geo_req *r, geo_track *t, geo_geod_ctx *c)
{
    geo_geom *route = geo_obj_get(r->hb, GEO_O_GEOM, r->err);
    GEOSGeometry *ll = NULL;
    PJ *geod = NULL;
    double *e;
    double tol;
    long long off = 0;
    size_t i;
    int rc;

    if (route == NULL)
        return TA_GEO_EINVAL;
    if ((rc = kv_dbl(r->opt, "tolerance", INFINITY, &tol, r->err)) != 0)
        return rc;
    rc = geo_lonlat(route->g, route->crs[0] != '\0' ? route->crs : t->crs,
                    kv_str(r->opt, "route_axis", t->axis), &ll, &geod,
                    r->err);
    proj_destroy(geod);
    if (rc != 0)
        return rc;
    e = malloc((t->n > 0 ? t->n : 1) * sizeof(*e));
    if (e == NULL)
    {
        GEOSGeom_destroy_r(geo_geos(), ll);
        return seterr(r->err, TA_GEO_ENOMEM, "out of memory");
    }
    for (i = 0; i < t->n; i++)
    {
        if ((rc = geo_geod_point_dist(c, t->x[i], t->y[i], ll, &e[i],
                                      r->err)) != 0)
            break;
        if (e[i] > tol)
            off++;
        if ((long long)i < geo_lim.max_report)
        {
            sb_printf(r->rep, "%zu\t", i);
            sb_num(r->rep, t->t[i]);
            sb_puts(r->rep, "\t");
            sb_num(r->rep, e[i]);
            sb_puts(r->rep, "\n");
        }
        if (geo_deadline_passed())
        {
            rc = seterr(r->err, TA_GEO_ETIMEDOUT, "route evaluation ran "
                        "past the deadline");
            break;
        }
    }
    GEOSGeom_destroy_r(geo_geos(), ll);
    if (rc == 0)
    {
        err_stats(r->res, e, t->n, kv_str(r->opt, "percentiles",
                                          "50,90,95,99"));
        sb_kv_int(r->res, "off_route", off);
    }
    free(e);
    return rc;
}

static int
eval_geofence(geo_req *r, geo_track *t)
{
    GEOSContextHandle_t h = geo_geos();
    geo_geom *fence = geo_obj_get(r->hb, GEO_O_GEOM, r->err);
    const char *expect = kv_str(r->opt, "expect", "inside");
    GEOSGeometry *ll = NULL;
    const GEOSPreparedGeometry *pg;
    PJ *geod = NULL;
    long long inside = 0, outside = 0, transitions = 0, violations = 0;
    double first_v = NAN;
    int prev = -1;
    bool want_in;
    size_t i;
    int rc;

    if (fence == NULL)
        return TA_GEO_EINVAL;
    if (strcmp(expect, "inside") != 0 && strcmp(expect, "outside") != 0 &&
        strcmp(expect, "any") != 0)
        return seterr(r->err, TA_GEO_EINVAL, "expect is inside, outside or "
                      "any");
    want_in = strcmp(expect, "inside") == 0;
    rc = geo_lonlat(fence->g, fence->crs[0] != '\0' ? fence->crs : t->crs,
                    kv_str(r->opt, "fence_axis", t->axis), &ll, &geod,
                    r->err);
    proj_destroy(geod);
    if (rc != 0)
        return rc;
    pg = GEOSPrepare_r(h, ll);
    for (i = 0; i < t->n; i++)
    {
        GEOSGeometry *p = GEOSGeom_createPointFromXY_r(h, t->x[i], t->y[i]);
        int in = GEOSPreparedCovers_r(h, pg, p) == 1;

        GEOSGeom_destroy_r(h, p);
        in ? inside++ : outside++;
        if (prev >= 0 && in != prev)
        {
            transitions++;
            if (transitions <= geo_lim.max_report)
            {
                sb_printf(r->rep, "%zu\t", i);
                sb_num(r->rep, t->t[i]);
                sb_printf(r->rep, "\t%s\n", in ? "enter" : "exit");
            }
        }
        if (strcmp(expect, "any") != 0 && in != want_in)
        {
            if (violations++ == 0)
                first_v = t->t[i];
        }
        prev = in;
    }
    GEOSPreparedGeom_destroy_r(h, pg);
    GEOSGeom_destroy_r(h, ll);
    sb_kv_int(r->res, "samples", (long long)t->n);
    sb_kv_int(r->res, "inside", inside);
    sb_kv_int(r->res, "outside", outside);
    sb_kv_int(r->res, "transitions", transitions);
    sb_kv_int(r->res, "violations", violations);
    sb_kv_num(r->res, "first_violation_t", first_v);
    sb_kv_int(r->res, "ok", violations == 0);
    return TA_GEO_OK;
}

static int
track_eval(geo_req *r)
{
    geo_track *t = geo_obj_get(r->ha, GEO_O_TRACK, r->err);
    geo_geod_ctx *c;
    const char *k = *r->kind != '\0' ? r->kind : "sanity";
    int rc;

    if (t == NULL)
        return TA_GEO_EINVAL;
    if (strcmp(k, "geofence") == 0)
        return eval_geofence(r, t);
    c = track_geod(t, r->err);
    if (c == NULL)
        return TA_GEO_EFAIL;
    if (strcmp(k, "sanity") == 0)
        rc = eval_sanity(r, t, c);
    else if (strcmp(k, "error") == 0)
        rc = eval_error(r, t, c);
    else if (strcmp(k, "route") == 0)
        rc = eval_route(r, t, c);
    else
        rc = seterr(r->err, TA_GEO_EINVAL, "unknown evaluation '%s' (sanity, "
                    "error, route, geofence)", k);
    geo_geod_free(c);
    return rc;
}

int
geo_op_track(ta_geo_op op, geo_req *r)
{
    if (op == TA_GEO_OP_TRACK_LOAD)
        return track_load(r);
    if (op == TA_GEO_OP_TRACK_EVAL)
        return track_eval(r);
    return seterr(r->err, TA_GEO_EINVAL, "not a track operation");
}
