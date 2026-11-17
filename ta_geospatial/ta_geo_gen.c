/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial engine: deterministic test inputs
 *
 * Every generator is a pure function of its options and seed: the same
 * options give the same items, byte for byte, on any agent. Identifiers
 * are start_id, start_id + 1, ... in output order.
 *
 * Validity guarantees (each item says what it is in "expect"):
 *  - points, grid, boundary, lines, trajectory: valid by construction.
 *  - polygons: star-shaped around their centre with strictly increasing
 *    vertex angles, so every ring is simple; a hole, when asked for and
 *    the shell has 4 or more vertices, is a star around the same centre
 *    inside the circle the shell's edges never cross, wound the other
 *    way. Always valid.
 *  - invalid: only with allow_invalid=1, and each item carries the
 *    defect category the validator must report for it.
 *  - outliers: valid points; "expect" is "outlier" for the planted ones.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ta_geo_int.h"

typedef struct gen {
    geo_gen_item *v;
    size_t n;
    size_t cap;
    long long next_id;
    bool oom;
} gen;

static void
add(gen *g, const char *fmt, const char *expect, double t, sbuf *text)
{
    geo_gen_item *nv;

    if (g->oom)
    {
        sb_free(text);
        return;
    }
    if (g->n == g->cap)
    {
        g->cap = g->cap == 0 ? 256 : g->cap * 2;
        nv = realloc(g->v, g->cap * sizeof(*nv));
        if (nv == NULL)
        {
            g->oom = true;
            sb_free(text);
            return;
        }
        g->v = nv;
    }
    g->v[g->n].id = g->next_id++;
    g->v[g->n].text = sb_take(text);
    snprintf(g->v[g->n].fmt, sizeof(g->v[g->n].fmt), "%s", fmt);
    snprintf(g->v[g->n].expect, sizeof(g->v[g->n].expect), "%s", expect);
    g->v[g->n].t = t;
    g->n++;
}

static void
xy(sbuf *b, double x, double y)
{
    sb_num(b, x);
    sb_puts(b, " ");
    sb_num(b, y);
}

static void
point(gen *g, double x, double y, const char *expect, double t)
{
    sbuf b = SBUF_INIT;

    sb_puts(&b, "POINT (");
    xy(&b, x, y);
    sb_puts(&b, ")");
    add(g, "wkt", expect, t, &b);
}

/* A star-shaped ring: k vertices at increasing angles. */
static void
star_ring(sbuf *b, geo_rng *r, double cx, double cy, double rmin,
          double rmax, int k, bool ccw)
{
    double *ang = malloc((size_t)k * sizeof(*ang));
    double *rad = malloc((size_t)k * sizeof(*rad));
    double fx = 0, fy = 0;
    int i;

    if (ang == NULL || rad == NULL)
    {
        free(ang);
        free(rad);
        b->oom = true;
        return;
    }
    /* Angles: k equal sectors, one jittered angle in the middle half of
     * each, so they strictly increase and no two neighbours are more
     * than 1.5 sectors apart. */
    for (i = 0; i < k; i++)
    {
        ang[i] = (i + 0.25 + 0.5 * geo_rng_unit(r)) * 2 * M_PI / k;
        rad[i] = geo_rng_range(r, rmin, rmax);
    }
    sb_puts(b, "(");
    for (i = 0; i < k; i++)
    {
        int j = ccw ? i : k - 1 - i;
        double x = cx + rad[j] * cos(ang[j]);
        double y = cy + rad[j] * sin(ang[j]);

        if (i == 0)
            fx = x, fy = y;
        else
            sb_puts(b, ", ");
        xy(b, x, y);
    }
    sb_puts(b, ", ");
    xy(b, fx, fy);
    sb_puts(b, ")");
    free(ang);
    free(rad);
}

static int
read_bounds(kv *o, double b[4], char **err)
{
    const char *s = kv_str(o, "bounds", "-10 -10 10 10");

    if (sscanf(s, "%lf %lf %lf %lf", &b[0], &b[1], &b[2], &b[3]) != 4 ||
        !(b[0] < b[2]) || !(b[1] < b[3]))
        return seterr(err, TA_GEO_EINVAL, "bounds is 'minx miny maxx maxy' "
                      "with min < max");
    return TA_GEO_OK;
}

