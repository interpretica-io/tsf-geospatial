/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial engine: geodesics on the ellipsoid
 *
 * PROJ's geodesic.h (C. F. F. Karney's algorithms): distances, lengths,
 * perimeters and areas in metres on the ellipsoid of the geometry's own
 * datum, never on a sphere unless the datum is one. Input is always
 * (longitude, latitude) in degrees: geo_lonlat() makes it so.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <geodesic.h>

#include "ta_geo_int.h"

struct geo_geod_ctx {
    struct geod_geodesic g;
    double a;
    double inv_f;
};

geo_geod_ctx *
geo_geod_new(PJ *geodetic, char **err)
{
    PJ *ell = proj_get_ellipsoid(geo_proj(), geodetic);
    double a = 0, b = 0, inv_f = 0;
    int computed = 0;
    geo_geod_ctx *c;

    if (ell == NULL)
    {
        seterr(err, TA_GEO_EFAIL, "no ellipsoid in %s",
               proj_get_name(geodetic));
        return NULL;
    }
    if (!proj_ellipsoid_get_parameters(geo_proj(), ell, &a, &b, &computed,
                                       &inv_f))
    {
        proj_destroy(ell);
        seterr(err, TA_GEO_EFAIL, "ellipsoid parameters unavailable");
        return NULL;
    }
    proj_destroy(ell);
    c = calloc(1, sizeof(*c));
    if (c == NULL)
    {
        seterr(err, TA_GEO_ENOMEM, "out of memory");
        return NULL;
    }
    c->a = a;
    c->inv_f = inv_f;
    geod_init(&c->g, a, inv_f != 0 ? 1.0 / inv_f : 0);
    return c;
}

void
geo_geod_free(geo_geod_ctx *c)
{
    free(c);
}

double
geo_geod_dist(geo_geod_ctx *c, double lon1, double lat1, double lon2,
              double lat2)
{
    double s12 = 0;

    geod_inverse(&c->g, lat1, lon1, lat2, lon2, &s12, NULL, NULL);
    return s12;
}

void
geo_geod_direct(geo_geod_ctx *c, double lon, double lat, double azi,
                double dist, double *lon2, double *lat2)
{
    geod_direct(&c->g, lat, lon, azi, dist, lat2, lon2, NULL);
}

/* ---- point to geometry ------------------------------------------- */

typedef struct vmin {
    geo_geod_ctx *c;
    double lon, lat;
    double best;
} vmin;

static void
vertex_min(vmin *v, const GEOSGeometry *g)
{
    GEOSContextHandle_t h = geo_geos();
    int t = GEOSGeomTypeId_r(h, g);
    int i;
    int n;

    if (GEOSisEmpty_r(h, g) == 1)
        return;
    switch (t)
    {
        case GEOS_POINT:
        case GEOS_LINESTRING:
        case GEOS_LINEARRING:
        {
            const GEOSCoordSequence *s = GEOSGeom_getCoordSeq_r(h, g);
            unsigned int sz = 0;
            unsigned int k;

            GEOSCoordSeq_getSize_r(h, s, &sz);
            for (k = 0; k < sz; k++)
            {
                double x, y, d;

                GEOSCoordSeq_getXY_r(h, s, k, &x, &y);
                d = geo_geod_dist(v->c, v->lon, v->lat, x, y);
                if (d < v->best)
                    v->best = d;
            }
            break;
        }

        case GEOS_POLYGON:
            vertex_min(v, GEOSGetExteriorRing_r(h, g));
            n = GEOSGetNumInteriorRings_r(h, g);
            for (i = 0; i < n; i++)
                vertex_min(v, GEOSGetInteriorRingN_r(h, g, i));
            break;

        default:
            n = GEOSGetNumGeometries_r(h, g);
            for (i = 0; i < n; i++)
                vertex_min(v, GEOSGetGeometryN_r(h, g, i));
            break;
    }
}

