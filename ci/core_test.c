/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Contract tests of the tsf-geospatial core, without TE
 *
 * Built with the sanitizers by ci/build-core.sh and run by
 * ci/core-test.sh. Exits non-zero on the first broken contract. The
 * reference values are published ones: the worked examples of IOGP
 * Guidance Note 7-2 (EPSG), GeographicLib's GeodSolve example, and
 * closed forms (areas of squares, the length of a degree on the
 * equator, statistics of a linear gradient).
 *
 * Usage: core_test <scratch directory>
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ta_geo_core.h"

typedef struct res {
    int rc;
    int h;
    unsigned int feat;
    char *result;
    char *report;
    char *err;
} res;

static const char *dir;
static int n_ok;

static void
res_free(res *r)
{
    free(r->result);
    free(r->report);
    free(r->err);
    memset(r, 0, sizeof(*r));
}

static res
call(ta_geo_op op, int ha, int hb, const char *kind, const char *text,
     const char *fmt, ...)
{
    res r;
    char params[4096] = "";
    va_list ap;

    memset(&r, 0, sizeof(r));
    if (fmt != NULL)
    {
        va_start(ap, fmt);
        vsnprintf(params, sizeof(params), fmt, ap);
        va_end(ap);
    }
    r.rc = ta_geo_call(op, ha, hb, kind, text, params, &r.h, &r.feat,
                       &r.result, &r.report, &r.err);
    return r;
}

static const char *
get(const res *r, const char *key)
{
    static char buf[4096];
    size_t kl = strlen(key);
    const char *p = r->result;

    while (p != NULL && *p != '\0')
    {
        const char *eol = strchr(p, '\n');
        size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);

        if (len > kl && strncmp(p, key, kl) == 0 && p[kl] == '=')
        {
            snprintf(buf, sizeof(buf), "%.*s", (int)(len - kl - 1),
                     p + kl + 1);
            return buf;
        }
        p = eol != NULL ? eol + 1 : NULL;
    }
    return NULL;
}

static double
num(const res *r, const char *key)
{
    const char *v = get(r, key);

    return v != NULL ? strtod(v, NULL) : NAN;
}

#define FAIL(...) \
    do {                                                            \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
        fprintf(stderr, __VA_ARGS__);                               \
        fprintf(stderr, "\n");                                      \
        exit(1);                                                    \
    } while (0)

#define OK(_r) \
    do {                                                            \
        if ((_r).rc != 0)                                           \
            FAIL("%s: %s\n%s", #_r, ta_geo_strerror((_r).rc),       \
                 (_r).err);                                         \
    } while (0)

#define EXPECT_RC(_r, _rc) \
    do {                                                            \
        if ((_r).rc != (_rc))                                       \
            FAIL("expected %s, got %s: %s", ta_geo_strerror(_rc),   \
                 ta_geo_strerror((_r).rc), (_r).err);               \
    } while (0)

#define EQ_STR(_r, _k, _v) \
    do {                                                            \
        const char *_g = get(&(_r), (_k));                          \
        if (_g == NULL || strcmp(_g, (_v)) != 0)                    \
            FAIL("%s = '%s', expected '%s'\n%s", (_k),              \
                 _g != NULL ? _g : "(none)", (_v), (_r).result);    \
    } while (0)

#define NEAR(_got, _want, _tol) \
    do {                                                            \
        double _a = (_got), _b = (_want);                           \
        if (!(fabs(_a - _b) <= (_tol)))                             \
            FAIL("%s = %.17g, expected %.17g +- %g", #_got, _a, _b, \
                 (double)(_tol));                                   \
    } while (0)

static void
ok(const char *what)
{
    printf("ok: %s\n", what);
    n_ok++;
}

static int
geom(const char *kind, const char *text, const char *crs)
{
    res r = call(TA_GEO_OP_GEOM_PARSE, 0, 0, kind, text, "crs=%s", crs);
    int h;

    OK(r);
    h = r.h;
    res_free(&r);
    return h;
}

static void
release(int h)
{
    res r = call(TA_GEO_OP_RELEASE, h, 0, "", "", NULL);

    OK(r);
    res_free(&r);
}

/* ---- tests -------------------------------------------------------- */

static void
t_available(void)
{
    res r = call(TA_GEO_OP_AVAILABLE, 0, 0, "", "", NULL);

    OK(r);
    if ((r.feat & 7) != 7)
        FAIL("GEOS, PROJ, GDAL bits: 0x%x", r.feat);
    if (strstr(r.report, "GPKG\t") == NULL ||
        strstr(r.report, "GTiff\t") == NULL)
        FAIL("driver list lacks GPKG/GTiff");
    printf("   geos=%s proj=%s gdal=%s m=%s\n", get(&r, "geos") != NULL ?
           "y" : "n", get(&r, "proj") != NULL ? "y" : "n",
           get(&r, "gdal") != NULL ? "y" : "n", get(&r, "m"));
    res_free(&r);
    ok("available");
}