static int
gen_invalid(gen *g, kv *o, geo_rng *r, const double b[4], long long count,
            char **err)
{
    static const char *const all = "bowtie,ring_self_intersection,"
        "hole_outside,nested_holes,unclosed,degenerate,non_finite,"
        "duplicate_vertex,empty,disconnected_interior";
    const char *list = kv_str(o, "kinds", all);
    char buf[512];
    char *tok;
    char *save = NULL;
    long long i;
    int nk = 0;
    const char *kinds[16];

    snprintf(buf, sizeof(buf), "%s", list);
    for (tok = strtok_r(buf, ",", &save); tok != NULL && nk < 16;
         tok = strtok_r(NULL, ",", &save))
        kinds[nk++] = tok;
    if (nk == 0)
        return seterr(err, TA_GEO_EINVAL, "no invalid kinds");
    for (i = 0; i < count; i++)
    {
        const char *k = kinds[i % nk];
        double s = (b[2] - b[0]) / 20 * geo_rng_range(r, 0.5, 1);
        double x = geo_rng_range(r, b[0], b[2] - 3 * s);
        double y = geo_rng_range(r, b[1], b[3] - 3 * s);
        sbuf t = SBUF_INIT;

#define P(_x, _y) xy(&t, (_x), (_y))
        if (strcmp(k, "bowtie") == 0)
        {
            sb_puts(&t, "POLYGON ((");
            P(x, y); sb_puts(&t, ", "); P(x + s, y + s); sb_puts(&t, ", ");
            P(x + s, y); sb_puts(&t, ", "); P(x, y + s); sb_puts(&t, ", ");
            P(x, y); sb_puts(&t, "))");
            add(g, "wkt", "self_intersection", 0, &t);
        }
        else if (strcmp(k, "ring_self_intersection") == 0)
        {
            /* The ring touches itself at one vertex (an inverted hole). */
            sb_puts(&t, "POLYGON ((");
            P(x, y); sb_puts(&t, ", "); P(x + 2 * s, y); sb_puts(&t, ", ");
            P(x + 2 * s, y + 2 * s); sb_puts(&t, ", "); P(x + s, y);
            sb_puts(&t, ", "); P(x, y + 2 * s); sb_puts(&t, ", "); P(x, y);
            sb_puts(&t, "))");
            add(g, "wkt", "ring_self_intersection", 0, &t);
        }
        else if (strcmp(k, "hole_outside") == 0)
        {
            sb_puts(&t, "POLYGON ((");
            P(x, y); sb_puts(&t, ", "); P(x + s, y); sb_puts(&t, ", ");
            P(x + s, y + s); sb_puts(&t, ", "); P(x, y + s);
            sb_puts(&t, ", "); P(x, y); sb_puts(&t, "), (");
            P(x + 2 * s, y + 2 * s); sb_puts(&t, ", ");
            P(x + 2 * s, y + 2.5 * s); sb_puts(&t, ", ");
            P(x + 2.5 * s, y + 2.5 * s); sb_puts(&t, ", ");
            P(x + 2 * s, y + 2 * s); sb_puts(&t, "))");
            add(g, "wkt", "hole_outside", 0, &t);
        }
        else if (strcmp(k, "nested_holes") == 0)
        {
            sb_puts(&t, "POLYGON ((");
            P(x, y); sb_puts(&t, ", "); P(x + 3 * s, y); sb_puts(&t, ", ");
            P(x + 3 * s, y + 3 * s); sb_puts(&t, ", "); P(x, y + 3 * s);
            sb_puts(&t, ", "); P(x, y); sb_puts(&t, "), (");
            P(x + 0.5 * s, y + 0.5 * s); sb_puts(&t, ", ");
            P(x + 0.5 * s, y + 2.5 * s); sb_puts(&t, ", ");
            P(x + 2.5 * s, y + 2.5 * s); sb_puts(&t, ", ");
            P(x + 2.5 * s, y + 0.5 * s); sb_puts(&t, ", ");
            P(x + 0.5 * s, y + 0.5 * s); sb_puts(&t, "), (");
            P(x + s, y + s); sb_puts(&t, ", "); P(x + s, y + 2 * s);
            sb_puts(&t, ", "); P(x + 2 * s, y + 2 * s); sb_puts(&t, ", ");
            P(x + 2 * s, y + s); sb_puts(&t, ", "); P(x + s, y + s);
            sb_puts(&t, "))");
            add(g, "wkt", "nested_holes", 0, &t);
        }
        else if (strcmp(k, "disconnected_interior") == 0)
        {
            /* Two holes that together cut the shell in two. */
            sb_puts(&t, "POLYGON ((");
            P(x, y); sb_puts(&t, ", "); P(x + 4 * s, y); sb_puts(&t, ", ");
            P(x + 4 * s, y + 4 * s); sb_puts(&t, ", "); P(x, y + 4 * s);
            sb_puts(&t, ", "); P(x, y); sb_puts(&t, "), (");
            P(x, y); sb_puts(&t, ", "); P(x + 2 * s, y + 3 * s);
            sb_puts(&t, ", "); P(x + 4 * s, y); sb_puts(&t, ", ");
            P(x + 2 * s, y + 1 * s); sb_puts(&t, ", "); P(x, y);
            sb_puts(&t, "))");
            add(g, "wkt", "disconnected_interior", 0, &t);
        }
        else if (strcmp(k, "unclosed") == 0)
        {
            sb_puts(&t, "POLYGON ((");
            P(x, y); sb_puts(&t, ", "); P(x + s, y); sb_puts(&t, ", ");
            P(x + s, y + s); sb_puts(&t, ", "); P(x, y + s);
            sb_puts(&t, "))");
            add(g, "wkt", "unclosed_ring", 0, &t);
        }
        else if (strcmp(k, "degenerate") == 0)
        {
            /* A polygon of zero area: all vertices on one line. */
            sb_puts(&t, "POLYGON ((");
            P(x, y); sb_puts(&t, ", "); P(x + s, y); sb_puts(&t, ", ");
            P(x + 2 * s, y); sb_puts(&t, ", "); P(x, y); sb_puts(&t, "))");
            add(g, "wkt", "degenerate", 0, &t);
        }
        else if (strcmp(k, "non_finite") == 0)
        {
            /* WKT has no portable NaN: little-endian WKB point. */
            unsigned char wkb[21] = { 1, 1, 0, 0, 0 };
            double nanv = NAN;
            int j;

            memcpy(wkb + 5, &nanv, 8);
            memcpy(wkb + 13, &y, 8);
            for (j = 0; j < 21; j++)
                sb_printf(&t, "%02X", wkb[j]);
            add(g, "wkb", "non_finite", 0, &t);
        }
        else if (strcmp(k, "duplicate_vertex") == 0)
        {
            sb_puts(&t, "LINESTRING (");
            P(x, y); sb_puts(&t, ", "); P(x + s, y); sb_puts(&t, ", ");
            P(x + s, y); sb_puts(&t, ", "); P(x + s, y + s);
            sb_puts(&t, ")");
            add(g, "wkt", "duplicate_vertex", 0, &t);
        }
        else if (strcmp(k, "empty") == 0)
        {
            sb_puts(&t, "POLYGON EMPTY");
            add(g, "wkt", "empty", 0, &t);
        }
        else
        {
            sb_free(&t);
            return seterr(err, TA_GEO_EINVAL, "unknown invalid kind '%s'",
                          k);
        }
#undef P
    }
    return TA_GEO_OK;
}