int
geo_geod_point_dist(geo_geod_ctx *c, double lon, double lat,
                    const GEOSGeometry *g, double *out, char **err)
{
    GEOSContextHandle_t h = geo_geos();
    vmin v = { c, lon, lat, INFINITY };
    char def[512];
    PJ *aeqd;
    GEOSGeometry *pg = NULL;
    GEOSGeometry *origin;
    long long bad = 0;
    double d;
    int t = GEOSGeomTypeId_r(h, g);
    int rc;

    vertex_min(&v, g);
    if (isinf(v.best))
        return seterr(err, TA_GEO_EINVAL, "distance to an empty geometry");
    if (t == GEOS_POINT || t == GEOS_MULTIPOINT)
    {
        *out = v.best;
        return TA_GEO_OK;
    }
    /* Inside an areal geometry the distance is zero, as in GEOS. */
    if (t == GEOS_POLYGON || t == GEOS_MULTIPOLYGON)
    {
        GEOSGeometry *p = GEOSGeom_createPointFromXY_r(h, lon, lat);
        char in = GEOSIntersects_r(h, g, p);

        GEOSGeom_destroy_r(h, p);
        if (in == 1)
        {
            *out = 0;
            return TA_GEO_OK;
        }
    }
    if (c->inv_f != 0)
        snprintf(def, sizeof(def), "+proj=pipeline +step +proj=unitconvert "
                 "+xy_in=deg +xy_out=rad +step +proj=aeqd +lat_0=%.17g "
                 "+lon_0=%.17g +a=%.17g +rf=%.17g", lat, lon, c->a, c->inv_f);
    else
        snprintf(def, sizeof(def), "+proj=pipeline +step +proj=unitconvert "
                 "+xy_in=deg +xy_out=rad +step +proj=aeqd +lat_0=%.17g "
                 "+lon_0=%.17g +R=%.17g", lat, lon, c->a);
    aeqd = proj_create(geo_proj(), def);
    if (aeqd == NULL)
        return seterr(err, TA_GEO_EFAIL, "azimuthal equidistant projection: "
                      "%s", geo_proj_err());
    rc = geo_transform_geom(aeqd, g, &pg, &bad, err);
    proj_destroy(aeqd);
    if (rc != 0)
        return rc;
    origin = GEOSGeom_createPointFromXY_r(h, 0, 0);
    if (GEOSDistance_r(h, origin, pg, &d) != 1)
        d = INFINITY;
    GEOSGeom_destroy_r(h, origin);
    GEOSGeom_destroy_r(h, pg);
    *out = d < v.best ? d : v.best;
    return TA_GEO_OK;
}

/* ---- lengths and areas -------------------------------------------- */

static double
seq_length(geo_geod_ctx *c, const GEOSCoordSequence *s)
{
    GEOSContextHandle_t h = geo_geos();
    unsigned int n = 0;
    unsigned int i;
    double px = 0, py = 0;
    double len = 0;

    GEOSCoordSeq_getSize_r(h, s, &n);
    for (i = 0; i < n; i++)
    {
        double x, y;

        GEOSCoordSeq_getXY_r(h, s, i, &x, &y);
        if (i > 0)
            len += geo_geod_dist(c, px, py, x, y);
        px = x;
        py = y;
    }
    return len;
}

static int
ring_area(geo_geod_ctx *c, const GEOSGeometry *ring, double *area,
          double *perim, char **err)
{
    GEOSContextHandle_t h = geo_geos();
    const GEOSCoordSequence *s = GEOSGeom_getCoordSeq_r(h, ring);
    unsigned int n = 0;
    unsigned int i;
    double *lat;
    double *lon;

    GEOSCoordSeq_getSize_r(h, s, &n);
    if (n < 4)
    {
        *area = *perim = 0;
        return TA_GEO_OK;
    }
    lat = malloc(n * sizeof(*lat));
    lon = malloc(n * sizeof(*lon));
    if (lat == NULL || lon == NULL)
    {
        free(lat);
        free(lon);
        return seterr(err, TA_GEO_ENOMEM, "out of memory");
    }
    for (i = 0; i < n; i++)
        GEOSCoordSeq_getXY_r(h, s, i, &lon[i], &lat[i]);
    /* The closing vertex repeats the first: geod_polygonarea closes. */
    geod_polygonarea(&c->g, lat, lon, (int)n - 1, area, perim);
    *area = fabs(*area);
    free(lat);
    free(lon);
    return TA_GEO_OK;
}

int
geo_geod_measure(geo_geod_ctx *c, const GEOSGeometry *g, double *length,
                 double *area, char **err)
{
    GEOSContextHandle_t h = geo_geos();
    int t = GEOSGeomTypeId_r(h, g);
    int i;
    int n;
    int rc;

    *length = 0;
    *area = 0;
    if (GEOSisEmpty_r(h, g) == 1)
        return TA_GEO_OK;
    switch (t)
    {
        case GEOS_POINT:
            return TA_GEO_OK;

        case GEOS_LINESTRING:
        case GEOS_LINEARRING:
            *length = seq_length(c, GEOSGeom_getCoordSeq_r(h, g));
            return TA_GEO_OK;

        case GEOS_POLYGON:
        {
            double a, p;

            if ((rc = ring_area(c, GEOSGetExteriorRing_r(h, g), &a, &p,
                                err)) != 0)
                return rc;
            *area = a;
            *length = p;
            n = GEOSGetNumInteriorRings_r(h, g);
            for (i = 0; i < n; i++)
            {
                if ((rc = ring_area(c, GEOSGetInteriorRingN_r(h, g, i), &a,
                                    &p, err)) != 0)
                    return rc;
                *area -= a;
                *length += p;
            }
            return TA_GEO_OK;
        }

        default:
            n = GEOSGetNumGeometries_r(h, g);
            for (i = 0; i < n; i++)
            {
                double l2, a2;

                if ((rc = geo_geod_measure(c, GEOSGetGeometryN_r(h, g, i),
                                           &l2, &a2, err)) != 0)
                    return rc;
                *length += l2;
                *area += a2;
            }
            return TA_GEO_OK;
    }
}