static void
t_parse(void)
{
    res r;
    int h;
    char wkb[512];

    r = call(TA_GEO_OP_GEOM_PARSE, 0, 0, "wkt", "POINT (-8.61 41.15)",
             "crs=4326");
    OK(r);
    EQ_STR(r, "type", "Point");
    EQ_STR(r, "dims", "xy");
    EQ_STR(r, "crs", "EPSG:4326");
    h = r.h;
    res_free(&r);

    r = call(TA_GEO_OP_GEOM_SERIALIZE, h, 0, "wkt", "", NULL);
    OK(r);
    if (strcmp(r.report, "POINT (-8.61 41.15)") != 0)
        FAIL("WKT round trip: %s", r.report);
    res_free(&r);
    r = call(TA_GEO_OP_GEOM_SERIALIZE, h, 0, "ewkb", "", NULL);
    OK(r);
    snprintf(wkb, sizeof(wkb), "%s", r.report);
    res_free(&r);
    release(h);
    r = call(TA_GEO_OP_GEOM_PARSE, 0, 0, "auto", wkb, NULL);
    OK(r);
    EQ_STR(r, "crs", "EPSG:4326");
    EQ_STR(r, "type", "Point");
    NEAR(num(&r, "minx"), -8.61, 0);
    release(r.h);
    res_free(&r);

    /* Z and M survive parse and serialise. */
    r = call(TA_GEO_OP_GEOM_PARSE, 0, 0, "wkt",
             "LINESTRING ZM (0 0 10 100, 1 1 20 200)", NULL);
    OK(r);
    EQ_STR(r, "dims", "xyzm");
    NEAR(num(&r, "maxz"), 20, 0);
    NEAR(num(&r, "maxm"), 200, 0);
    h = r.h;
    res_free(&r);
    r = call(TA_GEO_OP_GEOM_SERIALIZE, h, 0, "wkb", "", NULL);
    OK(r);
    snprintf(wkb, sizeof(wkb), "%s", r.report);
    res_free(&r);
    release(h);
    r = call(TA_GEO_OP_GEOM_PARSE, 0, 0, "wkb", wkb, NULL);
    OK(r);
    EQ_STR(r, "dims", "xyzm");
    NEAR(num(&r, "minm"), 100, 0);
    release(r.h);
    res_free(&r);

    r = call(TA_GEO_OP_GEOM_PARSE, 0, 0, "geojson",
             "{\"type\":\"Polygon\",\"coordinates\":[[[0,0],[2,0],[2,2],"
             "[0,2],[0,0]]]}", NULL);
    OK(r);
    EQ_STR(r, "type", "Polygon");
    release(r.h);
    res_free(&r);

    /* GeoJSON keeps Z both ways; Features and collections are read. */
    r = call(TA_GEO_OP_GEOM_PARSE, 0, 0, "geojson",
             "{\"type\":\"LineString\",\"coordinates\":[[1,2,3],[4,5,6]]}",
             NULL);
    OK(r);
    EQ_STR(r, "dims", "xyz");
    h = r.h;
    res_free(&r);
    r = call(TA_GEO_OP_GEOM_SERIALIZE, h, 0, "geojson", "", NULL);
    OK(r);
    if (strstr(r.report, "6") == NULL)
        FAIL("GeoJSON lost Z: %s", r.report);
    res_free(&r);
    release(h);
    r = call(TA_GEO_OP_GEOM_PARSE, 0, 0, "auto",
             "{\"type\":\"FeatureCollection\",\"features\":["
             "{\"type\":\"Feature\",\"properties\":{},\"geometry\":"
             "{\"type\":\"Point\",\"coordinates\":[1,2]}},"
             "{\"type\":\"Feature\",\"properties\":{},\"geometry\":"
             "{\"type\":\"Point\",\"coordinates\":[3,4]}}]}", NULL);
    OK(r);
    EQ_STR(r, "type", "GeometryCollection");
    EQ_STR(r, "ngeoms", "2");
    release(r.h);
    res_free(&r);
    ok("parse and serialise: WKT, (E)WKB, GeoJSON, ZM");
}

static void
t_bad_input(void)
{
    static const struct { const char *kind; const char *text; int rc;
                          const char *cat; } cases[] = {
        { "wkt", "POLYGON ((0 0, 1 0, 1 1, 0 1))", TA_GEO_ECORRUPT,
          "unclosed_ring" },
        { "wkt", "POINT (1 2", TA_GEO_ECORRUPT, NULL },
        { "wkt", "LINESTRING (1 2)", TA_GEO_ECORRUPT, NULL },
        { "wkt", "TRIANGLE (1 2)", TA_GEO_ECORRUPT, NULL },
        { "wkb", "0101000000", TA_GEO_ECORRUPT, NULL },
        { "wkb", "01020000000000ffff", TA_GEO_ECORRUPT, NULL },
        { "wkb", "010700000080969800", TA_GEO_ECORRUPT, NULL },
        { "wkb", "02010000000000000000000000", TA_GEO_ECORRUPT, NULL },
        { "wkb", "zz", TA_GEO_ECORRUPT, NULL },
        { "geojson", "{\"type\":\"Point\",\"coordinates\":[1,", TA_GEO_ECORRUPT,
          NULL },
        { "geojson", "{\"type\":\"Pointless\",\"coordinates\":[1,2]}",
          TA_GEO_ECORRUPT, NULL },
        { "wkt", "", TA_GEO_ECORRUPT, NULL },
    };
    size_t i;
    char deep[16384];
    size_t p = 0;
    res r;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        r = call(TA_GEO_OP_GEOM_PARSE, 0, 0, cases[i].kind, cases[i].text,
                 NULL);
        if (r.rc != cases[i].rc)
            FAIL("'%s' (%s): %s, expected %s: %s", cases[i].text,
                 cases[i].kind, ta_geo_strerror(r.rc),
                 ta_geo_strerror(cases[i].rc), r.err);
        if (cases[i].cat != NULL)
            EQ_STR(r, "category", cases[i].cat);
        if (r.h != 0)
            FAIL("a failed parse left a handle");
        res_free(&r);
    }
    /* 500 nested collections: refused before GEOS recurses. */
    for (i = 0; i < 500; i++)
        p += (size_t)snprintf(deep + p, sizeof(deep) - p,
                              "GEOMETRYCOLLECTION (");
    p += (size_t)snprintf(deep + p, sizeof(deep) - p, "POINT (1 1)");
    for (i = 0; i < 500; i++)
        deep[p++] = ')';
    deep[p] = '\0';
    r = call(TA_GEO_OP_GEOM_PARSE, 0, 0, "wkt", deep, NULL);
    EXPECT_RC(r, TA_GEO_E2BIG);
    res_free(&r);
    r = call(TA_GEO_OP_GEOM_PARSE, 0, 0, "wkt",
             "LINESTRING (0 0, 1 1, 2 2, 3 3, 4 4)", "max_vertices=3");
    EXPECT_RC(r, TA_GEO_E2BIG);
    res_free(&r);
    r = call(TA_GEO_OP_GEOM_PARSE, 0, 0, "wkt", "POINT (1 1)", "bogus=1");
    EXPECT_RC(r, TA_GEO_EINVAL);
    if (r.h != 0)
        FAIL("an unknown option left a handle");
    res_free(&r);
    ok("malformed input refused with a category, limits enforced");
}