static int
gen_trajectory(gen *g, kv *o, geo_rng *r, const double b[4],
               long long count, char **err)
{
    const char *crs = kv_str(o, "crs", "EPSG:4326");
    const char *start = kv_get(o, "start");
    double lon = (b[0] + b[2]) / 2, lat = (b[1] + b[3]) / 2;
    double speed, interval, heading, turn, t0, noise, jump;
    long long drop_every, jump_every;
    geo_rng nr;
    PJ *p;
    geo_geod_ctx *c;
    long long i;
    int rc;

    if ((rc = kv_dbl(o, "speed", 10, &speed, err)) != 0 ||
        (rc = kv_dbl(o, "interval", 1, &interval, err)) != 0 ||
        (rc = kv_dbl(o, "heading", 45, &heading, err)) != 0 ||
        (rc = kv_dbl(o, "turn", 0, &turn, err)) != 0 ||
        (rc = kv_dbl(o, "t0", 0, &t0, err)) != 0 ||
        (rc = kv_dbl(o, "noise", 0, &noise, err)) != 0 ||
        (rc = kv_dbl(o, "jump", 0, &jump, err)) != 0 ||
        (rc = kv_long(o, "drop_every", 0, &drop_every, err)) != 0 ||
        (rc = kv_long(o, "jump_every", 0, &jump_every, err)) != 0)
        return rc;
    if (start != NULL && sscanf(start, "%lf %lf", &lon, &lat) != 2)
        return seterr(err, TA_GEO_EINVAL, "start is 'lon lat'");
    if (!(interval > 0) || speed < 0)
        return seterr(err, TA_GEO_EINVAL, "interval > 0, speed >= 0");
    p = geo_crs_create(crs, err);
    if (p == NULL)
        return TA_GEO_ENOENT;
    if (!geo_crs_is_geographic(p))
    {
        proj_destroy(p);
        return seterr(err, TA_GEO_EINVAL, "a trajectory is generated in a "
                      "geographic CRS (lon/lat)");
    }
    c = geo_geod_new(p, err);
    proj_destroy(p);
    if (c == NULL)
        return TA_GEO_EFAIL;
    /*
     * The true path is a geodesic walk: every interval the vehicle moves
     * speed * interval metres along its heading, which drifts by a
     * normal turn. The observation adds independent east and north
     * errors of sd "noise" metres; every jump_every-th sample is thrown
     * "jump" metres north; every drop_every-th sample is not recorded.
     * "expect" says which: "true", "noisy", "jump".
     */
    /* The observation errors come from a stream of their own, so the
     * same seed gives the same true path with or without noise. */
    geo_rng_seed(&nr, (uint64_t)r->s[0] ^ 0x6a09e667f3bcc909ULL);
    for (i = 0; i < count; i++)
    {
        double ox = lon, oy = lat;
        const char *what = noise > 0 ? "noisy" : "true";
        double t = t0 + (double)i * interval;
        bool drop = drop_every > 0 && i > 0 && i % drop_every == 0;

        if (noise > 0)
        {
            double e = geo_rng_normal(&nr) * noise;
            double nn = geo_rng_normal(&nr) * noise;

            geo_geod_direct(c, ox, oy, 90, e, &ox, &oy);
            geo_geod_direct(c, ox, oy, 0, nn, &ox, &oy);
        }
        if (jump_every > 0 && i > 0 && i % jump_every == 0)
        {
            geo_geod_direct(c, ox, oy, 0, jump, &ox, &oy);
            what = "jump";
        }
        if (!drop)
            point(g, ox, oy, what, t);
        geo_geod_direct(c, lon, lat, heading, speed * interval, &lon, &lat);
        heading += turn > 0 ? geo_rng_normal(r) * turn : 0;
    }
    geo_geod_free(c);
    return TA_GEO_OK;
}

