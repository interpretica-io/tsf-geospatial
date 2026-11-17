/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial engine: geometries over GEOS
 *
 * Parsing (WKT, WKB, EWKB, GeoJSON) behind a cheap structural pre-scan
 * that enforces the limits before GEOS allocates anything, serialising,
 * derived geometries, validation (which never repairs), explicit
 * repair, DE-9IM predicates, constructive operations and measurements,
 * planar or geodesic, always with their unit.
 *
 * Everything topological here is two-dimensional, as GEOS is: Z and M
 * are carried through parsing, serialising and transformation, and
 * every result that ignored them says so ("ignored=z,m").
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cpl_vsi.h>

#include "ta_geo_int.h"

/* ---- structural pre-scan ----------------------------------------- */

/* WKT / GeoJSON: nesting depth and a coordinate estimate, on the text. */
static int
prescan_text(const char *t, bool json, const geo_limits *l, char **err)
{
    long long depth = 0;
    long long maxd = 0;
    long long commas = 0;
    long long numbers = 0;
    bool in_str = false;
    bool in_num = false;
    const char *p;
    /* WKT: a multipolygon is three levels deep before any nesting of
     * collections; GeoJSON: Feature/FeatureCollection add four. */
    long long allow = l->max_depth + (json ? 6 : 3);

    for (p = t; *p != '\0'; p++)
    {
        char c = *p;

        if (json && in_str)
        {
            if (c == '\\' && p[1] != '\0')
                p++;
            else if (c == '"')
                in_str = false;
            continue;
        }
        if (json && c == '"')
        {
            in_str = true;
            in_num = false;
            continue;
        }
        if (c == '(' || (json && (c == '[' || c == '{')))
        {
            if (++depth > maxd)
                maxd = depth;
            if (depth > allow)
                return seterr(err, TA_GEO_E2BIG, "nesting deeper than "
                              "max_depth (%lld)", l->max_depth);
        }
        else if (c == ')' || (json && (c == ']' || c == '}')))
        {
            if (--depth < 0)
                return seterr(err, TA_GEO_ECORRUPT, "unbalanced brackets "
                              "at offset %td", p - t);
        }
        else if (c == ',')
        {
            commas++;
        }
        if (json)
        {
            bool digit = (c >= '0' && c <= '9') || c == '-' || c == '.';

            if (digit && !in_num)
                numbers++;
            in_num = digit || (in_num && (c == 'e' || c == 'E' || c == '+'));
        }
    }
    if (depth != 0)
        return seterr(err, TA_GEO_ECORRUPT, "unbalanced brackets (input "
                      "truncated?)");
    if ((json ? numbers / 2 : commas + 1) > l->max_vertices)
        return seterr(err, TA_GEO_E2BIG, "about %lld coordinates, over "
                      "max_vertices (%lld)", json ? numbers / 2 : commas + 1,
                      l->max_vertices);
    return TA_GEO_OK;
}

typedef struct wkb_walk {
    const unsigned char *p;
    size_t len;
    size_t off;
    long long points;
    const geo_limits *l;
    int srid;
    char **err;
} wkb_walk;