static void
t_predicates(void)
{
    int sq = geom("wkt", "POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))", "EPSG:3857");
    int in = geom("wkt", "POINT (1 1)", "EPSG:3857");
    int edge = geom("wkt", "POINT (2 1)", "EPSG:3857");
    int out = geom("wkt", "POINT (3 3)", "EPSG:3857");
    int sq2 = geom("wkt", "POLYGON ((1 1, 3 1, 3 3, 1 3, 1 1))", "EPSG:3857");
    int other = geom("wkt", "POINT (1 1)", "EPSG:4326");
    static const struct { const char *p; int a; int b; int v; } cases[6 * 3];
    struct { const char *p; int *a; int *b; int v; } c[] = {
        { "contains", &sq, &in, 1 }, { "within", &in, &sq, 1 },
        { "contains", &sq, &edge, 0 }, { "touches", &edge, &sq, 1 },
        { "intersects", &sq, &edge, 1 }, { "disjoint", &sq, &out, 1 },
        { "overlaps", &sq, &sq2, 1 }, { "crosses", &sq, &sq2, 0 },
        { "equals", &sq, &sq, 1 }, { "covers", &sq, &edge, 1 },
    };
    size_t i;
    res r;

    (void)cases;
    for (i = 0; i < sizeof(c) / sizeof(c[0]); i++)
    {
        r = call(TA_GEO_OP_GEOM_PREDICATE, *c[i].a, *c[i].b, c[i].p, "",
                 NULL);
        OK(r);
        if ((int)num(&r, "value") != c[i].v)
            FAIL("%s = %s, expected %d", c[i].p, get(&r, "value"), c[i].v);
        res_free(&r);
    }
    r = call(TA_GEO_OP_GEOM_PREDICATE, sq, in, "relate", "", NULL);
    OK(r);
    EQ_STR(r, "de9im", "0F2FF1FF2");
    res_free(&r);
    /* Different CRS: refused, not silently compared. */
    r = call(TA_GEO_OP_GEOM_PREDICATE, sq, other, "contains", "", NULL);
    EXPECT_RC(r, TA_GEO_EINVAL);
    res_free(&r);

    r = call(TA_GEO_OP_GEOM_CONSTRUCT, sq, sq2, "intersection", "", NULL);
    OK(r);
    {
        int ix = r.h;
        res a;

        res_free(&r);
        a = call(TA_GEO_OP_GEOM_MEASURE, ix, 0, "area", "", "mode=planar");
        OK(a);
        NEAR(num(&a, "value"), 1, 1e-12);
        EQ_STR(a, "unit", "m2");
        res_free(&a);
        release(ix);
    }
    r = call(TA_GEO_OP_GEOM_CONSTRUCT, sq, sq2, "union", "", NULL);
    OK(r);
    {
        int u = r.h;
        res a;

        res_free(&r);
        a = call(TA_GEO_OP_GEOM_MEASURE, u, 0, "area", "", "mode=planar");
        OK(a);
        NEAR(num(&a, "value"), 7, 1e-12);
        res_free(&a);
        release(u);
    }
    r = call(TA_GEO_OP_GEOM_CONSTRUCT, in, 0, "buffer", "",
             "distance=1\nquad_segs=64");
    OK(r);
    {
        int b = r.h;
        res a;

        res_free(&r);
        a = call(TA_GEO_OP_GEOM_MEASURE, b, 0, "area", "", "mode=planar");
        OK(a);
        NEAR(num(&a, "value"), M_PI, 1e-3);
        res_free(&a);
        release(b);
    }
    /* A measurement must name its mode. */
    r = call(TA_GEO_OP_GEOM_MEASURE, sq, 0, "area", "", NULL);
    EXPECT_RC(r, TA_GEO_EINVAL);
    res_free(&r);
    r = call(TA_GEO_OP_GEOM_MEASURE, sq, 0, "perimeter", "", "mode=planar");
    OK(r);
    NEAR(num(&r, "value"), 8, 0);
    res_free(&r);
    release(sq);
    release(in);
    release(edge);
    release(out);
    release(sq2);
    release(other);
    ok("DE-9IM predicates, overlay, buffer, planar measures");
}