int
geo_generate(const char *kind, kv *o, geo_gen_item **items, size_t *n,
             char **err)
{
    gen g = { NULL, 0, 0, 1, false };
    geo_rng r;
    long long seed, count, start_id;
    double b[4];
    bool allow_invalid = false;
    long long i;
    int rc;

    *items = NULL;
    *n = 0;
    if ((rc = kv_long(o, "seed", 1, &seed, err)) != 0 ||
        (rc = kv_long(o, "count", 10, &count, err)) != 0 ||
        (rc = kv_long(o, "start_id", 1, &start_id, err)) != 0 ||
        (rc = kv_bool(o, "allow_invalid", false, &allow_invalid, err)) != 0 ||
        (rc = read_bounds(o, b, err)) != 0)
        return rc;
    if (count < 0 || count > geo_lim.max_features)
        return seterr(err, TA_GEO_E2BIG, "count %lld outside 0..max_features",
                      count);
    g.next_id = start_id;
    geo_rng_seed(&r, (uint64_t)seed);

    if (strcmp(kind, "points") == 0)
    {
        for (i = 0; i < count; i++)
        {
            double x = geo_rng_range(&r, b[0], b[2]);
            double y = geo_rng_range(&r, b[1], b[3]);

            point(&g, x, y, "valid", 0);
        }
    }
    else if (strcmp(kind, "grid") == 0)
    {
        long long nx, ny, ix, iy;
        bool cells;

        if ((rc = kv_long(o, "nx", 10, &nx, err)) != 0 ||
            (rc = kv_long(o, "ny", 10, &ny, err)) != 0 ||
            (rc = kv_bool(o, "cells", false, &cells, err)) != 0)
            return rc;
        if (nx < 1 || ny < 1 || nx * ny > geo_lim.max_features)
            return seterr(err, TA_GEO_EINVAL, "1 <= nx * ny <= max_features");
        for (iy = 0; iy < ny; iy++)
        {
            for (ix = 0; ix < nx; ix++)
            {
                double dx = (b[2] - b[0]) / (double)nx;
                double dy = (b[3] - b[1]) / (double)ny;
                double x0 = b[0] + dx * (double)ix;
                double y0 = b[1] + dy * (double)iy;

                if (cells)
                {
                    sbuf t = SBUF_INIT;

                    sb_puts(&t, "POLYGON ((");
                    xy(&t, x0, y0);
                    sb_puts(&t, ", ");
                    xy(&t, x0 + dx, y0);
                    sb_puts(&t, ", ");
                    xy(&t, x0 + dx, y0 + dy);
                    sb_puts(&t, ", ");
                    xy(&t, x0, y0 + dy);
                    sb_puts(&t, ", ");
                    xy(&t, x0, y0);
                    sb_puts(&t, "))");
                    add(&g, "wkt", "valid", 0, &t);
                }
                else
                    point(&g, x0 + dx / 2, y0 + dy / 2, "valid", 0);
            }
        }
    }
    else if (strcmp(kind, "polygons") == 0)
    {
        long long verts;
        double radius;
        bool holes;

        if ((rc = kv_long(o, "vertices", 8, &verts, err)) != 0 ||
            (rc = kv_dbl(o, "radius", 0, &radius, err)) != 0 ||
            (rc = kv_bool(o, "holes", false, &holes, err)) != 0)
            return rc;
        if (verts < 3 || verts > geo_lim.max_vertices)
            return seterr(err, TA_GEO_EINVAL, "3 <= vertices <= "
                          "max_vertices");
        if (radius <= 0)
            radius = fmin(b[2] - b[0], b[3] - b[1]) / 20;
        for (i = 0; i < count; i++)
        {
            /* The whole star stays inside the bounds. */
            double cx = geo_rng_range(&r, b[0] + radius, b[2] - radius);
            double cy = geo_rng_range(&r, b[1] + radius, b[3] - radius);
            sbuf t = SBUF_INIT;

            /*
             * The shell's edges come no closer to the centre than
             * rmin * cos(0.75 * 2 pi / k) (neighbouring vertices are at
             * most 1.5 sectors apart): a hole inside 90% of that circle
             * lies inside the shell. A triangle has no such circle and
             * gets no hole.
             */
            double inner = 0.5 * radius * cos(0.75 * 2 * M_PI / (double)verts)
                           * 0.9;

            sb_puts(&t, "POLYGON (");
            star_ring(&t, &r, cx, cy, radius * 0.5, radius, (int)verts, true);
            if (holes && inner > 0.05 * radius)
            {
                sb_puts(&t, ", ");
                star_ring(&t, &r, cx, cy, inner * 0.3, inner,
                          (int)(verts < 6 ? 3 : verts / 2 + 1), false);
            }
            sb_puts(&t, ")");
            add(&g, "wkt", "valid", 0, &t);
        }
    }
    else if (strcmp(kind, "lines") == 0)
    {
        long long verts;
        double step;

        if ((rc = kv_long(o, "vertices", 5, &verts, err)) != 0 ||
            (rc = kv_dbl(o, "step", 0, &step, err)) != 0)
            return rc;
        if (verts < 2 || verts > geo_lim.max_vertices)
            return seterr(err, TA_GEO_EINVAL, "2 <= vertices");
        if (step <= 0)
            step = fmin(b[2] - b[0], b[3] - b[1]) / 50;
        for (i = 0; i < count; i++)
        {
            double x = geo_rng_range(&r, b[0], b[2]);
            double y = geo_rng_range(&r, b[1], b[3]);
            sbuf t = SBUF_INIT;
            long long k;

            sb_puts(&t, "LINESTRING (");
            for (k = 0; k < verts; k++)
            {
                double a = geo_rng_range(&r, 0, 2 * M_PI);

                if (k > 0)
                {
                    /* Reflect at the bounds: the walk stays inside. */
                    x += step * cos(a);
                    y += step * sin(a);
                    if (x < b[0]) x = 2 * b[0] - x;
                    if (x > b[2]) x = 2 * b[2] - x;
                    if (y < b[1]) y = 2 * b[1] - y;
                    if (y > b[3]) y = 2 * b[3] - y;
                    sb_puts(&t, ", ");
                }
                xy(&t, x, y);
            }
            sb_puts(&t, ")");
            add(&g, "wkt", "valid", 0, &t);
        }
    }
    else if (strcmp(kind, "boundary") == 0)
    {
        /* Corners, edge midpoints, the centre; for geographic bounds
         * also the poles and the antimeridian. */
        double mx = (b[0] + b[2]) / 2, my = (b[1] + b[3]) / 2;

        point(&g, b[0], b[1], "corner", 0);
        point(&g, b[2], b[1], "corner", 0);
        point(&g, b[2], b[3], "corner", 0);
        point(&g, b[0], b[3], "corner", 0);
        point(&g, mx, b[1], "edge", 0);
        point(&g, b[2], my, "edge", 0);
        point(&g, mx, b[3], "edge", 0);
        point(&g, b[0], my, "edge", 0);
        point(&g, mx, my, "inside", 0);
        point(&g, nextafter(b[0], -INFINITY), my, "outside", 0);
        point(&g, nextafter(b[2], INFINITY), my, "outside", 0);
    }
    else if (strcmp(kind, "outliers") == 0)
    {
        long long every;
        double dist;

        if ((rc = kv_long(o, "every", 10, &every, err)) != 0 ||
            (rc = kv_dbl(o, "distance", 0, &dist, err)) != 0)
            return rc;
        if (every < 1)
            return seterr(err, TA_GEO_EINVAL, "every >= 1");
        if (dist <= 0)
            dist = (b[2] - b[0]) * 10;
        for (i = 0; i < count; i++)
        {
            double x = geo_rng_range(&r, b[0], b[2]);
            double y = geo_rng_range(&r, b[1], b[3]);

            if (i % every == every - 1)
                point(&g, x + dist, y + dist, "outlier", 0);
            else
                point(&g, x, y, "valid", 0);
        }
    }
    else if (strcmp(kind, "trajectory") == 0)
    {
        if ((rc = gen_trajectory(&g, o, &r, b, count, err)) != 0)
            goto fail;
    }
    else if (strcmp(kind, "invalid") == 0)
    {
        if (!allow_invalid)
            return seterr(err, TA_GEO_EINVAL, "invalid geometries are "
                          "generated only with allow_invalid=1");
        if ((rc = gen_invalid(&g, o, &r, b, count, err)) != 0)
            goto fail;
    }
    else
    {
        return seterr(err, TA_GEO_EINVAL, "unknown generator '%s' (points, "
                      "grid, polygons, lines, boundary, outliers, "
                      "trajectory, invalid)", kind);
    }
    if (g.oom)
    {
        rc = seterr(err, TA_GEO_ENOMEM, "out of memory");
        goto fail;
    }
    *items = g.v;
    *n = g.n;
    return TA_GEO_OK;

fail:
    geo_gen_items_free(g.v, g.n);
    return rc;
}