static int
wkb_u32(wkb_walk *w, bool le, uint32_t *v)
{
    const unsigned char *b;

    if (w->len - w->off < 4)
        return seterr(w->err, TA_GEO_ECORRUPT, "WKB truncated at byte %zu",
                      w->off);
    b = w->p + w->off;
    *v = le ? (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
              (uint32_t)b[3] << 24
            : (uint32_t)b[3] | (uint32_t)b[2] << 8 | (uint32_t)b[1] << 16 |
              (uint32_t)b[0] << 24;
    w->off += 4;
    return TA_GEO_OK;
}

/* Walk one WKB geometry: counts must fit in what is left of the input,
 * nesting is bounded, the total of points is bounded. */
static int
wkb_geom(wkb_walk *w, int depth)
{
    uint32_t type;
    uint32_t base;
    uint32_t n;
    uint32_t i;
    unsigned int dims = 2;
    bool le;
    int rc;

    if (depth > w->l->max_depth)
        return seterr(w->err, TA_GEO_E2BIG, "WKB nesting deeper than "
                      "max_depth (%lld)", w->l->max_depth);
    if (w->off >= w->len)
        return seterr(w->err, TA_GEO_ECORRUPT, "WKB truncated at byte %zu",
                      w->off);
    if (w->p[w->off] > 1)
        return seterr(w->err, TA_GEO_ECORRUPT, "WKB byte order %u at byte "
                      "%zu is neither 0 nor 1", w->p[w->off], w->off);
    le = w->p[w->off++] == 1;
    if ((rc = wkb_u32(w, le, &type)) != 0)
        return rc;
    if (type & 0x80000000U)
        dims++;
    if (type & 0x40000000U)
        dims++;
    if (type & 0x20000000U)
    {
        uint32_t srid;

        if ((rc = wkb_u32(w, le, &srid)) != 0)
            return rc;
        if (depth == 0)
            w->srid = (int)srid;
    }
    base = type & 0x0fffffffU;
    if (base >= 3000)
        dims += 2, base -= 3000;
    else if (base >= 2000)
        dims += 1, base -= 2000;
    else if (base >= 1000)
        dims += 1, base -= 1000;
    if (dims > 4)
        return seterr(w->err, TA_GEO_ECORRUPT, "WKB type 0x%x has too many "
                      "dimensions", type);
    switch (base)
    {
        case 1:
            if (w->len - w->off < dims * 8)
                return seterr(w->err, TA_GEO_ECORRUPT, "WKB point truncated");
            w->off += dims * 8;
            w->points++;
            break;

        case 2:
            if ((rc = wkb_u32(w, le, &n)) != 0)
                return rc;
            if ((uint64_t)n * dims * 8 > w->len - w->off)
                return seterr(w->err, TA_GEO_ECORRUPT, "WKB line claims %u "
                              "points, %zu bytes are left", n,
                              w->len - w->off);
            w->off += (size_t)n * dims * 8;
            w->points += n;
            break;

        case 3:
        {
            uint32_t r;

            if ((rc = wkb_u32(w, le, &n)) != 0)
                return rc;
            if ((uint64_t)n * 4 > w->len - w->off)
                return seterr(w->err, TA_GEO_ECORRUPT, "WKB polygon claims "
                              "%u rings, %zu bytes are left", n,
                              w->len - w->off);
            for (r = 0; r < n; r++)
            {
                uint32_t np;

                if ((rc = wkb_u32(w, le, &np)) != 0)
                    return rc;
                if ((uint64_t)np * dims * 8 > w->len - w->off)
                    return seterr(w->err, TA_GEO_ECORRUPT, "WKB ring claims "
                                  "%u points, %zu bytes are left", np,
                                  w->len - w->off);
                w->off += (size_t)np * dims * 8;
                w->points += np;
            }
            break;
        }

        case 4: case 5: case 6: case 7:
            if ((rc = wkb_u32(w, le, &n)) != 0)
                return rc;
            /* Every member is at least a 5-byte header. */
            if ((uint64_t)n * 5 > w->len - w->off)
                return seterr(w->err, TA_GEO_ECORRUPT, "WKB collection "
                              "claims %u members, %zu bytes are left", n,
                              w->len - w->off);
            for (i = 0; i < n; i++)
            {
                if ((rc = wkb_geom(w, depth + 1)) != 0)
                    return rc;
            }
            break;

        default:
            return seterr(w->err, TA_GEO_ENOTSUP, "WKB geometry type %u is "
                          "not supported", type);
    }
    if (w->points > w->l->max_vertices)
        return seterr(w->err, TA_GEO_E2BIG, "WKB has more than max_vertices "
                      "(%lld) points", w->l->max_vertices);
    return TA_GEO_OK;
}

static int
hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static int
unhex(const char *t, unsigned char **out, size_t *len, char **err)
{
    size_t n;
    size_t i;
    unsigned char *b;

    while (isspace((unsigned char)*t))
        t++;
    if (t[0] == '\\' && t[1] == 'x')
        t += 2;
    n = strlen(t);
    while (n > 0 && isspace((unsigned char)t[n - 1]))
        n--;
    if (n == 0 || n % 2 != 0)
        return seterr(err, TA_GEO_ECORRUPT, "hex WKB of odd or zero length "
                      "(%zu digits)", n);
    b = malloc(n / 2);
    if (b == NULL)
        return seterr(err, TA_GEO_ENOMEM, "out of memory");
    for (i = 0; i < n / 2; i++)
    {
        int hi = hexval((unsigned char)t[2 * i]);
        int lo = hexval((unsigned char)t[2 * i + 1]);

        if (hi < 0 || lo < 0)
        {
            free(b);
            return seterr(err, TA_GEO_ECORRUPT, "not a hex digit at offset "
                          "%zu", 2 * i);
        }
        b[i] = (unsigned char)(hi << 4 | lo);
    }
    *out = b;
    *len = n / 2;
    return TA_GEO_OK;
}

static bool
looks_hex(const char *t)
{
    const char *p;

    while (isspace((unsigned char)*t))
        t++;
    if (t[0] == '\\' && t[1] == 'x')
        t += 2;
    for (p = t; *p != '\0' && !isspace((unsigned char)*p); p++)
    {
        if (hexval((unsigned char)*p) < 0)
            return false;
    }
    return p != t;
}

/* The category of a GEOS parse failure, for the result. */
static const char *
parse_category(const char *msg)
{
    if (strstr(msg, "closed") != NULL)
        return "unclosed_ring";
    if (strstr(msg, "must have") != NULL ||
        strstr(msg, "Invalid number of points") != NULL)
        return "degenerate";
    if (strstr(msg, "dimension") != NULL)
        return "unsupported_dimensionality";
    return "parse_error";
}

/* A GeoJSON Feature or FeatureCollection: its geometry, or all of
 * them as a collection. Read with GDAL's GeoJSON driver from memory. */
static OGRGeometryH
geojson_features(const char *t)
{
    char path[64];
    VSILFILE *f;
    GDALDatasetH ds;
    OGRLayerH lay;
    OGRFeatureH ft;
    OGRGeometryH coll;
    int n = 0;
    const char *const drv[] = { "GeoJSON", NULL };

    snprintf(path, sizeof(path), "/vsimem/ta_geo_%p.geojson", (void *)&t);
    f = VSIFileFromMemBuffer(path, (GByte *)t, (vsi_l_offset)strlen(t),
                             FALSE);
    if (f == NULL)
        return NULL;
    VSIFCloseL(f);
    ds = GDALOpenEx(path, GDAL_OF_VECTOR, drv, NULL, NULL);
    if (ds == NULL)
    {
        VSIUnlink(path);
        return NULL;
    }
    coll = OGR_G_CreateGeometry(wkbGeometryCollection);
    lay = GDALDatasetGetLayer(ds, 0);
    while (lay != NULL && (ft = OGR_L_GetNextFeature(lay)) != NULL)
    {
        OGRGeometryH g = OGR_F_GetGeometryRef(ft);

        if (g != NULL)
        {
            OGR_G_AddGeometry(coll, g);
            n++;
        }
        OGR_F_Destroy(ft);
    }
    GDALClose(ds);
    VSIUnlink(path);
    if (n == 1 && strstr(t, "FeatureCollection") == NULL)
    {
        OGRGeometryH one = OGR_G_Clone(OGR_G_GetGeometryRef(coll, 0));

        OGR_G_DestroyGeometry(coll);
        return one;
    }
    return coll;
}

int
geo_parse_text(const char *kind, const char *text, const geo_limits *l,
               GEOSGeometry **out, int *srid_out, char **err)
{
    GEOSContextHandle_t h = geo_geos();
    GEOSGeometry *g = NULL;
    const char *t = text;
    int rc;

    *srid_out = 0;
    while (isspace((unsigned char)*t))
        t++;
    if (strcmp(kind, "auto") == 0 || *kind == '\0')
    {
        if (*t == '{')
            kind = "geojson";
        else if (looks_hex(t))
            kind = "wkb";
        else
            kind = "wkt";
    }
    if (*t == '\0')
        return seterr(err, TA_GEO_ECORRUPT, "empty input");

    if (strcmp(kind, "wkt") == 0 || strcmp(kind, "ewkt") == 0)
    {
        GEOSWKTReader *rd;

        if (strncasecmp(t, "SRID=", 5) == 0)
        {
            const char *semi = strchr(t, ';');

            if (semi == NULL)
                return seterr(err, TA_GEO_ECORRUPT, "EWKT SRID without ';'");
            *srid_out = atoi(t + 5);
            t = semi + 1;
        }
        if ((rc = prescan_text(t, false, l, err)) != 0)
            return rc;
        rd = GEOSWKTReader_create_r(h);
        GEOSWKTReader_setFixStructure_r(h, rd, 0);
        g = GEOSWKTReader_read_r(h, rd, t);
        GEOSWKTReader_destroy_r(h, rd);
    }
    else if (strcmp(kind, "wkb") == 0 || strcmp(kind, "ewkb") == 0)
    {
        unsigned char *b = NULL;
        size_t len = 0;
        GEOSWKBReader *rd;
        wkb_walk w = { NULL, 0, 0, 0, l, 0, err };

        if ((rc = unhex(t, &b, &len, err)) != 0)
            return rc;
        w.p = b;
        w.len = len;
        rc = wkb_geom(&w, 0);
        if (rc == 0 && w.off != len)
            rc = seterr(err, TA_GEO_ECORRUPT, "%zu bytes after the end of "
                        "the WKB geometry", len - w.off);
        if (rc != 0)
        {
            free(b);
            return rc;
        }
        *srid_out = w.srid;
        rd = GEOSWKBReader_create_r(h);
        g = GEOSWKBReader_read_r(h, rd, b, len);
        GEOSWKBReader_destroy_r(h, rd);
        free(b);
    }
    else if (strcmp(kind, "geojson") == 0)
    {
        /* Through OGR: GEOS's GeoJSON reader and writer are 2D before
         * 3.14, and Z is part of GeoJSON. */
        OGRGeometryH og;

        if ((rc = prescan_text(t, true, l, err)) != 0)
            return rc;
        geo_gdal_init();
        og = OGR_G_CreateGeometryFromJson(t);
        if (og == NULL && strstr(t, "\"Feature") != NULL)
            og = geojson_features(t);
        if (og == NULL)
            return seterr(err, TA_GEO_ECORRUPT, "geojson does not parse: %s "
                          "[parse_error]", geo_gdal_err());
        g = geo_ogr2geos(og, err);
        OGR_G_DestroyGeometry(og);
        if (g == NULL)
            return TA_GEO_ECORRUPT;
    }
    else
    {
        return seterr(err, TA_GEO_EINVAL, "unknown geometry format '%s' "
                      "(wkt, ewkt, wkb, ewkb, geojson, auto)", kind);
    }

    if (g == NULL)
    {
        if (geo_deadline_passed())
            return seterr(err, TA_GEO_ETIMEDOUT, "parse ran past the "
                          "deadline");
        return seterr(err, TA_GEO_ECORRUPT, "%s does not parse: %s [%s]",
                      kind, geo_geos_err(), parse_category(geo_geos_err()));
    }
    if (geo_npoints(g) > l->max_vertices)
    {
        GEOSGeom_destroy_r(h, g);
        return seterr(err, TA_GEO_E2BIG, "more than max_vertices (%lld) "
                      "coordinates", l->max_vertices);
    }
    if (*srid_out == 0)
        *srid_out = GEOSGetSRID_r(h, g);
    *out = g;
    return TA_GEO_OK;
}

/* ---- coordinate walking ------------------------------------------ */

typedef void (*coord_cb)(void *ctx, const double *c, unsigned int dims,
                         bool has_z, bool has_m, unsigned int idx,
                         unsigned int n, int part);

/* Visit every coordinate sequence; part numbers count sequences. */
static void
walk_seq(const GEOSCoordSequence *s, bool z, bool m, int part, coord_cb cb,
         void *ctx)
{
    GEOSContextHandle_t h = geo_geos();
    unsigned int n = 0;
    unsigned int i;
    unsigned int dims = 2 + z + m;

    if (s == NULL || !GEOSCoordSeq_getSize_r(h, s, &n))
        return;
    for (i = 0; i < n; i++)
    {
        double c[4] = { 0, 0, NAN, NAN };

        GEOSCoordSeq_getXY_r(h, s, i, &c[0], &c[1]);
        if (z)
            GEOSCoordSeq_getOrdinate_r(h, s, i, 2, &c[2]);
        if (m)
            GEOSCoordSeq_getOrdinate_r(h, s, i, 3, &c[z ? 3 : 2]);
        cb(ctx, c, dims, z, m, i, n, part);
    }
}

static void
walk_geom(const GEOSGeometry *g, coord_cb cb, void *ctx, int *part)
{
    GEOSContextHandle_t h = geo_geos();
    int t = GEOSGeomTypeId_r(h, g);
    bool z = GEOSHasZ_r(h, g) == 1;
    bool m = false;
    int i;
    int n;

#if GEOS_AT_LEAST(3, 12)
    m = GEOSHasM_r(h, g) == 1;
#endif
    if (GEOSisEmpty_r(h, g) == 1)
        return;
    switch (t)
    {
        case GEOS_POINT:
        case GEOS_LINESTRING:
        case GEOS_LINEARRING:
            walk_seq(GEOSGeom_getCoordSeq_r(h, g), z, m, (*part)++, cb, ctx);
            break;

        case GEOS_POLYGON:
            walk_geom(GEOSGetExteriorRing_r(h, g), cb, ctx, part);
            n = GEOSGetNumInteriorRings_r(h, g);
            for (i = 0; i < n; i++)
                walk_geom(GEOSGetInteriorRingN_r(h, g, i), cb, ctx, part);
            break;

        default:
            n = GEOSGetNumGeometries_r(h, g);
            for (i = 0; i < n; i++)
                walk_geom(GEOSGetGeometryN_r(h, g, i), cb, ctx, part);
            break;
    }
}

typedef struct zm_range {
    double zmin, zmax, mmin, mmax;
    bool z, m;
    long long nonfinite;
    double bad_x, bad_y;
} zm_range;

static void
zm_cb(void *ctx, const double *c, unsigned int dims, bool has_z, bool has_m,
      unsigned int idx, unsigned int n, int part)
{
    zm_range *r = ctx;
    unsigned int k;

    (void)idx;
    (void)n;
    (void)part;
    for (k = 0; k < dims; k++)
    {
        if (!isfinite(c[k]))
        {
            if (r->nonfinite++ == 0)
            {
                r->bad_x = c[0];
                r->bad_y = c[1];
            }
            break;
        }
    }
    if (has_z && isfinite(c[2]))
    {
        if (!r->z || c[2] < r->zmin)
            r->zmin = c[2];
        if (!r->z || c[2] > r->zmax)
            r->zmax = c[2];
        r->z = true;
    }
    if (has_m)
    {
        double mv = c[has_z ? 3 : 2];

        if (isfinite(mv))
        {
            if (!r->m || mv < r->mmin)
                r->mmin = mv;
            if (!r->m || mv > r->mmax)
                r->mmax = mv;
            r->m = true;
        }
    }
}

/* ---- operations --------------------------------------------------- */

static geo_geom *
get_geom(geo_req *r, int h)
{
    return geo_obj_get(h, GEO_O_GEOM, r->err);
}

static void
put_ignored(geo_req *r, const GEOSGeometry *a, const GEOSGeometry *b)
{
    GEOSContextHandle_t h = geo_geos();
    bool z = GEOSHasZ_r(h, a) == 1 || (b != NULL && GEOSHasZ_r(h, b) == 1);
    bool m = false;

#if GEOS_AT_LEAST(3, 12)
    m = GEOSHasM_r(h, a) == 1 || (b != NULL && GEOSHasM_r(h, b) == 1);
#endif
    sb_kv_str(r->res, "ignored", z ? (m ? "z,m" : "z") : (m ? "m" : ""));
}

static void
put_info(sbuf *res, const geo_geom *o)
{
    GEOSContextHandle_t h = geo_geos();
    const GEOSGeometry *g = o->g;
    double b[4];
    zm_range zr = { 0, 0, 0, 0, false, false, 0, 0, 0 };
    int part = 0;

    sb_kv_str(res, "type", geo_type_name(GEOSGeomTypeId_r(h, g)));
    sb_kv_str(res, "dims", geo_dims_name(g));
    sb_kv_str(res, "crs", o->crs);
    sb_kv_int(res, "npoints", geo_npoints(g));
    sb_kv_int(res, "ngeoms", GEOSGetNumGeometries_r(h, g));
    sb_kv_int(res, "empty", GEOSisEmpty_r(h, g) == 1);
    sb_kv_int(res, "dimension", GEOSGeom_getDimensions_r(h, g));
    if (geo_bounds(g, b))
    {
        sb_kv_num(res, "minx", b[0]);
        sb_kv_num(res, "miny", b[1]);
        sb_kv_num(res, "maxx", b[2]);
        sb_kv_num(res, "maxy", b[3]);
    }
    walk_geom(g, zm_cb, &zr, &part);
    if (zr.z)
    {
        sb_kv_num(res, "minz", zr.zmin);
        sb_kv_num(res, "maxz", zr.zmax);
    }
    if (zr.m)
    {
        sb_kv_num(res, "minm", zr.mmin);
        sb_kv_num(res, "maxm", zr.mmax);
    }
    sb_kv_int(res, "nonfinite", zr.nonfinite);
}

static int
op_parse(geo_req *r)
{
    geo_limits l;
    GEOSGeometry *g = NULL;
    int srid = 0;
    char crs[256];
    const char *c;
    int rc;
    geo_geom *o;

    if ((rc = geo_limits_override(r->opt, &l, r->err)) != 0)
        return rc;
    rc = geo_parse_text(r->kind, r->text, &l, &g, &srid, r->err);
    if (rc != 0)
    {
        if (*r->err != NULL)
        {
            const char *br = strrchr(*r->err, '[');

            if (br != NULL)
                sb_printf(r->res, "category=%.*s\n",
                          (int)strcspn(br + 1, "]"), br + 1);
        }
        return rc;
    }
    c = kv_get(r->opt, "crs");
    if (c == NULL)
        c = kv_get(r->opt, "srid");
    if (c != NULL && *c != '\0' && strcmp(c, "0") != 0)
    {
        geo_crs_norm(c, crs, sizeof(crs));
        if (srid != 0 && strncmp(crs, "EPSG:", 5) == 0 &&
            atoi(crs + 5) != srid)
        {
            GEOSGeom_destroy_r(geo_geos(), g);
            return seterr(r->err, TA_GEO_EINVAL, "the input says SRID %d, "
                          "the caller %s", srid, crs);
        }
    }
    else if (srid != 0)
    {
        snprintf(crs, sizeof(crs), "EPSG:%d", srid);
    }
    else
    {
        crs[0] = '\0';
    }
    if ((rc = geo_geom_new(g, crs, r->handle, r->err)) != 0)
        return rc;
    o = geo_obj_get(*r->handle, GEO_O_GEOM, r->err);
    put_info(r->res, o);
    return TA_GEO_OK;
}

static int
op_info(geo_req *r)
{
    geo_geom *o = get_geom(r, r->ha);

    if (o == NULL)
        return TA_GEO_EINVAL;
    put_info(r->res, o);
    return TA_GEO_OK;
}

static void
gj_num(sbuf *b, double v, int prec)
{
    if (prec >= 0)
        sb_printf(b, "%.*g", prec > 0 ? prec : 1, v);
    else
        sb_num(b, v);
}

static void
gj_seq(sbuf *b, const GEOSGeometry *g, int prec)
{
    GEOSContextHandle_t h = geo_geos();
    const GEOSCoordSequence *s = GEOSGeom_getCoordSeq_r(h, g);
    bool z = GEOSHasZ_r(h, g) == 1;
    unsigned int n = 0;
    unsigned int i;
    bool point = GEOSGeomTypeId_r(h, g) == GEOS_POINT;

    GEOSCoordSeq_getSize_r(h, s, &n);
    if (!point)
        sb_puts(b, "[");
    for (i = 0; i < n; i++)
    {
        double x, y, zz = 0;

        GEOSCoordSeq_getXY_r(h, s, i, &x, &y);
        sb_puts(b, i > 0 ? ",[" : "[");
        gj_num(b, x, prec);
        sb_puts(b, ",");
        gj_num(b, y, prec);
        if (z)
        {
            GEOSCoordSeq_getOrdinate_r(h, s, i, 2, &zz);
            sb_puts(b, ",");
            gj_num(b, zz, prec);
        }
        sb_puts(b, "]");
    }
    if (!point)
        sb_puts(b, "]");
}

static void
gj_poly(sbuf *b, const GEOSGeometry *g, int prec)
{
    GEOSContextHandle_t h = geo_geos();
    int n = GEOSGetNumInteriorRings_r(h, g);
    int i;

    sb_puts(b, "[");
    if (GEOSisEmpty_r(h, g) != 1)
    {
        gj_seq(b, GEOSGetExteriorRing_r(h, g), prec);
        for (i = 0; i < n; i++)
        {
            sb_puts(b, ",");
            gj_seq(b, GEOSGetInteriorRingN_r(h, g, i), prec);
        }
    }
    sb_puts(b, "]");
}

static void
geojson_geom(sbuf *b, const GEOSGeometry *g, int prec)
{
    GEOSContextHandle_t h = geo_geos();
    int t = GEOSGeomTypeId_r(h, g);
    int n = GEOSGetNumGeometries_r(h, g);
    bool empty = GEOSisEmpty_r(h, g) == 1;
    int i;

    sb_printf(b, "{\"type\":\"%s\",",
              t == GEOS_LINEARRING ? "LineString" : geo_type_name(t));
    switch (t)
    {
        case GEOS_POINT:
        case GEOS_LINESTRING:
        case GEOS_LINEARRING:
            sb_puts(b, "\"coordinates\":");
            if (empty)
                sb_puts(b, "[]");
            else
                gj_seq(b, g, prec);
            break;
        case GEOS_POLYGON:
            sb_puts(b, "\"coordinates\":");
            gj_poly(b, g, prec);
            break;
        case GEOS_MULTIPOINT:
        case GEOS_MULTILINESTRING:
        case GEOS_MULTIPOLYGON:
            sb_puts(b, "\"coordinates\":[");
            for (i = 0; i < n && !empty; i++)
            {
                const GEOSGeometry *p = GEOSGetGeometryN_r(h, g, i);

                if (i > 0)
                    sb_puts(b, ",");
                if (t == GEOS_MULTIPOLYGON)
                    gj_poly(b, p, prec);
                else
                    gj_seq(b, p, prec);
            }
            sb_puts(b, "]");
            break;
        default:
            sb_puts(b, "\"geometries\":[");
            for (i = 0; i < n && !empty; i++)
            {
                if (i > 0)
                    sb_puts(b, ",");
                geojson_geom(b, GEOSGetGeometryN_r(h, g, i), prec);
            }
            sb_puts(b, "]");
            break;
    }
    sb_puts(b, "}");
}

static int
op_serialize(geo_req *r)
{
    GEOSContextHandle_t h = geo_geos();
    geo_geom *o = get_geom(r, r->ha);
    long long prec;
    long long dims;
    const char *order;
    int rc;

    if (o == NULL)
        return TA_GEO_EINVAL;
    if ((rc = kv_long(r->opt, "precision", -1, &prec, r->err)) != 0 ||
        (rc = kv_long(r->opt, "dims", 4, &dims, r->err)) != 0)
        return rc;
    order = kv_str(r->opt, "byte_order", "ndr");
    if (dims < 2 || dims > 4)
        return seterr(r->err, TA_GEO_EINVAL, "dims must be 2, 3 or 4");

    if (strcmp(r->kind, "wkt") == 0 || *r->kind == '\0')
    {
        GEOSWKTWriter *w = GEOSWKTWriter_create_r(h);
        char *s;

        GEOSWKTWriter_setOutputDimension_r(h, w, (int)dims);
        GEOSWKTWriter_setTrim_r(h, w, 1);
        GEOSWKTWriter_setRoundingPrecision_r(h, w, (int)prec);
        s = GEOSWKTWriter_write_r(h, w, o->g);
        GEOSWKTWriter_destroy_r(h, w);
        if (s == NULL)
            return seterr(r->err, TA_GEO_EFAIL, "WKT writer: %s",
                          geo_geos_err());
        sb_puts(r->rep, s);
        sb_kv_int(r->res, "bytes", (long long)strlen(s));
        GEOSFree_r(h, s);
    }
    else if (strcmp(r->kind, "wkb") == 0 || strcmp(r->kind, "ewkb") == 0)
    {
        GEOSWKBWriter *w = GEOSWKBWriter_create_r(h);
        unsigned char *hex;
        size_t len = 0;
        bool ewkb = strcmp(r->kind, "ewkb") == 0;

        GEOSWKBWriter_setOutputDimension_r(h, w, (int)dims);
        GEOSWKBWriter_setByteOrder_r(h, w, strcmp(order, "xdr") == 0 ?
                                     GEOS_WKB_XDR : GEOS_WKB_NDR);
        GEOSWKBWriter_setFlavor_r(h, w, ewkb ? GEOS_WKB_EXTENDED :
                                  GEOS_WKB_ISO);
        if (ewkb && strncmp(o->crs, "EPSG:", 5) == 0)
        {
            GEOSSetSRID_r(h, o->g, atoi(o->crs + 5));
            GEOSWKBWriter_setIncludeSRID_r(h, w, 1);
        }
        hex = GEOSWKBWriter_writeHEX_r(h, w, o->g, &len);
        GEOSWKBWriter_destroy_r(h, w);
        if (hex == NULL)
            return seterr(r->err, TA_GEO_EFAIL, "WKB writer: %s",
                          geo_geos_err());
        sb_put(r->rep, (const char *)hex, len);
        sb_kv_int(r->res, "bytes", (long long)len / 2);
        GEOSFree_r(h, hex);
    }
    else if (strcmp(r->kind, "geojson") == 0)
    {
        /* Written here: GEOS's writer drops Z before 3.14 and OGR's
         * rounds to 15 digits. Every number reads back exactly; Z is
         * kept, M dropped (GeoJSON has no M). */
        geojson_geom(r->rep, o->g, (int)prec);
        sb_kv_int(r->res, "bytes", (long long)r->rep->len);
    }
    else
    {
        return seterr(r->err, TA_GEO_EINVAL, "unknown output format '%s' "
                      "(wkt, wkb, ewkb, geojson)", r->kind);
    }
    sb_kv_str(r->res, "format", *r->kind != '\0' ? r->kind : "wkt");
    return TA_GEO_OK;
}

static int
op_derive(geo_req *r)
{
    GEOSContextHandle_t h = geo_geos();
    geo_geom *o = get_geom(r, r->ha);
    GEOSGeometry *g = NULL;
    int rc;

    if (o == NULL)
        return TA_GEO_EINVAL;
    if (strcmp(r->kind, "centroid") == 0)
        g = GEOSGetCentroid_r(h, o->g);
    else if (strcmp(r->kind, "envelope") == 0)
        g = GEOSEnvelope_r(h, o->g);
    else if (strcmp(r->kind, "boundary") == 0)
        g = GEOSBoundary_r(h, o->g);
    else if (strcmp(r->kind, "convex_hull") == 0)
        g = GEOSConvexHull_r(h, o->g);
    else if (strcmp(r->kind, "point_on_surface") == 0)
        g = GEOSPointOnSurface_r(h, o->g);
    else if (strcmp(r->kind, "clone") == 0)
        g = GEOSGeom_clone_r(h, o->g);
    else if (strcmp(r->kind, "normalize") == 0)
    {
        g = GEOSGeom_clone_r(h, o->g);
        if (g != NULL && GEOSNormalize_r(h, g) != 0)
        {
            GEOSGeom_destroy_r(h, g);
            g = NULL;
        }
    }
    else if (strcmp(r->kind, "part") == 0)
    {
        long long n;
        const GEOSGeometry *p;

        if ((rc = kv_long(r->opt, "index", 0, &n, r->err)) != 0)
            return rc;
        if (n < 0 || n >= GEOSGetNumGeometries_r(h, o->g))
            return seterr(r->err, TA_GEO_ENOENT, "no part %lld", n);
        p = GEOSGetGeometryN_r(h, o->g, (int)n);
        g = p != NULL ? GEOSGeom_clone_r(h, p) : NULL;
    }
    else
        return seterr(r->err, TA_GEO_EINVAL, "unknown derivation '%s' "
                      "(centroid, envelope, boundary, convex_hull, "
                      "point_on_surface, clone, normalize, part)", r->kind);
    if (g == NULL)
        return seterr(r->err, TA_GEO_EFAIL, "%s: %s", r->kind,
                      geo_geos_err());
    if ((rc = geo_geom_new(g, o->crs, r->handle, r->err)) != 0)
        return rc;
    put_info(r->res, geo_obj_get(*r->handle, GEO_O_GEOM, r->err));
    return TA_GEO_OK;
}

/* ---- validation ---------------------------------------------------- */

typedef struct issue_list {
    sbuf *rep;
    const char *id;
    long long n;
    long long max;
    char first[48];
    char first_reason[256];
    double fx, fy;
} issue_list;

static void
issue(issue_list *il, const char *category, double x, double y,
      const char *fmt, ...) GEO_PRINTF(5, 6);

static void
issue(issue_list *il, const char *category, double x, double y,
      const char *fmt, ...)
{
    va_list ap;
    sbuf d = SBUF_INIT;

    va_start(ap, fmt);
    sb_vprintf(&d, fmt, ap);
    va_end(ap);
    if (il->n == 0)
    {
        snprintf(il->first, sizeof(il->first), "%s", category);
        snprintf(il->first_reason, sizeof(il->first_reason), "%s",
                 d.p != NULL ? d.p : "");
        il->fx = x;
        il->fy = y;
    }
    if (il->n < il->max)
    {
        sb_field(il->rep, il->id);
        sb_printf(il->rep, "\t%s\t", category);
        sb_num(il->rep, x);
        sb_puts(il->rep, "\t");
        sb_num(il->rep, y);
        sb_puts(il->rep, "\t");
        sb_field(il->rep, d.p);
        sb_puts(il->rep, "\n");
    }
    il->n++;
    sb_free(&d);
}

const char *
geo_validity_category(const char *reason)
{
    static const struct { const char *needle; const char *cat; } map[] = {
        { "ring self-intersection", "ring_self_intersection" },
        { "self-intersection", "self_intersection" },
        { "hole lies outside", "hole_outside" },
        { "holes are nested", "nested_holes" },
        { "nested holes", "nested_holes" },
        { "shells are nested", "nested_shells" },
        { "nested shells", "nested_shells" },
        { "disconnected", "disconnected_interior" },
        { "too few points", "degenerate" },
        { "invalid coordinate", "non_finite" },
        { "not closed", "unclosed_ring" },
        { "duplicate rings", "duplicate_rings" },
    };
    size_t i;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    {
        if (strcasestr(reason, map[i].needle) != NULL)
            return map[i].cat;
    }
    return "invalid";
}

typedef struct dup_ctx {
    issue_list *il;
    double tol;
    double px, py;
    int part;
    long long count;
} dup_ctx;

static void
dup_cb(void *ctx, const double *c, unsigned int dims, bool has_z, bool has_m,
       unsigned int idx, unsigned int n, int part)
{
    dup_ctx *d = ctx;

    (void)dims;
    (void)has_z;
    (void)has_m;
    (void)n;
    if (idx > 0 && part == d->part &&
        fabs(c[0] - d->px) <= d->tol && fabs(c[1] - d->py) <= d->tol)
    {
        d->count++;
        issue(d->il, "duplicate_vertex", c[0], c[1],
              "vertex %u repeats vertex %u of part %d", idx, idx - 1, part);
    }
    d->px = c[0];
    d->py = c[1];
    d->part = part;
}

typedef struct bounds_ctx {
    issue_list *il;
    double b[4];
    long long count;
} bounds_ctx;

static void
bounds_cb(void *ctx, const double *c, unsigned int dims, bool has_z,
          bool has_m, unsigned int idx, unsigned int n, int part)
{
    bounds_ctx *bc = ctx;

    (void)dims;
    (void)has_z;
    (void)has_m;
    (void)n;
    if (!(c[0] >= bc->b[0] && c[0] <= bc->b[2] &&
          c[1] >= bc->b[1] && c[1] <= bc->b[3]))
    {
        bc->count++;
        issue(bc->il, "out_of_bounds", c[0], c[1],
              "vertex %u of part %d outside [%g %g, %g %g]", idx, part,
              bc->b[0], bc->b[1], bc->b[2], bc->b[3]);
    }
}

/* Orientation of the rings of every polygon. */
static void
check_orient(const GEOSGeometry *g, bool want_ccw, issue_list *il,
             long long *bad)
{
    GEOSContextHandle_t h = geo_geos();
    int t = GEOSGeomTypeId_r(h, g);
    int i;
    int n;

    if (t == GEOS_POLYGON && GEOSisEmpty_r(h, g) != 1)
    {
        n = GEOSGetNumInteriorRings_r(h, g);
        for (i = -1; i < n; i++)
        {
            const GEOSGeometry *ring = i < 0 ? GEOSGetExteriorRing_r(h, g) :
                                       GEOSGetInteriorRingN_r(h, g, i);
            const GEOSCoordSequence *s = GEOSGeom_getCoordSeq_r(h, ring);
            char ccw = 0;
            /* Holes turn the other way from the shell. */
            bool want = i < 0 ? want_ccw : !want_ccw;
            double x = 0, y = 0;

            if (s == NULL || !GEOSCoordSeq_isCCW_r(h, s, &ccw))
                continue;
            if ((ccw != 0) != want)
            {
                GEOSCoordSeq_getXY_r(h, s, 0, &x, &y);
                (*bad)++;
                issue(il, "orientation", x, y, "%s %d runs %s, expected %s",
                      i < 0 ? "shell" : "hole", i < 0 ? 0 : i + 1,
                      ccw ? "counter-clockwise" : "clockwise",
                      want ? "counter-clockwise" : "clockwise");
            }
        }
    }
    else if (t == GEOS_MULTIPOLYGON || t == GEOS_GEOMETRYCOLLECTION)
    {
        n = GEOSGetNumGeometries_r(h, g);
        for (i = 0; i < n; i++)
            check_orient(GEOSGetGeometryN_r(h, g, i), want_ccw, il, bad);
    }
}

static int
parse_bounds(const char *s, double b[4], char **err)
{
    if (s == NULL ||
        sscanf(s, "%lf %lf %lf %lf", &b[0], &b[1], &b[2], &b[3]) != 4 ||
        b[0] > b[2] || b[1] > b[3])
        return seterr(err, TA_GEO_EINVAL, "bounds must be 'minx miny maxx "
                      "maxy'");
    return TA_GEO_OK;
}

/* Degenerate: a line of zero length, a polygon of zero area. */
static void
check_degenerate(const GEOSGeometry *g, issue_list *il)
{
    GEOSContextHandle_t h = geo_geos();
    int t = GEOSGeomTypeId_r(h, g);
    double v = 0;
    double b[4] = { 0, 0, 0, 0 };
    int i;

    if (GEOSisEmpty_r(h, g) == 1)
        return;
    geo_bounds(g, b);
    switch (t)
    {
        case GEOS_LINESTRING:
        case GEOS_LINEARRING:
            if (GEOSLength_r(h, g, &v) == 1 && v == 0)
                issue(il, "degenerate", b[0], b[1], "line of zero length");
            break;

        case GEOS_POLYGON:
            if (GEOSArea_r(h, g, &v) == 1 && v == 0)
                issue(il, "degenerate", b[0], b[1], "polygon of zero area");
            break;

        case GEOS_POINT:
            break;

        default:
            for (i = 0; i < GEOSGetNumGeometries_r(h, g); i++)
                check_degenerate(GEOSGetGeometryN_r(h, g, i), il);
            break;
    }
}

static int
op_check(geo_req *r)
{
    GEOSContextHandle_t h = geo_geos();
    geo_geom *o = get_geom(r, r->ha);
    const char *k = *r->kind != '\0' ? r->kind : "all";
    bool all = strcmp(k, "all") == 0;
    issue_list il;
    long long valid = 1;
    int rc;
    int part = 0;
    bool known = false;

    if (o == NULL)
        return TA_GEO_EINVAL;
    il.rep = r->rep;
    il.id = kv_str(r->opt, "id", "");
    il.n = 0;
    il.max = geo_lim.max_report;
    il.first[0] = il.first_reason[0] = '\0';
    il.fx = il.fy = NAN;

    if (all || strcmp(k, "finite") == 0)
    {
        zm_range zr = { 0, 0, 0, 0, false, false, 0, 0, 0 };

        known = true;
        walk_geom(o->g, zm_cb, &zr, &part);
        if (zr.nonfinite > 0)
            issue(&il, "non_finite", zr.bad_x, zr.bad_y,
                  "%lld coordinates are NaN or infinite", zr.nonfinite);
        sb_kv_int(r->res, "nonfinite", zr.nonfinite);
    }
    if (all || strcmp(k, "empty") == 0)
    {
        known = true;
        if (GEOSisEmpty_r(h, o->g) == 1)
            issue(&il, "empty", NAN, NAN, "empty geometry");
    }
    if (all || strcmp(k, "validity") == 0)
    {
        char *reason = NULL;
        GEOSGeometry *loc = NULL;
        char v;
        double x = NAN, y = NAN;

        known = true;
        v = GEOSisValidDetail_r(h, o->g, 0, &reason, &loc);
        if (v == 2)
            return seterr(r->err, TA_GEO_EFAIL, "validity: %s",
                          geo_geos_err());
        valid = v == 1;
        if (!valid)
        {
            if (loc != NULL)
            {
                GEOSGeomGetX_r(h, loc, &x);
                GEOSGeomGetY_r(h, loc, &y);
            }
            issue(&il, geo_validity_category(reason != NULL ? reason : ""), x, y,
                  "%s", reason != NULL ? reason : "invalid");
        }
        if (reason != NULL)
            GEOSFree_r(h, reason);
        if (loc != NULL)
            GEOSGeom_destroy_r(h, loc);
        sb_kv_int(r->res, "valid", valid);
        check_degenerate(o->g, &il);
    }
    if (all || strcmp(k, "duplicates") == 0)
    {
        dup_ctx d = { &il, 0, NAN, NAN, -1, 0 };

        known = true;
        if ((rc = kv_dbl(r->opt, "tolerance", 0, &d.tol, r->err)) != 0)
            return rc;
        part = 0;
        walk_geom(o->g, dup_cb, &d, &part);
        sb_kv_int(r->res, "duplicates", d.count);
    }
    if (all || strcmp(k, "orientation") == 0)
    {
        const char *want = kv_str(r->opt, "orientation",
                                  all ? NULL : "ccw");
        long long bad = 0;

        known = true;
        if (want != NULL)
        {
            if (strcmp(want, "ccw") != 0 && strcmp(want, "cw") != 0)
                return seterr(r->err, TA_GEO_EINVAL, "orientation is ccw or "
                              "cw");
            check_orient(o->g, strcmp(want, "ccw") == 0, &il, &bad);
            sb_kv_int(r->res, "misoriented", bad);
        }
    }
    if (all || strcmp(k, "bounds") == 0)
    {
        const char *s = kv_get(r->opt, "bounds");

        known = true;
        if (s != NULL || !all)
        {
            bounds_ctx bc;

            bc.il = &il;
            bc.count = 0;
            if ((rc = parse_bounds(s, bc.b, r->err)) != 0)
                return rc;
            part = 0;
            walk_geom(o->g, bounds_cb, &bc, &part);
            sb_kv_int(r->res, "out_of_bounds", bc.count);
        }
    }
    if (all || strcmp(k, "dims") == 0)
    {
        const char *allow = kv_get(r->opt, "allow_dims");

        known = true;
        if (allow != NULL)
        {
            const char *d = geo_dims_name(o->g);
            char list[64];
            char *tok;
            char *save = NULL;
            bool ok = false;

            snprintf(list, sizeof(list), "%s", allow);
            for (tok = strtok_r(list, ",", &save); tok != NULL;
                 tok = strtok_r(NULL, ",", &save))
                ok = ok || strcmp(tok, d) == 0;
            if (!ok)
                issue(&il, "unsupported_dimensionality", NAN, NAN,
                      "%s coordinates, allowed: %s", d, allow);
        }
    }
    if (all || strcmp(k, "type") == 0)
    {
        const char *want = kv_get(r->opt, "expect_type");

        known = true;
        if (want != NULL)
        {
            const char *t = geo_type_name(GEOSGeomTypeId_r(h, o->g));

            if (strstr(want, t) == NULL)
                issue(&il, "unexpected_type", NAN, NAN, "%s, expected %s", t,
                      want);
        }
    }
    if (!known)
        return seterr(r->err, TA_GEO_EINVAL, "unknown check '%s' (all, "
                      "validity, finite, empty, duplicates, orientation, "
                      "bounds, dims, type)", k);
    sb_kv_int(r->res, "issues", il.n);
    sb_kv_int(r->res, "ok", il.n == 0);
    sb_kv_str(r->res, "category", il.first);
    sb_kv_str(r->res, "reason", il.first_reason);
    sb_kv_num(r->res, "location_x", il.fx);
    sb_kv_num(r->res, "location_y", il.fy);
    sb_kv_str(r->res, "id", il.id);
    return TA_GEO_OK;
}

static int
op_repair(geo_req *r)
{
    GEOSContextHandle_t h = geo_geos();
    geo_geom *o = get_geom(r, r->ha);
    GEOSGeometry *g = NULL;
    const char *k = *r->kind != '\0' ? r->kind : "make_valid";
    int rc;
    char v;

    if (o == NULL)
        return TA_GEO_EINVAL;
    if (strcmp(k, "make_valid") == 0)
    {
        GEOSMakeValidParams *p = GEOSMakeValidParams_create_r(h);
        const char *method = kv_str(r->opt, "method", "linework");
        bool keep = false;

        if ((rc = kv_bool(r->opt, "keep_collapsed", false, &keep,
                          r->err)) != 0)
        {
            GEOSMakeValidParams_destroy_r(h, p);
            return rc;
        }
        GEOSMakeValidParams_setMethod_r(h, p,
            strcmp(method, "structure") == 0 ? GEOS_MAKE_VALID_STRUCTURE :
                                               GEOS_MAKE_VALID_LINEWORK);
        GEOSMakeValidParams_setKeepCollapsed_r(h, p, keep);
        g = GEOSMakeValidWithParams_r(h, o->g, p);
        GEOSMakeValidParams_destroy_r(h, p);
    }
    else if (strcmp(k, "buffer0") == 0)
    {
        g = GEOSBuffer_r(h, o->g, 0, 8);
    }
    else if (strcmp(k, "orient") == 0)
    {
        const char *want = kv_str(r->opt, "orientation", "ccw");

        g = GEOSGeom_clone_r(h, o->g);
        if (g != NULL &&
            GEOSOrientPolygons_r(h, g, strcmp(want, "cw") == 0) != 0)
        {
            GEOSGeom_destroy_r(h, g);
            g = NULL;
        }
    }
    else if (strcmp(k, "remove_duplicates") == 0)
    {
        double tol;

        if ((rc = kv_dbl(r->opt, "tolerance", 0, &tol, r->err)) != 0)
            return rc;
        g = GEOSRemoveRepeatedPoints_r(h, o->g, tol);
    }
    else
        return seterr(r->err, TA_GEO_EINVAL, "unknown repair '%s' "
                      "(make_valid, buffer0, orient, remove_duplicates)", k);
    if (g == NULL)
        return seterr(r->err, TA_GEO_EFAIL, "%s: %s", k, geo_geos_err());
    v = GEOSisValid_r(h, g);
    if ((rc = geo_geom_new(g, o->crs, r->handle, r->err)) != 0)
        return rc;
    put_info(r->res, geo_obj_get(*r->handle, GEO_O_GEOM, r->err));
    sb_kv_int(r->res, "valid", v == 1);
    sb_kv_str(r->res, "method", k);
    return TA_GEO_OK;
}

/* ---- predicates and construction ---------------------------------- */

static int
same_crs(geo_req *r, const geo_geom *a, const geo_geom *b)
{
    bool any = false;
    int rc;

    if ((rc = kv_bool(r->opt, "any_crs", false, &any, r->err)) != 0)
        return rc;
    if (!any && a->crs[0] != '\0' && b->crs[0] != '\0' &&
        strcmp(a->crs, b->crs) != 0)
        return seterr(r->err, TA_GEO_EINVAL, "geometries are in different "
                      "CRS (%s, %s): transform one first", a->crs, b->crs);
    return TA_GEO_OK;
}

static int
op_predicate(geo_req *r)
{
    GEOSContextHandle_t h = geo_geos();
    geo_geom *a = get_geom(r, r->ha);
    geo_geom *b = a != NULL ? get_geom(r, r->hb) : NULL;
    const char *k = r->kind;
    char v = 2;
    int rc;

    if (a == NULL || b == NULL)
        return TA_GEO_EINVAL;
    if ((rc = same_crs(r, a, b)) != 0)
        return rc;
    if (strcmp(k, "intersects") == 0)
        v = GEOSIntersects_r(h, a->g, b->g);
    else if (strcmp(k, "contains") == 0)
        v = GEOSContains_r(h, a->g, b->g);
    else if (strcmp(k, "within") == 0)
        v = GEOSWithin_r(h, a->g, b->g);
    else if (strcmp(k, "touches") == 0)
        v = GEOSTouches_r(h, a->g, b->g);
    else if (strcmp(k, "crosses") == 0)
        v = GEOSCrosses_r(h, a->g, b->g);
    else if (strcmp(k, "overlaps") == 0)
        v = GEOSOverlaps_r(h, a->g, b->g);
    else if (strcmp(k, "disjoint") == 0)
        v = GEOSDisjoint_r(h, a->g, b->g);
    else if (strcmp(k, "equals") == 0)
        v = GEOSEquals_r(h, a->g, b->g);
    else if (strcmp(k, "covers") == 0)
        v = GEOSCovers_r(h, a->g, b->g);
    else if (strcmp(k, "covered_by") == 0)
        v = GEOSCoveredBy_r(h, a->g, b->g);
    else if (strcmp(k, "equals_exact") == 0)
    {
        double tol;

        if ((rc = kv_dbl(r->opt, "tolerance", 0, &tol, r->err)) != 0)
            return rc;
        v = GEOSEqualsExact_r(h, a->g, b->g, tol);
    }
    else if (strcmp(k, "equals_identical") == 0)
        v = GEOSEqualsIdentical_r(h, a->g, b->g);
    else if (strcmp(k, "dwithin") == 0)
    {
        double dist;
        double d;

        if ((rc = kv_dbl(r->opt, "distance", NAN, &dist, r->err)) != 0)
            return rc;
        if (isnan(dist))
            return seterr(r->err, TA_GEO_EINVAL, "dwithin needs distance");
        if (GEOSDistance_r(h, a->g, b->g, &d) == 1)
            v = d <= dist;
    }
    else if (strcmp(k, "relate") == 0)
    {
        const char *pat = kv_get(r->opt, "pattern");

        if (pat != NULL)
            v = GEOSRelatePattern_r(h, a->g, b->g, pat);
        else
            v = 1;
    }
    else
        return seterr(r->err, TA_GEO_EINVAL, "unknown predicate '%s'", k);
    if (v == 2)
    {
        if (geo_deadline_passed())
            return seterr(r->err, TA_GEO_ETIMEDOUT, "%s ran past the "
                          "deadline", k);
        return seterr(r->err, TA_GEO_EFAIL, "%s: %s", k, geo_geos_err());
    }
    sb_kv_int(r->res, "value", v);
    {
        char *m = GEOSRelate_r(h, a->g, b->g);

        if (m != NULL)
        {
            sb_kv_str(r->res, "de9im", m);
            GEOSFree_r(h, m);
        }
    }
    put_ignored(r, a->g, b->g);
    return TA_GEO_OK;
}

static int
op_construct(geo_req *r)
{
    GEOSContextHandle_t h = geo_geos();
    geo_geom *a = get_geom(r, r->ha);
    geo_geom *b = NULL;
    const char *k = r->kind;
    GEOSGeometry *g = NULL;
    double grid;
    int rc;
    bool binary = strcmp(k, "intersection") == 0 || strcmp(k, "union") == 0 ||
                  strcmp(k, "difference") == 0 ||
                  strcmp(k, "sym_difference") == 0;

    if (a == NULL)
        return TA_GEO_EINVAL;
    if (binary)
    {
        b = get_geom(r, r->hb);
        if (b == NULL)
            return TA_GEO_EINVAL;
        if ((rc = same_crs(r, a, b)) != 0)
            return rc;
    }
    if ((rc = kv_dbl(r->opt, "grid", -1, &grid, r->err)) != 0)
        return rc;

    if (strcmp(k, "intersection") == 0)
        g = GEOSIntersectionPrec_r(h, a->g, b->g, grid);
    else if (strcmp(k, "union") == 0)
        g = GEOSUnionPrec_r(h, a->g, b->g, grid);
    else if (strcmp(k, "difference") == 0)
        g = GEOSDifferencePrec_r(h, a->g, b->g, grid);
    else if (strcmp(k, "sym_difference") == 0)
        g = GEOSSymDifferencePrec_r(h, a->g, b->g, grid);
    else if (strcmp(k, "unary_union") == 0)
        g = GEOSUnaryUnionPrec_r(h, a->g, grid);
    else if (strcmp(k, "buffer") == 0)
    {
        GEOSBufferParams *p = GEOSBufferParams_create_r(h);
        double dist;
        double mitre;
        long long quad;
        bool single;
        const char *cap = kv_str(r->opt, "cap", "round");
        const char *join = kv_str(r->opt, "join", "round");

        if ((rc = kv_dbl(r->opt, "distance", NAN, &dist, r->err)) != 0 ||
            (rc = kv_dbl(r->opt, "mitre_limit", 5, &mitre, r->err)) != 0 ||
            (rc = kv_long(r->opt, "quad_segs", 8, &quad, r->err)) != 0 ||
            (rc = kv_bool(r->opt, "single_sided", false, &single,
                          r->err)) != 0)
        {
            GEOSBufferParams_destroy_r(h, p);
            return rc;
        }
        if (isnan(dist) || quad < 1 || quad > 1000)
        {
            GEOSBufferParams_destroy_r(h, p);
            return seterr(r->err, TA_GEO_EINVAL, "buffer needs distance and "
                          "1 <= quad_segs <= 1000");
        }
        GEOSBufferParams_setEndCapStyle_r(h, p,
            strcmp(cap, "flat") == 0 ? GEOSBUF_CAP_FLAT :
            strcmp(cap, "square") == 0 ? GEOSBUF_CAP_SQUARE :
                                         GEOSBUF_CAP_ROUND);
        GEOSBufferParams_setJoinStyle_r(h, p,
            strcmp(join, "mitre") == 0 ? GEOSBUF_JOIN_MITRE :
            strcmp(join, "bevel") == 0 ? GEOSBUF_JOIN_BEVEL :
                                         GEOSBUF_JOIN_ROUND);
        GEOSBufferParams_setMitreLimit_r(h, p, mitre);
        GEOSBufferParams_setQuadrantSegments_r(h, p, (int)quad);
        GEOSBufferParams_setSingleSided_r(h, p, single);
        g = GEOSBufferWithParams_r(h, a->g, p, dist);
        GEOSBufferParams_destroy_r(h, p);
    }
    else if (strcmp(k, "simplify") == 0)
    {
        double tol;
        bool topo;

        if ((rc = kv_dbl(r->opt, "tolerance", NAN, &tol, r->err)) != 0 ||
            (rc = kv_bool(r->opt, "preserve_topology", true, &topo,
                          r->err)) != 0)
            return rc;
        if (isnan(tol) || tol < 0)
            return seterr(r->err, TA_GEO_EINVAL, "simplify needs "
                          "tolerance >= 0");
        g = topo ? GEOSTopologyPreserveSimplify_r(h, a->g, tol) :
                   GEOSSimplify_r(h, a->g, tol);
    }
    else if (strcmp(k, "densify") == 0)
    {
        double tol;

        if ((rc = kv_dbl(r->opt, "tolerance", NAN, &tol, r->err)) != 0)
            return rc;
        if (!(tol > 0))
            return seterr(r->err, TA_GEO_EINVAL, "densify needs "
                          "tolerance > 0");
        g = GEOSDensify_r(h, a->g, tol);
    }
    else if (strcmp(k, "snap_to_grid") == 0)
    {
        if (!(grid > 0))
            return seterr(r->err, TA_GEO_EINVAL, "snap_to_grid needs "
                          "grid > 0");
        g = GEOSGeom_setPrecision_r(h, a->g, grid, 0);
    }
    else
        return seterr(r->err, TA_GEO_EINVAL, "unknown operation '%s' "
                      "(intersection, union, difference, sym_difference, "
                      "unary_union, buffer, simplify, densify, "
                      "snap_to_grid)", k);
    if (g == NULL)
    {
        if (geo_deadline_passed())
            return seterr(r->err, TA_GEO_ETIMEDOUT, "%s ran past the "
                          "deadline", k);
        return seterr(r->err, TA_GEO_EFAIL, "%s: %s", k, geo_geos_err());
    }
    if (geo_npoints(g) > geo_lim.max_vertices)
    {
        GEOSGeom_destroy_r(h, g);
        return seterr(r->err, TA_GEO_E2BIG, "%s made more than max_vertices "
                      "(%lld) coordinates", k, geo_lim.max_vertices);
    }
    if ((rc = geo_geom_new(g, a->crs, r->handle, r->err)) != 0)
        return rc;
    put_info(r->res, geo_obj_get(*r->handle, GEO_O_GEOM, r->err));
    put_ignored(r, a->g, b != NULL ? b->g : NULL);
    return TA_GEO_OK;
}

/* ---- measurements ------------------------------------------------- */

static void
area_unit(const char *lin, char *out, size_t len)
{
    if (strcmp(lin, "metre") == 0)
        snprintf(out, len, "m2");
    else
        snprintf(out, len, "%s2", lin);
}

static int
measure_planar(geo_req *r, geo_geom *a, geo_geom *b)
{
    GEOSContextHandle_t h = geo_geos();
    const char *k = r->kind;
    char unit[96] = "crs_unit";
    char u2[128];
    double v = NAN;
    int ok = 0;
    bool geographic = false;
    bool require_linear = false;
    int rc;

    if ((rc = kv_bool(r->opt, "require_linear", false, &require_linear,
                      r->err)) != 0)
        return rc;
    if (a->crs[0] != '\0')
    {
        PJ *crs = geo_crs_create(a->crs, r->err);

        if (crs == NULL)
            return TA_GEO_ENOENT;
        geographic = geo_crs_is_geographic(crs);
        geo_crs_unit(crs, unit, sizeof(unit));
        proj_destroy(crs);
    }
    if (geographic && require_linear)
        return seterr(r->err, TA_GEO_EINVAL, "planar %s of geographic "
                      "coordinates (%s) would be in degrees, not metres: "
                      "use mode=geodesic or a projected CRS", k, a->crs);

    if (strcmp(k, "distance") == 0)
        ok = GEOSDistance_r(h, a->g, b->g, &v);
    else if (strcmp(k, "hausdorff") == 0)
        ok = GEOSHausdorffDistance_r(h, a->g, b->g, &v);
    else if (strcmp(k, "frechet") == 0)
        ok = GEOSFrechetDistance_r(h, a->g, b->g, &v);
    else if (strcmp(k, "length") == 0 || strcmp(k, "perimeter") == 0)
    {
        int t = GEOSGeomTypeId_r(h, a->g);
        bool areal = t == GEOS_POLYGON || t == GEOS_MULTIPOLYGON;

        if (strcmp(k, "length") == 0 && areal)
            v = 0, ok = 1;
        else if (strcmp(k, "perimeter") == 0 && !areal &&
                 t != GEOS_GEOMETRYCOLLECTION)
            v = 0, ok = 1;
        else
            ok = GEOSLength_r(h, a->g, &v);
    }
    else if (strcmp(k, "area") == 0)
    {
        ok = GEOSArea_r(h, a->g, &v);
        area_unit(unit, u2, sizeof(u2));
        memcpy(unit, u2, sizeof(unit) - 1);
        unit[sizeof(unit) - 1] = '\0';
    }
    if (ok != 1)
        return seterr(r->err, TA_GEO_EFAIL, "%s: %s", k, geo_geos_err());
    sb_kv_num(r->res, "value", v);
    sb_kv_str(r->res, "unit", unit);
    sb_kv_str(r->res, "mode", "planar");
    sb_kv_str(r->res, "method", "GEOS, Cartesian");
    sb_kv_int(r->res, "geographic", geographic);
    if (geographic)
        sb_kv_str(r->res, "warning", "planar measure of geographic "
                  "coordinates: the unit is the degree, not the metre");
    return TA_GEO_OK;
}

static bool
is_point(const GEOSGeometry *g)
{
    int t = GEOSGeomTypeId_r(geo_geos(), g);

    return t == GEOS_POINT || t == GEOS_MULTIPOINT;
}

/* Geodesic distance where one side is (multi)point: the minimum over
 * the points of the point-to-geometry distance. */
static int
geod_distance(geo_geod_ctx *c, const GEOSGeometry *pts,
              const GEOSGeometry *other, double *out, char **err)
{
    GEOSContextHandle_t h = geo_geos();
    int n = GEOSGetNumGeometries_r(h, pts);
    int i;
    double best = INFINITY;
    int rc;

    for (i = 0; i < n; i++)
    {
        const GEOSGeometry *p = GEOSGetGeometryN_r(h, pts, i);
        double x, y, d;

        if (GEOSisEmpty_r(h, p) == 1)
            continue;
        GEOSGeomGetX_r(h, p, &x);
        GEOSGeomGetY_r(h, p, &y);
        if ((rc = geo_geod_point_dist(c, x, y, other, &d, err)) != 0)
            return rc;
        if (d < best)
            best = d;
    }
    if (isinf(best))
        return seterr(err, TA_GEO_EINVAL, "distance to an empty geometry");
    *out = best;
    return TA_GEO_OK;
}

static int
measure_geodesic(geo_req *r, geo_geom *a, geo_geom *b)
{
    GEOSContextHandle_t h = geo_geos();
    const char *k = r->kind;
    const char *axis = kv_get(r->opt, "axis");
    GEOSGeometry *la = NULL;
    GEOSGeometry *lb = NULL;
    PJ *geod = NULL;
    PJ *geod_b = NULL;
    geo_geod_ctx *c = NULL;
    double v = NAN;
    double len, area;
    const char *unit = "m";
    const char *method = "geodesic on the ellipsoid (Karney, PROJ "
                         "geodesic.h)";
    int rc;

    if (a->crs[0] == '\0')
        return seterr(r->err, TA_GEO_EINVAL, "geodesic %s needs the "
                      "geometry's CRS", k);
    if (axis == NULL)
        return seterr(r->err, TA_GEO_EINVAL, "geodesic %s needs axis=gis "
                      "(x = longitude) or axis=authority (the CRS's own "
                      "axis order)", k);
    rc = geo_lonlat(a->g, a->crs, axis, &la, &geod, r->err);
    if (rc == 0 && b != NULL)
    {
        rc = geo_lonlat(b->g, b->crs[0] != '\0' ? b->crs : a->crs, axis,
                        &lb, &geod_b, r->err);
        if (rc == 0 &&
            !proj_is_equivalent_to(geod, geod_b, PJ_COMP_EQUIVALENT))
            rc = seterr(r->err, TA_GEO_EINVAL, "the two geometries are on "
                        "different datums");
    }
    if (rc == 0)
    {
        c = geo_geod_new(geod, r->err);
        if (c == NULL)
            rc = TA_GEO_EFAIL;
    }
    if (rc != 0)
        goto out;

    if (strcmp(k, "distance") == 0)
    {
        if (is_point(la))
            rc = geod_distance(c, la, lb, &v, r->err);
        else if (is_point(lb))
            rc = geod_distance(c, lb, la, &v, r->err);
        else
            rc = seterr(r->err, TA_GEO_ENOTSUP, "geodesic distance needs a "
                        "point on one side");
        if (rc == 0 && !(is_point(la) && is_point(lb)))
            method = "geodesic to vertices, azimuthal equidistant to "
                     "segment interiors";
    }
    else if (strcmp(k, "length") == 0 || strcmp(k, "perimeter") == 0 ||
             strcmp(k, "area") == 0)
    {
        int t = GEOSGeomTypeId_r(h, la);
        bool areal = t == GEOS_POLYGON || t == GEOS_MULTIPOLYGON;

        rc = geo_geod_measure(c, la, &len, &area, r->err);
        if (rc == 0)
        {
            if (strcmp(k, "area") == 0)
                v = area, unit = "m2";
            else if (strcmp(k, "length") == 0)
                v = areal ? 0 : len;
            else
                v = areal || t == GEOS_GEOMETRYCOLLECTION ? len : 0;
        }
    }
    else
    {
        rc = seterr(r->err, TA_GEO_ENOTSUP, "no geodesic %s", k);
    }
    if (rc != 0)
        goto out;
    sb_kv_num(r->res, "value", v);
    sb_kv_str(r->res, "unit", unit);
    sb_kv_str(r->res, "mode", "geodesic");
    sb_kv_str(r->res, "method", method);
    sb_kv_str(r->res, "ellipsoid_of", proj_get_name(geod));
    sb_kv_str(r->res, "axis", axis);

out:
    geo_geod_free(c);
    proj_destroy(geod);
    proj_destroy(geod_b);
    if (la != NULL)
        GEOSGeom_destroy_r(h, la);
    if (lb != NULL)
        GEOSGeom_destroy_r(h, lb);
    return rc;
}

static int
op_measure(geo_req *r)
{
    geo_geom *a = get_geom(r, r->ha);
    geo_geom *b = NULL;
    const char *mode = kv_get(r->opt, "mode");
    const char *k = r->kind;
    bool binary = strcmp(k, "distance") == 0 || strcmp(k, "hausdorff") == 0 ||
                  strcmp(k, "frechet") == 0;
    int rc;

    if (a == NULL)
        return TA_GEO_EINVAL;
    if (!binary && strcmp(k, "length") != 0 && strcmp(k, "area") != 0 &&
        strcmp(k, "perimeter") != 0)
        return seterr(r->err, TA_GEO_EINVAL, "unknown measurement '%s' "
                      "(distance, hausdorff, frechet, length, area, "
                      "perimeter)", k);
    if (mode == NULL)
        return seterr(r->err, TA_GEO_EINVAL, "say mode=planar or "
                      "mode=geodesic: a measurement is one or the other");
    if (binary)
    {
        b = get_geom(r, r->hb);
        if (b == NULL)
            return TA_GEO_EINVAL;
        if ((rc = same_crs(r, a, b)) != 0)
            return rc;
    }
    if (strcmp(mode, "planar") == 0)
        rc = measure_planar(r, a, b);
    else if (strcmp(mode, "geodesic") == 0)
        rc = measure_geodesic(r, a, b);
    else
        return seterr(r->err, TA_GEO_EINVAL, "mode is planar or geodesic");
    if (rc == 0)
        put_ignored(r, a->g, b != NULL ? b->g : NULL);
    return rc;
}

int
geo_op_geom(ta_geo_op op, geo_req *r)
{
    switch (op)
    {
        case TA_GEO_OP_GEOM_PARSE: return op_parse(r);
        case TA_GEO_OP_GEOM_INFO: return op_info(r);
        case TA_GEO_OP_GEOM_SERIALIZE: return op_serialize(r);
        case TA_GEO_OP_GEOM_DERIVE: return op_derive(r);
        case TA_GEO_OP_GEOM_CHECK: return op_check(r);
        case TA_GEO_OP_GEOM_REPAIR: return op_repair(r);
        case TA_GEO_OP_GEOM_PREDICATE: return op_predicate(r);
        case TA_GEO_OP_GEOM_CONSTRUCT: return op_construct(r);
        case TA_GEO_OP_GEOM_MEASURE: return op_measure(r);
        default:
            return seterr(r->err, TA_GEO_EINVAL, "not a geometry operation");
    }
}