static void
t_geodesic(void)
{
    int a = geom("wkt", "POINT (-73.8 40.6)", "EPSG:4326");
    int b = geom("wkt", "POINT (-0.5 51.6)", "EPSG:4326");
    int e0 = geom("wkt", "POINT (0 0)", "EPSG:4326");
    int e1 = geom("wkt", "POINT (1 0)", "EPSG:4326");
    int cell = geom("wkt", "POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))",
                    "EPSG:4326");
    res r;

    /* GeographicLib GeodSolve -i: 40.6 -73.8 51.6 -0.5 ->
     * s12 = 5551759.400319 m on WGS84. */
    r = call(TA_GEO_OP_GEOM_MEASURE, a, b, "distance", "",
             "mode=geodesic\naxis=gis");
    OK(r);
    NEAR(num(&r, "value"), 5551759.400319, 1e-4);
    EQ_STR(r, "unit", "m");
    res_free(&r);
    /* The same points in the authority order of EPSG:4326 (lat, lon)
     * give the same distance only if the axis option is honoured. */
    {
        int la = geom("wkt", "POINT (40.6 -73.8)", "EPSG:4326");
        int lb = geom("wkt", "POINT (51.6 -0.5)", "EPSG:4326");

        r = call(TA_GEO_OP_GEOM_MEASURE, la, lb, "distance", "",
                 "mode=geodesic\naxis=authority");
        OK(r);
        NEAR(num(&r, "value"), 5551759.400319, 1e-4);
        res_free(&r);
        release(la);
        release(lb);
    }
    /* A degree of longitude on the equator: a * pi / 180. */
    r = call(TA_GEO_OP_GEOM_MEASURE, e0, e1, "distance", "",
             "mode=geodesic\naxis=gis");
    OK(r);
    NEAR(num(&r, "value"), 6378137.0 * M_PI / 180.0, 1e-6);
    res_free(&r);
    /* Planar on lon/lat: degrees, flagged, refused when metres are
     * required. */
    r = call(TA_GEO_OP_GEOM_MEASURE, e0, e1, "distance", "", "mode=planar");
    OK(r);
    EQ_STR(r, "unit", "degree");
    EQ_STR(r, "geographic", "1");
    res_free(&r);
    r = call(TA_GEO_OP_GEOM_MEASURE, e0, e1, "distance", "",
             "mode=planar\nrequire_linear=1");
    EXPECT_RC(r, TA_GEO_EINVAL);
    res_free(&r);
    /* Area of the 1x1 degree cell at the equator on WGS84 is
     * 12308778361.469 m2 (GeographicLib Planimeter). */
    r = call(TA_GEO_OP_GEOM_MEASURE, cell, 0, "area", "",
             "mode=geodesic\naxis=gis");
    OK(r);
    NEAR(num(&r, "value"), 12308778361.469, 1);
    EQ_STR(r, "unit", "m2");
    res_free(&r);
    release(a);
    release(b);
    release(e0);
    release(e1);
    release(cell);
    ok("geodesic distance and area against GeographicLib");
}