void
geo_gen_items_free(geo_gen_item *items, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        free(items[i].text);
    free(items);
}

int
geo_op_gen(geo_req *r)
{
    geo_gen_item *items = NULL;
    size_t n = 0;
    size_t i;
    uint64_t digest = 0xcbf29ce484222325ULL;
    int rc;

    if ((rc = geo_generate(r->kind, r->opt, &items, &n, r->err)) != 0)
        return rc;
    if ((long long)n > geo_lim.max_report)
    {
        geo_gen_items_free(items, n);
        return seterr(r->err, TA_GEO_E2BIG, "%zu items over max_report "
                      "(%lld): write them to a dataset instead", n,
                      geo_lim.max_report);
    }
    for (i = 0; i < n; i++)
    {
        sb_printf(r->rep, "%lld\t%s\t%s\t", items[i].id, items[i].fmt,
                  items[i].expect);
        sb_num(r->rep, items[i].t);
        sb_puts(r->rep, "\t");
        sb_field(r->rep, items[i].text);
        sb_puts(r->rep, "\n");
        digest ^= geo_hash(items[i].text, strlen(items[i].text));
        digest *= 0x100000001b3ULL;
    }
    sb_kv_int(r->res, "count", (long long)n);
    sb_printf(r->res, "digest=%016llx\n", (unsigned long long)digest);
    geo_gen_items_free(items, n);
    return TA_GEO_OK;
}