/* EPSG Guidance Note 7-2 worked examples. */
static void
t_crs(void)
{
    static const struct {
        const char *what, *src, *dst, *in;
        double e, n, tol;
    } gn[] = {
        { "Transverse Mercator (OSGB 1936 / British National Grid)",
          "EPSG:4277", "EPSG:27700", "0.5 50.5", 577274.99, 69740.50, 0.01 },
        { "Lambert Conic Conformal 2SP (NAD27 / Texas South Central)",
          "EPSG:4267", "EPSG:32040", "-96 28.5", 2963503.91, 254759.80, 0.01 },
        { "Mercator variant A (Makassar / NEIEZ)",
          "EPSG:4257", "EPSG:3002", "120 -3", 5009726.58, 569150.82, 0.01 },
        { "Polar Stereographic variant A (WGS 84 / UPS North (N,E))",
          "EPSG:4326", "EPSG:32661", "44 73", 3320416.75, 632668.43, 0.01 },
        { "Popular Visualisation Pseudo-Mercator",
          "EPSG:4326", "EPSG:3857", "-100.333333333333333 24.381786944444444",
          -11169055.58, 2800000.00, 0.01 },
    };
    size_t i;
    res r;

    for (i = 0; i < sizeof(gn) / sizeof(gn[0]); i++)
    {
        double x, y;

        r = call(TA_GEO_OP_TRANSFORM_COORDS, 0, 0, "", gn[i].in,
                 "src=%s\ndst=%s\naxis=gis", gn[i].src, gn[i].dst);
        OK(r);
        if (sscanf(r.report, "%lf %lf", &x, &y) != 2)
            FAIL("no coordinates: %s", r.report);
        NEAR(x, gn[i].e, gn[i].tol);
        NEAR(y, gn[i].n, gn[i].tol);
        EQ_STR(r, "exact", "1");
        res_free(&r);
        printf("   GN7-2 %s\n", gn[i].what);
    }

    /* Axis order: EPSG:4326 authority order is lat, lon. */
    r = call(TA_GEO_OP_TRANSFORM_COORDS, 0, 0, "", "73 44",
             "src=EPSG:4326\ndst=EPSG:32661\naxis=authority");
    OK(r);
    {
        double x, y;

        sscanf(r.report, "%lf %lf", &x, &y);
        /* EPSG:32661 is northing, easting in authority order. */
        NEAR(x, 632668.43, 0.01);
        NEAR(y, 3320416.75, 0.01);
    }
    res_free(&r);
    r = call(TA_GEO_OP_TRANSFORM_COORDS, 0, 0, "", "44 73",
             "src=EPSG:4326\ndst=EPSG:32661");
    EXPECT_RC(r, TA_GEO_EINVAL);
    res_free(&r);

    /* A datum change is not exact and says how accurate it is. */
    r = call(TA_GEO_OP_TRANSFORM_COORDS, 0, 0, "", "-1.5 52.5",
             "src=EPSG:4326\ndst=EPSG:27700\naxis=gis");
    OK(r);
    EQ_STR(r, "exact", "0");
    printf("   4326->27700: %s\n", get(&r, "operation"));
    printf("     accuracy %s m,", get(&r, "accuracy"));
    printf(" grids '%s',", get(&r, "grids"));
    printf(" fallback %s\n", get(&r, "fallback"));
    res_free(&r);

    /* Outside the area of use. */
    r = call(TA_GEO_OP_TRANSFORM_COORDS, 0, 0, "", "150 -30",
             "src=EPSG:4326\ndst=EPSG:27700\naxis=gis");
    OK(r);
    EQ_STR(r, "out_of_domain", "1");
    res_free(&r);
    r = call(TA_GEO_OP_TRANSFORM_COORDS, 0, 0, "", "150 -30",
             "src=EPSG:4326\ndst=EPSG:27700\naxis=gis\nstrict_domain=1");
    EXPECT_RC(r, TA_GEO_EDOM);
    res_free(&r);

    r = call(TA_GEO_OP_CRS_INFO, 0, 0, "lookup", "EPSG:4326", NULL);
    OK(r);
    EQ_STR(r, "type", "geographic2d");
    EQ_STR(r, "lat_first", "1");
    res_free(&r);
    r = call(TA_GEO_OP_CRS_INFO, 0, 0, "compare", "EPSG:4326",
             "other=OGC:CRS84");
    OK(r);
    EQ_STR(r, "equivalent", "0");
    EQ_STR(r, "axis_swapped", "1");
    res_free(&r);
    r = call(TA_GEO_OP_CRS_INFO, 0, 0, "validate", "EPSG:999999", NULL);
    OK(r);
    EQ_STR(r, "valid", "0");
    res_free(&r);

    /* A geometry, Z preserved. */
    {
        int g = geom("wkt", "LINESTRING Z (0.5 50.5 10, 0.6 50.6 20)",
                     "EPSG:4277");

        r = call(TA_GEO_OP_TRANSFORM, g, 0, "", "",
                 "dst=EPSG:27700\naxis=gis");
        OK(r);
        {
            int t = r.h;
            res i2;

            res_free(&r);
            i2 = call(TA_GEO_OP_GEOM_INFO, t, 0, "", "", NULL);
            OK(i2);
            EQ_STR(i2, "dims", "xyz");
            EQ_STR(i2, "crs", "EPSG:27700");
            NEAR(num(&i2, "minx"), 577274.99, 0.01);
            NEAR(num(&i2, "minz"), 10, 1e-9);
            res_free(&i2);
            release(t);
        }
        release(g);
    }
    ok("CRS: GN7-2 projections, axis order, accuracy, domain");
}

static void
t_validation(void)
{
    res r;
    int h;
    static const struct { const char *wkt; const char *cat; } c[] = {
        { "POLYGON ((0 0, 2 2, 2 0, 0 2, 0 0))", "self_intersection" },
        { "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), (5 5, 5 6, 6 6, 5 5))",
          "hole_outside" },
        { "POLYGON ((0 0, 6 0, 6 6, 0 6, 0 0), (1 1, 1 5, 5 5, 5 1, 1 1), "
          "(2 2, 2 3, 3 3, 3 2, 2 2))", "nested_holes" },
        { "LINESTRING (0 0, 1 0, 1 0, 2 2)", "duplicate_vertex" },
        { "POLYGON EMPTY", "empty" },
    };
    size_t i;

    for (i = 0; i < sizeof(c) / sizeof(c[0]); i++)
    {
        h = geom("wkt", c[i].wkt, "");
        r = call(TA_GEO_OP_GEOM_CHECK, h, 0, "all", "", "id=g%zu", i);
        OK(r);
        if (strstr(r.report, c[i].cat) == NULL)
            FAIL("%s: no %s in\n%s", c[i].wkt, c[i].cat, r.report);
        EQ_STR(r, "ok", "0");
        res_free(&r);
        /* Validation never repairs. */
        r = call(TA_GEO_OP_GEOM_SERIALIZE, h, 0, "wkt", "", NULL);
        OK(r);
        res_free(&r);
        release(h);
    }
    h = geom("wkt", "POLYGON ((0 0, 2 2, 2 0, 0 2, 0 0))", "");
    r = call(TA_GEO_OP_GEOM_REPAIR, h, 0, "make_valid", "", NULL);
    OK(r);
    EQ_STR(r, "valid", "1");
    release(r.h);
    res_free(&r);
    r = call(TA_GEO_OP_GEOM_CHECK, h, 0, "validity", "", NULL);
    OK(r);
    EQ_STR(r, "valid", "0");
    res_free(&r);
    release(h);
    /* Orientation: a clockwise shell. */
    h = geom("wkt", "POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0))", "");
    r = call(TA_GEO_OP_GEOM_CHECK, h, 0, "orientation", "",
             "orientation=ccw");
    OK(r);
    EQ_STR(r, "misoriented", "1");
    res_free(&r);
    release(h);
    ok("validation categories, no implicit repair");
}

static void
t_generate(void)
{
    res a = call(TA_GEO_OP_GEN, 0, 0, "polygons", "",
                 "seed=42\ncount=200\nholes=1\nvertices=12");
    res b = call(TA_GEO_OP_GEN, 0, 0, "polygons", "",
                 "seed=42\ncount=200\nholes=1\nvertices=12");
    res c;
    char *p;
    int n = 0;

    OK(a);
    OK(b);
    if (strcmp(a.report, b.report) != 0)
        FAIL("same seed, different output");
    /* Every generated polygon is valid, as documented. */
    for (p = strtok(a.report, "\n"); p != NULL; p = strtok(NULL, "\n"))
    {
        char *wkt = strrchr(p, '\t') + 1;
        int h = geom("wkt", wkt, "");
        res v = call(TA_GEO_OP_GEOM_CHECK, h, 0, "validity", "", NULL);

        OK(v);
        EQ_STR(v, "valid", "1");
        res_free(&v);
        release(h);
        n++;
    }
    if (n != 200)
        FAIL("%d polygons", n);
    res_free(&a);
    res_free(&b);
    c = call(TA_GEO_OP_GEN, 0, 0, "invalid", "", "count=10");
    EXPECT_RC(c, TA_GEO_EINVAL);
    res_free(&c);
    c = call(TA_GEO_OP_GEN, 0, 0, "invalid", "", "count=20\nallow_invalid=1");
    OK(c);
    /* Every invalid item is reported with the category it promises. */
    for (p = strtok(c.report, "\n"); p != NULL; p = strtok(NULL, "\n"))
    {
        char fmt[8], expect[40], text[2048];
        long long id;
        double t;
        res g2, v;

        if (sscanf(p, "%lld\t%7s\t%39s\t%lf\t%2047[^\n]", &id, fmt, expect,
                   &t, text) != 5)
            FAIL("bad generator line %s", p);
        g2 = call(TA_GEO_OP_GEOM_PARSE, 0, 0, fmt, text, NULL);
        if (strcmp(expect, "unclosed_ring") == 0)
        {
            EXPECT_RC(g2, TA_GEO_ECORRUPT);
            EQ_STR(g2, "category", "unclosed_ring");
            res_free(&g2);
            continue;
        }
        OK(g2);
        v = call(TA_GEO_OP_GEOM_CHECK, g2.h, 0, "all", "", NULL);
        OK(v);
        if (strstr(v.report, expect) == NULL)
            FAIL("%s (%s): no %s in\n%s", text, fmt, expect, v.report);
        res_free(&v);
        release(g2.h);
        res_free(&g2);
    }
    res_free(&c);
    ok("deterministic generation, documented validity");
}

static void
t_dataset(void)
{
    char path[512], path2[512];
    res r, v;
    int ha, hb;

    snprintf(path, sizeof(path), "%s/gen.gpkg", dir);
    r = call(TA_GEO_OP_DS_WRITE, 0, 0, "generate", path,
             "driver=GPKG\ncrs=EPSG:3857\ngen=polygons\ncount=500\n"
             "bounds=0 0 100000 100000\nseed=3\n"
             "defects=invalid:4,dup_id:3,missing_geom:2,out_of_range:5,"
             "bad_attr:2,null_attr:1");
    OK(r);
    EQ_STR(r, "written", "500");
    res_free(&r);
    r = call(TA_GEO_OP_DS_OPEN, 0, 0, "", path, NULL);
    OK(r);
    ha = r.h;
    res_free(&r);
    v = call(TA_GEO_OP_DS_VALIDATE, ha, 0, "", "",
             "id_field=id\nexpect_types=Polygon,MultiPolygon\n"
             "expect_crs=EPSG:3857\nbounds=0 0 100000 100000\n"
             "range.value=0:999\nrequired=name\n"
             "schema=id:Integer64;name:String;value:Real");
    OK(v);
    EQ_STR(v, "features", "500");
    EQ_STR(v, "invalid_geometry", "4");
    EQ_STR(v, "duplicate_id", "3");
    EQ_STR(v, "missing_geometry", "2");
    /* out_of_range moves a feature off the map and makes it a point. */
    EQ_STR(v, "out_of_range_coordinate", "5");
    EQ_STR(v, "unexpected_geometry_type", "5");
    EQ_STR(v, "invalid_attribute", "3");
    EQ_STR(v, "crs_mismatch", "0");
    EQ_STR(v, "schema_mismatch", "0");
    res_free(&v);

    /* A copy compares equal, ordering aside; a perturbed copy does not. */
    snprintf(path2, sizeof(path2), "%s/copy.geojson", dir);
    r = call(TA_GEO_OP_DS_WRITE, ha, 0, "copy", path2, "driver=GeoJSON");
    OK(r);
    res_free(&r);
    r = call(TA_GEO_OP_DS_OPEN, 0, 0, "", path2, NULL);
    OK(r);
    hb = r.h;
    res_free(&r);
    r = call(TA_GEO_OP_DS_COMPARE, ha, hb, "", "",
             "id_field=id\ngeometry=exact");
    OK(r);
    printf("   copy: %s", r.result);
    EQ_STR(r, "geometry_diff", "0");
    EQ_STR(r, "attribute_diff", "0");
    res_free(&r);
    release(hb);

    /* Shapefile and the other formats read back what was written. */
    {
        static const char *const drv[] = { "ESRI Shapefile", "GeoJSON",
                                           "FlatGeobuf", "KML", "GML" };
        static const char *const ext[] = { "shp", "json", "fgb", "kml",
                                           "gml" };
        size_t i;

        for (i = 0; i < 5; i++)
        {
            snprintf(path2, sizeof(path2), "%s/g.%s", dir, ext[i]);
            r = call(TA_GEO_OP_DS_WRITE, 0, 0, "generate", path2,
                     "driver=%s\ncrs=EPSG:4326\ngen=points\ncount=50\n"
                     "seed=9", drv[i]);
            OK(r);
            res_free(&r);
            r = call(TA_GEO_OP_DS_OPEN, 0, 0, "", path2, NULL);
            OK(r);
            hb = r.h;
            res_free(&r);
            r = call(TA_GEO_OP_DS_INFO, hb, 0, "count", "", "exact=1");
            OK(r);
            EQ_STR(r, "count", "50");
            res_free(&r);
            release(hb);
        }
    }
    release(ha);

    /* A VRT is not opened unless asked for. */
    snprintf(path2, sizeof(path2), "%s/x.vrt", dir);
    {
        FILE *f = fopen(path2, "w");

        fprintf(f, "<OGRVRTDataSource><OGRVRTLayer name=\"x\"><SrcDataSource>"
                "%s</SrcDataSource></OGRVRTLayer></OGRVRTDataSource>\n",
                path);
        fclose(f);
    }
    r = call(TA_GEO_OP_DS_OPEN, 0, 0, "", path2, NULL);
    EXPECT_RC(r, TA_GEO_ECORRUPT);
    res_free(&r);
    ok("datasets: planted defects found exactly, copies compare equal");
}

static void
t_raster(void)
{
    char a[512], b[512];
    res r;
    int ha, hb;
    const double W = 300, H = 200;

    snprintf(a, sizeof(a), "%s/a.tif", dir);
    snprintf(b, sizeof(b), "%s/b.tif", dir);
    r = call(TA_GEO_OP_RASTER_WRITE, 0, 0, "", a,
             "width=300\nheight=200\npattern=gradient\na=5\nbx=2\nby=3\n"
             "type=Float64\ncrs=EPSG:32629\nblock=64\n"
             "geotransform=500000 10 0 4600000 0 -10");
    OK(r);
    res_free(&r);
    r = call(TA_GEO_OP_RASTER_WRITE, 0, 0, "", b,
             "width=300\nheight=200\npattern=gradient\na=5\nbx=2\nby=3\n"
             "type=Float64\ncrs=EPSG:32629\nblock=64\n"
             "geotransform=500000 10 0 4600000 0 -10\nnoise_pixels=25\n"
             "noise=0.5\nseed=4");
    OK(r);
    res_free(&r);
    r = call(TA_GEO_OP_DS_OPEN, 0, 0, "", a, "mode=raster");
    OK(r);
    ha = r.h;
    res_free(&r);
    r = call(TA_GEO_OP_DS_OPEN, 0, 0, "", b, "mode=raster");
    OK(r);
    hb = r.h;
    res_free(&r);
    r = call(TA_GEO_OP_RASTER_INFO, ha, 0, "", "", NULL);
    OK(r);
    EQ_STR(r, "crs", "EPSG:32629");
    EQ_STR(r, "width", "300");
    NEAR(num(&r, "res_x"), 10, 0);
    NEAR(num(&r, "maxx"), 503000, 0);
    res_free(&r);
    /* mean of a + bx x + by y = a + bx (W-1)/2 + by (H-1)/2 */
    r = call(TA_GEO_OP_RASTER_STATS, ha, 0, "", "", "tile=37");
    OK(r);
    NEAR(num(&r, "mean"), 5 + 2 * (W - 1) / 2 + 3 * (H - 1) / 2, 1e-9);
    NEAR(num(&r, "min"), 5, 0);
    NEAR(num(&r, "max"), 5 + 2 * (W - 1) + 3 * (H - 1), 0);
    res_free(&r);
    r = call(TA_GEO_OP_RASTER_COMPARE, ha, hb, "", "", "tolerance=0.1");
    OK(r);
    EQ_STR(r, "metadata_diff", "0");
    /* 25 positions, some may coincide. */
    if (num(&r, "pixel_diff") < 20 || num(&r, "pixel_diff") > 25)
        FAIL("pixel_diff %s", get(&r, "pixel_diff"));
    NEAR(num(&r, "max_abs_diff"), 0.5, 1e-12);
    res_free(&r);
    r = call(TA_GEO_OP_RASTER_COMPARE, ha, ha, "", "", NULL);
    OK(r);
    EQ_STR(r, "equal", "1");
    res_free(&r);
    release(ha);
    release(hb);
    ok("raster: metadata, tiled statistics in closed form, comparison");
}

static void
t_track(void)
{
    res g, r;
    char *truth = NULL, *noisy = NULL;
    size_t lt = 0, ln = 0;
    FILE *ft = open_memstream(&truth, &lt);
    FILE *fn = open_memstream(&noisy, &ln);
    char *p;
    int ht, hn;

    g = call(TA_GEO_OP_GEN, 0, 0, "trajectory", "",
             "count=600\nspeed=12\ninterval=1\nheading=30\nstart=-8.6 41.1");
    OK(g);
    for (p = strtok(g.report, "\n"); p != NULL; p = strtok(NULL, "\n"))
    {
        double t, x, y;
        long long id;
        char fmt[8], exp[32];

        if (sscanf(p, "%lld\t%7s\t%31s\t%lf\tPOINT (%lf %lf)", &id, fmt, exp,
                   &t, &x, &y) == 6)
            fprintf(ft, "%.17g %.17g %.17g\n", t, x, y);
    }
    res_free(&g);
    g = call(TA_GEO_OP_GEN, 0, 0, "trajectory", "",
             "count=600\nspeed=12\ninterval=1\nheading=30\nstart=-8.6 41.1\n"
             "noise=3\nseed=5\njump_every=100\njump=500\ndrop_every=70");
    OK(g);
    for (p = strtok(g.report, "\n"); p != NULL; p = strtok(NULL, "\n"))
    {
        double t, x, y;
        long long id;
        char fmt[8], exp[32];

        if (sscanf(p, "%lld\t%7s\t%31s\t%lf\tPOINT (%lf %lf)", &id, fmt, exp,
                   &t, &x, &y) == 6)
            fprintf(fn, "%.17g %.17g %.17g\n", t, x, y);
    }
    res_free(&g);
    fclose(ft);
    fclose(fn);
    r = call(TA_GEO_OP_TRACK_LOAD, 0, 0, "text", truth, "axis=gis");
    OK(r);
    ht = r.h;
    res_free(&r);
    r = call(TA_GEO_OP_TRACK_LOAD, 0, 0, "text", noisy, "axis=gis");
    OK(r);
    hn = r.h;
    res_free(&r);
    r = call(TA_GEO_OP_TRACK_EVAL, ht, 0, "sanity", "", "max_speed=15");
    OK(r);
    NEAR(num(&r, "length_m"), 12.0 * 599, 1e-6);
    EQ_STR(r, "ok", "1");
    res_free(&r);
    r = call(TA_GEO_OP_TRACK_EVAL, hn, 0, "sanity", "",
             "max_speed=100\nmax_jump=200\ninterval=1");
    OK(r);
    /* Samples 100..500 every 100 are thrown off by 500 m: a jump out
     * and back each, and drop_every=70 removes 8 samples. */
    EQ_STR(r, "jumps", "10");
    EQ_STR(r, "missing", "8");
    res_free(&r);
    r = call(TA_GEO_OP_TRACK_EVAL, hn, ht, "error", "", "threshold=100");
    OK(r);
    EQ_STR(r, "over_threshold", "5");
    printf("   noise 3 m: mean %.3f, p50 %.3f, rmse %.3f m\n", num(&r, "mean"),
           num(&r, "p50"), num(&r, "rmse"));
    res_free(&r);
    {
        int fence = geom("wkt", "POLYGON ((-8.62 41.09, -8.55 41.09, "
                         "-8.55 41.15, -8.62 41.15, -8.62 41.09))",
                         "EPSG:4326");

        r = call(TA_GEO_OP_TRACK_EVAL, ht, fence, "geofence", "",
                 "expect=inside");
        OK(r);
        printf("   geofence: inside %g, outside %g, transitions %g\n",
               num(&r, "inside"), num(&r, "outside"),
               num(&r, "transitions"));
        res_free(&r);
        release(fence);
    }
    release(ht);
    release(hn);
    free(truth);
    free(noisy);
    ok("tracking: length, jumps, gaps, error statistics");
}

static void
t_handles(void)
{
    int h = geom("wkt", "POINT (0 0)", "");
    res r;
    int i;

    release(h);
    r = call(TA_GEO_OP_GEOM_INFO, h, 0, "", "", NULL);
    EXPECT_RC(r, TA_GEO_EINVAL);
    res_free(&r);
    r = call(TA_GEO_OP_RELEASE, h, 0, "", "", NULL);
    EXPECT_RC(r, TA_GEO_EINVAL);
    res_free(&r);
    r = call(TA_GEO_OP_CONFIG, 0, 0, "", "", "max_handles=10");
    OK(r);
    res_free(&r);
    for (i = 0; i < 10; i++)
        geom("wkt", "POINT (0 0)", "");
    r = call(TA_GEO_OP_GEOM_PARSE, 0, 0, "wkt", "POINT (0 0)", NULL);
    EXPECT_RC(r, TA_GEO_E2BIG);
    res_free(&r);
    r = call(TA_GEO_OP_STATS, 0, 0, "", "", NULL);
    OK(r);
    EQ_STR(r, "live", "10");
    res_free(&r);
    r = call(TA_GEO_OP_CONFIG, 0, 0, "", "", "max_handles=4095");
    OK(r);
    res_free(&r);
    ok("handles: stale refused, limit enforced");
}

static void
t_perf(void)
{
    res r = call(TA_GEO_OP_PERF, 0, 0, "", "",
                 "what=transform\nsrc=EPSG:4326\ndst=EPSG:3763\n"
                 "iterations=5\ncount=20000");

    OK(r);
    printf("   transform 4326->3763: cold %.0f us, warm %.0f us per 20k "
           "(%.2g pt/s)\n", num(&r, "cold_us"), num(&r, "warm_us"),
           num(&r, "items_per_s"));
    res_free(&r);
    ok("perf");
}

int
main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <scratch dir>\n", argv[0]);
        return 2;
    }
    dir = argv[1];
    t_available();
    t_parse();
    t_bad_input();
    t_predicates();
    t_geodesic();
    t_crs();
    t_validation();
    t_generate();
    t_dataset();
    t_raster();
    t_track();
    t_handles();
    t_perf();
    ta_geo_shutdown();
    printf("all %d core contracts hold\n", n_ok);
    return 0;
}
