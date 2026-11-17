/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial engine: reference systems and transformations
 *
 * PROJ, through its C API. Three rules the operations keep:
 *
 *  - The axis order is always the caller's explicit choice: "gis"
 *    (x = easting / longitude) or "authority" (the order the CRS
 *    defines, latitude first for EPSG:4326). No call defaults it, so
 *    no call can swap axes behind the caller's back.
 *  - A transformation says which operation PROJ actually used, how
 *    accurate it is when that is known, which grids it needs and
 *    whether they are here, whether it was a ballpark, and whether a
 *    better candidate was passed over for want of a grid ("fallback").
 *    "exact" is claimed only for a conversion with no datum change.
 *  - Coordinates are checked against the source CRS's area of use, and
 *    non-finite results are reported, not passed on.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ta_geo_int.h"

/* ---- transforming a geometry -------------------------------------- */

static GEOSCoordSequence *
xf_seq(PJ *op, const GEOSCoordSequence *s, bool z, bool m, long long *bad,
       bool *fail)
{
    GEOSContextHandle_t h = geo_geos();
    unsigned int n = 0;
    unsigned int i;
    size_t stride = 2 + z + m;
    double *buf;
    GEOSCoordSequence *out;

    if (!GEOSCoordSeq_getSize_r(h, s, &n))
    {
        *fail = true;
        return NULL;
    }
    buf = malloc((n > 0 ? n : 1) * stride * sizeof(double));
    if (buf == NULL)
    {
        *fail = true;
        return NULL;
    }
    if (n > 0 && !GEOSCoordSeq_copyToBuffer_r(h, s, buf, z, m))
    {
        free(buf);
        *fail = true;
        return NULL;
    }
    if (n > 0)
    {
        proj_trans_generic(op, PJ_FWD,
                           buf, stride * sizeof(double), n,
                           buf + 1, stride * sizeof(double), n,
                           z ? buf + 2 : NULL, stride * sizeof(double),
                           z ? n : 0,
                           NULL, 0, 0);
    }
    for (i = 0; i < n; i++)
    {
        if (!isfinite(buf[i * stride]) || !isfinite(buf[i * stride + 1]) ||
            buf[i * stride] == HUGE_VAL)
            (*bad)++;
    }
    out = GEOSCoordSeq_copyFromBuffer_r(h, buf, n, z, m);
    free(buf);
    if (out == NULL)
        *fail = true;
    return out;
}

static GEOSGeometry *
xf_geom(PJ *op, const GEOSGeometry *g, long long *bad, bool *fail)
{
    GEOSContextHandle_t h = geo_geos();
    int t = GEOSGeomTypeId_r(h, g);
    bool z = GEOSHasZ_r(h, g) == 1;
    bool m = false;
    GEOSCoordSequence *s;
    GEOSGeometry *res = NULL;
    GEOSGeometry **parts = NULL;
    int n = 0;
    int i;

#if GEOS_AT_LEAST(3, 12)
    m = GEOSHasM_r(h, g) == 1;
#endif
    if (*fail)
        return NULL;
    if (GEOSisEmpty_r(h, g) == 1)
        return GEOSGeom_clone_r(h, g);
    switch (t)
    {
        case GEOS_POINT:
        case GEOS_LINESTRING:
        case GEOS_LINEARRING:
            s = xf_seq(op, GEOSGeom_getCoordSeq_r(h, g), z, m, bad, fail);
            if (s == NULL)
                return NULL;
            res = t == GEOS_POINT ? GEOSGeom_createPoint_r(h, s) :
                  t == GEOS_LINESTRING ? GEOSGeom_createLineString_r(h, s) :
                                         GEOSGeom_createLinearRing_r(h, s);
            break;

        case GEOS_POLYGON:
        {
            GEOSGeometry *shell = xf_geom(op, GEOSGetExteriorRing_r(h, g),
                                          bad, fail);

            n = GEOSGetNumInteriorRings_r(h, g);
            parts = calloc(n > 0 ? (size_t)n : 1, sizeof(*parts));
            for (i = 0; i < n && parts != NULL && !*fail; i++)
                parts[i] = xf_geom(op, GEOSGetInteriorRingN_r(h, g, i), bad,
                                   fail);
            if (shell == NULL || parts == NULL || *fail)
            {
                if (shell != NULL)
                    GEOSGeom_destroy_r(h, shell);
                for (i = 0; parts != NULL && i < n; i++)
                    if (parts[i] != NULL)
                        GEOSGeom_destroy_r(h, parts[i]);
                free(parts);
                *fail = true;
                return NULL;
            }
            res = GEOSGeom_createPolygon_r(h, shell, parts, (unsigned int)n);
            free(parts);
            break;
        }

        default:
            n = GEOSGetNumGeometries_r(h, g);
            parts = calloc(n > 0 ? (size_t)n : 1, sizeof(*parts));
            for (i = 0; i < n && parts != NULL && !*fail; i++)
                parts[i] = xf_geom(op, GEOSGetGeometryN_r(h, g, i), bad,
                                   fail);
            if (parts == NULL || *fail)
            {
                for (i = 0; parts != NULL && i < n; i++)
                    if (parts[i] != NULL)
                        GEOSGeom_destroy_r(h, parts[i]);
                free(parts);
                *fail = true;
                return NULL;
            }
            res = GEOSGeom_createCollection_r(h, t, parts, (unsigned int)n);
            free(parts);
            break;
    }
    if (res == NULL)
        *fail = true;
    return res;
}

int
geo_transform_geom(PJ *op, const GEOSGeometry *in, GEOSGeometry **out,
                   long long *n_bad, char **err)
{
    bool fail = false;

    *n_bad = 0;
    *out = xf_geom(op, in, n_bad, &fail);
    if (*out == NULL)
        return seterr(err, TA_GEO_EFAIL, "transformation failed: %s %s",
                      geo_geos_err(), geo_proj_err());
    return TA_GEO_OK;
}

/* ---- the operation ------------------------------------------------- */

typedef struct xform {
    PJ *src;
    PJ *dst;
    PJ *op;
    char src_def[256];
    char dst_def[256];
    char axis[16];
    bool check_domain;
    double aou[4];          /* west south east north of the source */
    bool have_aou;
    double aou2[4];         /* ... and of the destination */
    bool have_aou2;
    PJ *to_lonlat;          /* source -> its geodetic CRS, GIS order */
    bool swap_in;           /* input is in north-first order */
} xform;

static void
xform_free(xform *x)
{
    proj_destroy(x->src);
    proj_destroy(x->dst);
    proj_destroy(x->op);
    proj_destroy(x->to_lonlat);
    memset(x, 0, sizeof(*x));
}

static int
xform_axis(kv *opt, char *axis, size_t len, char **err)
{
    const char *a = kv_get(opt, "axis");

    if (a == NULL)
        return seterr(err, TA_GEO_EINVAL, "say axis=gis (x = easting or "
                      "longitude) or axis=authority (the CRS's own order): "
                      "the axis order is never assumed");
    if (strcmp(a, "gis") != 0 && strcmp(a, "authority") != 0)
        return seterr(err, TA_GEO_EINVAL, "axis is gis or authority, not "
                      "'%s'", a);
    snprintf(axis, len, "%s", a);
    return TA_GEO_OK;
}

static int
xform_init(xform *x, kv *opt, const char *src_def, char **err)
{
    PJ_CONTEXT *c = geo_proj();
    const char *dst = kv_get(opt, "dst");
    const char *opts[4] = { NULL, NULL, NULL, NULL };
    char acc_opt[64];
    int no = 0;
    bool ballpark = true;
    bool best = false;
    double acc = -1;
    int rc;

    memset(x, 0, sizeof(*x));
    if ((rc = xform_axis(opt, x->axis, sizeof(x->axis), err)) != 0)
        return rc;
    if ((rc = kv_bool(opt, "allow_ballpark", true, &ballpark, err)) != 0 ||
        (rc = kv_bool(opt, "only_best", false, &best, err)) != 0 ||
        (rc = kv_bool(opt, "check_domain", true, &x->check_domain,
                      err)) != 0 ||
        (rc = kv_dbl(opt, "accuracy", -1, &acc, err)) != 0)
        return rc;
    if (src_def == NULL || *src_def == '\0')
        return seterr(err, TA_GEO_EINVAL, "no source CRS (src=, or a "
                      "geometry with a CRS)");
    if (dst == NULL || *dst == '\0')
        return seterr(err, TA_GEO_EINVAL, "no destination CRS (dst=)");
    geo_crs_norm(src_def, x->src_def, sizeof(x->src_def));
    geo_crs_norm(dst, x->dst_def, sizeof(x->dst_def));
    x->src = geo_crs_create(x->src_def, err);
    if (x->src == NULL)
        return TA_GEO_ENOENT;
    x->dst = geo_crs_create(x->dst_def, err);
    if (x->dst == NULL)
    {
        xform_free(x);
        return TA_GEO_ENOENT;
    }
    if (!ballpark)
        opts[no++] = "ALLOW_BALLPARK=NO";
    if (best)
        opts[no++] = "ONLY_BEST=YES";
    if (acc >= 0)
    {
        snprintf(acc_opt, sizeof(acc_opt), "ACCURACY=%.17g", acc);
        opts[no++] = acc_opt;
    }
    x->op = proj_create_crs_to_crs_from_pj(c, x->src, x->dst, NULL, opts);
    if (x->op == NULL)
    {
        rc = seterr(err, TA_GEO_ENOENT, "no operation from %s to %s: %s",
                    x->src_def, x->dst_def, geo_proj_err());
        xform_free(x);
        return rc;
    }
    if (strcmp(x->axis, "gis") == 0)
    {
        PJ *n = proj_normalize_for_visualization(c, x->op);

        if (n == NULL)
        {
            rc = seterr(err, TA_GEO_EFAIL, "cannot normalise the axis order: "
                        "%s", geo_proj_err());
            xform_free(x);
            return rc;
        }
        proj_destroy(x->op);
        x->op = n;
    }
    if (x->check_domain)
    {
        const char *nm = NULL;

        x->have_aou = proj_get_area_of_use(c, x->src, &x->aou[0], &x->aou[1],
                                           &x->aou[2], &x->aou[3], &nm) &&
                      x->aou[0] > -1000;
        x->have_aou2 = proj_get_area_of_use(c, x->dst, &x->aou2[0],
                                            &x->aou2[1], &x->aou2[2],
                                            &x->aou2[3], &nm) &&
                       x->aou2[0] > -1000;
        if (x->have_aou || x->have_aou2)
        {
            PJ *geod = proj_crs_get_geodetic_crs(c, x->src);
            PJ *op = geod != NULL ?
                     proj_create_crs_to_crs_from_pj(c, x->src, geod, NULL,
                                                    NULL) : NULL;

            if (op != NULL)
            {
                x->to_lonlat = proj_normalize_for_visualization(c, op);
                proj_destroy(op);
            }
            proj_destroy(geod);
            /* to_lonlat reads GIS order; authority-order input whose
             * first axis is northing or latitude is swapped first. */
            x->swap_in = strcmp(x->axis, "authority") == 0 &&
                         geo_crs_lat_first(x->src);
        }
    }
    return TA_GEO_OK;
}

static bool
in_box(const double *b, double lon, double lat)
{
    /* A small margin: a coordinate on the boundary is in. */
    if (lat < b[1] - 1e-9 || lat > b[3] + 1e-9)
        return false;
    if (b[0] <= b[2])
        return lon >= b[0] - 1e-9 && lon <= b[2] + 1e-9;
    /* Crosses the antimeridian. */
    return lon >= b[0] - 1e-9 || lon <= b[2] + 1e-9;
}

/* Is a source coordinate inside the areas of use of the source and of
 * the destination CRS? Both are published as lon/lat boxes. */
static bool
in_domain(xform *x, double cx, double cy)
{
    PJ_COORD c;
    double lon, lat;

    if (x->to_lonlat == NULL)
        return true;
    c = x->swap_in ? proj_coord(cy, cx, 0, 0) : proj_coord(cx, cy, 0, 0);
    c = proj_trans(x->to_lonlat, PJ_FWD, c);
    lon = c.v[0];
    lat = c.v[1];
    if (!isfinite(lon) || !isfinite(lat))
        return false;
    return (!x->have_aou || in_box(x->aou, lon, lat)) &&
           (!x->have_aou2 || in_box(x->aou2, lon, lat));
}

/* Describe one coordinate operation: name, id, accuracy, grids. */
static void
op_describe(PJ *op, sbuf *res, int *grids_missing, double *accuracy,
            bool *ballpark)
{
    PJ_CONTEXT *c = geo_proj();
    const char *auth = proj_get_id_auth_name(op, 0);
    const char *code = proj_get_id_code(op, 0);
    int ng = proj_coordoperation_get_grid_used_count(c, op);
    int i;
    sbuf grids = SBUF_INIT;

    *accuracy = proj_coordoperation_get_accuracy(c, op);
    *ballpark = proj_coordoperation_has_ballpark_transformation(c, op) != 0;
    *grids_missing = 0;
    for (i = 0; i < ng; i++)
    {
        const char *sn = NULL, *fn = NULL, *pn = NULL, *url = NULL;
        int direct = 0, open = 0, avail = 0;

        if (proj_coordoperation_get_grid_used(c, op, i, &sn, &fn, &pn, &url,
                                              &direct, &open, &avail))
        {
            sb_printf(&grids, "%s%s%s", i > 0 ? "," : "", sn,
                      avail ? "" : "(missing)");
            if (!avail)
                (*grids_missing)++;
        }
    }
    if (res != NULL)
    {
        sb_kv_str(res, "operation", proj_get_name(op));
        if (auth != NULL && code != NULL)
            sb_printf(res, "operation_id=%s:%s\n", auth, code);
        else
            sb_kv_str(res, "operation_id", "");
        sb_kv_num(res, "accuracy", *accuracy);
        sb_kv_int(res, "ballpark", *ballpark);
        sb_kv_str(res, "grids", grids.p != NULL ? grids.p : "");
        sb_kv_int(res, "grids_missing", *grids_missing);
    }
    sb_free(&grids);
}

/*
 * The candidates PROJ knows for src -> dst whatever the grids, best
 * first: is there one more accurate than the operation used, that
 * needs a grid that is not here? Then the result is a fallback.
 */
static void
xform_meta(xform *x, PJ *used, sbuf *res, sbuf *rep)
{
    PJ_CONTEXT *c = geo_proj();
    PJ_OPERATION_FACTORY_CONTEXT *fc =
        proj_create_operation_factory_context(c, NULL);
    PJ_OBJ_LIST *l = NULL;
    int n = 0;
    int i;
    double used_acc;
    bool used_bp;
    int used_missing;
    bool fallback = false;
    double best_acc = -1;
    bool conversion;
    PJ_TYPE ut;

    sb_kv_str(res, "src", x->src_def);
    sb_kv_str(res, "dst", x->dst_def);
    sb_kv_str(res, "axis", x->axis);
    op_describe(used, res, &used_missing, &used_acc, &used_bp);
    ut = proj_get_type(used);
    conversion = ut == PJ_TYPE_CONVERSION;
    if (fc != NULL)
    {
        proj_operation_factory_context_set_grid_availability_use(c, fc,
            PROJ_GRID_AVAILABILITY_IGNORED);
        proj_operation_factory_context_set_spatial_criterion(c, fc,
            PROJ_SPATIAL_CRITERION_PARTIAL_INTERSECTION);
        l = proj_create_operations(c, x->src, x->dst, fc);
    }
    if (l != NULL)
        n = proj_list_get_count(l);
    for (i = 0; i < n; i++)
    {
        PJ *cand = proj_list_get(c, l, i);
        double acc;
        bool bp;
        int miss;

        if (cand == NULL)
            continue;
        op_describe(cand, NULL, &miss, &acc, &bp);
        if (i == 0)
            best_acc = acc;
        if (miss > 0 && acc >= 0 && !bp &&
            (used_acc < 0 || used_bp || acc < used_acc))
            fallback = true;
        if ((long long)i < geo_lim.max_report)
        {
            sb_field(rep, proj_get_name(cand));
            sb_puts(rep, "\t");
            sb_num(rep, acc);
            sb_printf(rep, "\t%d\t%d\n", bp, miss);
        }
        proj_destroy(cand);
    }
    if (n == 0 && proj_get_type(used) != PJ_TYPE_CONCATENATED_OPERATION &&
        conversion)
        best_acc = 0;
    sb_kv_int(res, "candidates", n);
    sb_kv_num(res, "best_accuracy", best_acc);
    sb_kv_int(res, "fallback", fallback || (used_missing > 0));
    /* Exact: a conversion (projection) with no datum change, or an
     * operation whose published accuracy is zero, and not a ballpark. */
    sb_kv_int(res, "exact", !used_bp && (conversion || used_acc == 0));
    proj_list_destroy(l);
    proj_operation_factory_context_destroy(fc);
}

static PJ *
last_used(PJ *op)
{
    PJ *u = proj_trans_get_last_used_operation(op);

    return u != NULL ? u : proj_clone(geo_proj(), op);
}

static int
op_transform(geo_req *r)
{
    geo_geom *o = geo_obj_get(r->ha, GEO_O_GEOM, r->err);
    xform x;
    GEOSGeometry *out = NULL;
    long long bad = 0;
    long long outside = 0;
    bool partial = false;
    bool strict = false;
    PJ *used;
    int rc;

    if (o == NULL)
        return TA_GEO_EINVAL;
    if ((rc = kv_bool(r->opt, "allow_partial", false, &partial,
                      r->err)) != 0 ||
        (rc = kv_bool(r->opt, "strict_domain", false, &strict, r->err)) != 0)
        return rc;
    if ((rc = xform_init(&x, r->opt, kv_str(r->opt, "src", o->crs),
                         r->err)) != 0)
        return rc;
    if (x.check_domain)
    {
        GEOSContextHandle_t h = geo_geos();
        GEOSGeometry *pts = GEOSGeom_extractUniquePoints_r(h, o->g);
        int i;
        int n = pts != NULL ? GEOSGetNumGeometries_r(h, pts) : 0;

        for (i = 0; i < n; i++)
        {
            double px, py;
            const GEOSGeometry *p = GEOSGetGeometryN_r(h, pts, i);

            GEOSGeomGetX_r(h, p, &px);
            GEOSGeomGetY_r(h, p, &py);
            outside += !in_domain(&x, px, py);
        }
        if (pts != NULL)
            GEOSGeom_destroy_r(h, pts);
    }
    if (strict && outside > 0)
    {
        sb_kv_int(r->res, "out_of_domain", outside);
        rc = seterr(r->err, TA_GEO_EDOM, "%lld coordinates outside the area "
                    "of use of %s", outside, x.src_def);
        xform_free(&x);
        return rc;
    }
    rc = geo_transform_geom(x.op, o->g, &out, &bad, r->err);
    if (rc != 0)
    {
        xform_free(&x);
        return rc;
    }
    used = last_used(x.op);
    xform_meta(&x, used, r->res, r->rep);
    proj_destroy(used);
    sb_kv_int(r->res, "out_of_domain", outside);
    sb_kv_int(r->res, "nonfinite", bad);
    if (bad > 0 && !partial)
    {
        GEOSGeom_destroy_r(geo_geos(), out);
        rc = seterr(r->err, TA_GEO_EDOM, "%lld coordinates did not "
                    "transform (outside the domain of the operation)", bad);
        xform_free(&x);
        return rc;
    }
    rc = geo_geom_new(out, x.dst_def, r->handle, r->err);
    xform_free(&x);
    return rc;
}

static int
op_transform_coords(geo_req *r)
{
    xform x;
    const char *p = r->text;
    long long n = 0;
    long long bad = 0;
    long long outside = 0;
    bool inverse = false;
    bool strict = false;
    PJ *used = NULL;
    sbuf coords = SBUF_INIT;
    int rc;

    if ((rc = kv_bool(r->opt, "inverse", false, &inverse, r->err)) != 0 ||
        (rc = kv_bool(r->opt, "strict_domain", false, &strict, r->err)) != 0)
        return rc;
    if ((rc = xform_init(&x, r->opt, kv_get(r->opt, "src"), r->err)) != 0)
        return rc;
    while (*p != '\0')
    {
        double v[4] = { 0, 0, 0, HUGE_VAL };
        int k = sscanf(p, "%lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3]);
        const char *eol = strchr(p, '\n');
        PJ_COORD c;

        if (k >= 2)
        {
            if (!inverse && !in_domain(&x, v[0], v[1]))
                outside++;
            c = proj_coord(v[0], v[1], v[2], v[3]);
            c = proj_trans(x.op, inverse ? PJ_INV : PJ_FWD, c);
            if (!isfinite(c.v[0]) || !isfinite(c.v[1]) ||
                c.v[0] == HUGE_VAL)
                bad++;
            if (used == NULL && !inverse)
                used = last_used(x.op);
            sb_num(&coords, c.v[0]);
            sb_puts(&coords, " ");
            sb_num(&coords, c.v[1]);
            if (k >= 3)
            {
                sb_puts(&coords, " ");
                sb_num(&coords, c.v[2]);
            }
            sb_puts(&coords, "\n");
            n++;
        }
        else if (strspn(p, " \t\r") != (size_t)(eol != NULL ? eol - p :
                                                 (long)strlen(p)))
        {
            sb_free(&coords);
            proj_destroy(used);
            xform_free(&x);
            return seterr(r->err, TA_GEO_ECORRUPT, "coordinate line %lld is "
                          "not 'x y [z [t]]'", n + 1);
        }
        if (eol == NULL)
            break;
        p = eol + 1;
    }
    if (used == NULL)
        used = proj_clone(geo_proj(), x.op);
    xform_meta(&x, used, r->res, r->rep);
    /* The candidates went to the report; the coordinates replace them. */
    sb_free(r->rep);
    sb_put(r->rep, coords.p != NULL ? coords.p : "", coords.len);
    sb_free(&coords);
    proj_destroy(used);
    sb_kv_int(r->res, "count", n);
    sb_kv_int(r->res, "out_of_domain", outside);
    sb_kv_int(r->res, "nonfinite", bad);
    xform_free(&x);
    if (strict && outside > 0)
        return seterr(r->err, TA_GEO_EDOM, "%lld coordinates outside the "
                      "area of use", outside);
    return TA_GEO_OK;
}

static int
op_transform_bounds(geo_req *r)
{
    xform x;
    double b[4];
    double o[4];
    long long dens;
    bool strict;
    int rc;

    if (sscanf(r->text, "%lf %lf %lf %lf", &b[0], &b[1], &b[2], &b[3]) != 4)
        return seterr(r->err, TA_GEO_EINVAL, "bounds are 'minx miny maxx "
                      "maxy'");

    /* Bounds are not checked point by point; the option is accepted so
     * one option set serves every transformation call. */
    if ((rc = kv_long(r->opt, "densify", 21, &dens, r->err)) != 0 ||
        (rc = kv_bool(r->opt, "strict_domain", false, &strict, r->err)) != 0)
        return rc;
    if ((rc = xform_init(&x, r->opt, kv_get(r->opt, "src"), r->err)) != 0)
        return rc;
    if (!proj_trans_bounds(geo_proj(), x.op, PJ_FWD, b[0], b[1], b[2], b[3],
                           &o[0], &o[1], &o[2], &o[3], (int)dens))
    {
        rc = seterr(r->err, TA_GEO_EDOM, "bounds do not transform: %s",
                    geo_proj_err());
        xform_free(&x);
        return rc;
    }
    sb_kv_num(r->res, "minx", o[0]);
    sb_kv_num(r->res, "miny", o[1]);
    sb_kv_num(r->res, "maxx", o[2]);
    sb_kv_num(r->res, "maxy", o[3]);
    {
        PJ *u = proj_clone(geo_proj(), x.op);

        xform_meta(&x, u, r->res, r->rep);
        proj_destroy(u);
    }
    xform_free(&x);
    return TA_GEO_OK;
}

/* ---- CRS information ---------------------------------------------- */

static const char *
type_name(PJ_TYPE t)
{
    switch (t)
    {
        case PJ_TYPE_GEOGRAPHIC_2D_CRS: return "geographic2d";
        case PJ_TYPE_GEOGRAPHIC_3D_CRS: return "geographic3d";
        case PJ_TYPE_GEOCENTRIC_CRS: return "geocentric";
        case PJ_TYPE_PROJECTED_CRS: return "projected";
        case PJ_TYPE_VERTICAL_CRS: return "vertical";
        case PJ_TYPE_COMPOUND_CRS: return "compound";
        case PJ_TYPE_BOUND_CRS: return "bound";
        case PJ_TYPE_ENGINEERING_CRS: return "engineering";
        case PJ_TYPE_TEMPORAL_CRS: return "temporal";
        case PJ_TYPE_DERIVED_PROJECTED_CRS: return "derived_projected";
        default: return "other";
    }
}

static void
crs_describe(PJ *crs, sbuf *res, sbuf *rep)
{
    PJ_CONTEXT *c = geo_proj();
    const char *auth = proj_get_id_auth_name(crs, 0);
    const char *code = proj_get_id_code(crs, 0);
    double w, s, e, n;
    const char *aname = NULL;
    PJ *cs = proj_crs_get_coordinate_system(c, crs);
    PJ *datum = proj_crs_get_datum_forced(c, crs);
    PJ *ell = proj_get_ellipsoid(c, crs);
    char unit[64];
    const char *wkt;
    int i;
    int na = cs != NULL ? proj_cs_get_axis_count(c, cs) : 0;

    sb_kv_str(res, "name", proj_get_name(crs));
    sb_kv_str(res, "type", type_name(proj_get_type(crs)));
    sb_kv_str(res, "auth", auth != NULL ? auth : "");
    sb_kv_str(res, "code", code != NULL ? code : "");
    sb_kv_int(res, "deprecated", proj_is_deprecated(crs));
    sb_kv_int(res, "geographic", geo_crs_is_geographic(crs));
    sb_kv_int(res, "lat_first", geo_crs_lat_first(crs));
    geo_crs_unit(crs, unit, sizeof(unit));
    sb_kv_str(res, "unit", unit);
    sb_kv_int(res, "axes", na);
    for (i = 0; i < na; i++)
    {
        const char *nm = NULL, *ab = NULL, *dir = NULL, *un = NULL;
        char key[16];

        proj_cs_get_axis_info(c, cs, i, &nm, &ab, &dir, NULL, &un, NULL,
                              NULL);
        snprintf(key, sizeof(key), "axis%d", i + 1);
        sb_printf(res, "%s=%s|%s|%s|%s\n", key, nm != NULL ? nm : "",
                  ab != NULL ? ab : "", dir != NULL ? dir : "",
                  un != NULL ? un : "");
    }
    if (datum != NULL)
        sb_kv_str(res, "datum", proj_get_name(datum));
    if (ell != NULL)
    {
        double a = 0, b = 0, inv_f = 0;
        int comp = 0;

        proj_ellipsoid_get_parameters(c, ell, &a, &b, &comp, &inv_f);
        sb_kv_str(res, "ellipsoid", proj_get_name(ell));
        sb_kv_num(res, "semi_major", a);
        sb_kv_num(res, "inv_flattening", inv_f);
    }
    if (proj_get_area_of_use(c, crs, &w, &s, &e, &n, &aname) && w > -1000)
    {
        sb_printf(res, "area_of_use=");
        sb_num(res, w);
        sb_puts(res, " ");
        sb_num(res, s);
        sb_puts(res, " ");
        sb_num(res, e);
        sb_puts(res, " ");
        sb_num(res, n);
        sb_puts(res, "\n");
        sb_kv_str(res, "area_name", aname);
    }
    wkt = proj_as_wkt(c, crs, PJ_WKT2_2019, NULL);
    if (wkt != NULL)
        sb_puts(rep, wkt);
    proj_destroy(cs);
    proj_destroy(datum);
    proj_destroy(ell);
}

static int
op_crs_info(geo_req *r)
{
    const char *k = *r->kind != '\0' ? r->kind : "lookup";
    PJ *a;
    char *why = NULL;

    if (strcmp(k, "lookup") == 0)
    {
        a = geo_crs_create(r->text, r->err);
        if (a == NULL)
            return TA_GEO_ENOENT;
        crs_describe(a, r->res, r->rep);
        proj_destroy(a);
        return TA_GEO_OK;
    }
    if (strcmp(k, "validate") == 0)
    {
        a = geo_crs_create(r->text, &why);
        sb_kv_int(r->res, "valid", a != NULL);
        if (a == NULL)
        {
            sb_kv_str(r->res, "reason", why);
            free(why);
            return TA_GEO_OK;
        }
        crs_describe(a, r->res, r->rep);
        if (proj_is_deprecated(a))
            sb_kv_str(r->res, "warning", "deprecated CRS");
        proj_destroy(a);
        return TA_GEO_OK;
    }
    if (strcmp(k, "compare") == 0)
    {
        PJ *b;
        PJ *ga, *gb;
        bool strict, equiv, ex_axis;
        const char *other = kv_get(r->opt, "other");

        if (other == NULL)
            return seterr(r->err, TA_GEO_EINVAL, "compare needs other=");
        a = geo_crs_create(r->text, r->err);
        if (a == NULL)
            return TA_GEO_ENOENT;
        b = geo_crs_create(other, r->err);
        if (b == NULL)
        {
            proj_destroy(a);
            return TA_GEO_ENOENT;
        }
        strict = proj_is_equivalent_to_with_ctx(geo_proj(), a, b,
                                                PJ_COMP_STRICT);
        equiv = proj_is_equivalent_to_with_ctx(geo_proj(), a, b,
                                               PJ_COMP_EQUIVALENT);
        ex_axis = proj_is_equivalent_to_with_ctx(geo_proj(), a, b,
                      PJ_COMP_EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS);
        ga = proj_crs_get_geodetic_crs(geo_proj(), a);
        gb = proj_crs_get_geodetic_crs(geo_proj(), b);
        sb_kv_int(r->res, "strict", strict);
        sb_kv_int(r->res, "equivalent", equiv);
        sb_kv_int(r->res, "equivalent_except_axis", ex_axis);
        sb_kv_int(r->res, "axis_swapped", ex_axis && !equiv);
        sb_kv_int(r->res, "same_geodetic", ga != NULL && gb != NULL &&
                  proj_is_equivalent_to_with_ctx(geo_proj(), ga, gb,
                      PJ_COMP_EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS));
        sb_kv_str(r->res, "a", proj_get_name(a));
        sb_kv_str(r->res, "b", proj_get_name(b));
        proj_destroy(ga);
        proj_destroy(gb);
        proj_destroy(a);
        proj_destroy(b);
        return TA_GEO_OK;
    }
    return seterr(r->err, TA_GEO_EINVAL, "unknown CRS operation '%s' "
                  "(lookup, validate, compare)", k);
}

/* ---- lon/lat for geodesics ---------------------------------------- */

static int
swap_xy(double *x, double *y, void *data)
{
    double t = *x;

    (void)data;
    *x = *y;
    *y = t;
    return 1;
}

int
geo_lonlat(const GEOSGeometry *g, const char *crs_def, const char *axis,
           GEOSGeometry **out, PJ **geodetic, char **err)
{
    GEOSContextHandle_t h = geo_geos();
    PJ_CONTEXT *c = geo_proj();
    PJ *crs = geo_crs_create(crs_def, err);
    bool gis;
    int rc = TA_GEO_OK;

    *out = NULL;
    *geodetic = NULL;
    if (crs == NULL)
        return TA_GEO_ENOENT;
    if (strcmp(axis, "gis") != 0 && strcmp(axis, "authority") != 0)
    {
        proj_destroy(crs);
        return seterr(err, TA_GEO_EINVAL, "axis is gis or authority");
    }
    gis = strcmp(axis, "gis") == 0;
    if (geo_crs_is_geographic(crs))
    {
        *geodetic = crs;
        if (!gis && geo_crs_lat_first(crs))
            *out = GEOSGeom_transformXY_r(h, g, swap_xy, NULL);
        else
            *out = GEOSGeom_clone_r(h, g);
        if (*out == NULL)
            rc = seterr(err, TA_GEO_EFAIL, "copy: %s", geo_geos_err());
        return rc;
    }
    *geodetic = proj_crs_get_geodetic_crs(c, crs);
    if (*geodetic == NULL || proj_get_type(*geodetic) ==
                             PJ_TYPE_GEOCENTRIC_CRS)
    {
        proj_destroy(crs);
        proj_destroy(*geodetic);
        *geodetic = NULL;
        return seterr(err, TA_GEO_ENOTSUP, "no geographic base CRS in %s",
                      crs_def);
    }
    {
        PJ *op = proj_create_crs_to_crs_from_pj(c, crs, *geodetic, NULL,
                                                NULL);
        PJ *norm = op != NULL ? proj_normalize_for_visualization(c, op) : NULL;
        long long bad = 0;

        if (norm == NULL)
            rc = seterr(err, TA_GEO_EFAIL, "no conversion to %s: %s",
                        proj_get_name(*geodetic), geo_proj_err());
        else if (!gis)
        {
            /* Normalised on both sides; the caller's input is in the
             * CRS's own order: swap in first when that order is not
             * x/easting first. */
            GEOSGeometry *in = geo_crs_lat_first(crs) ?
                GEOSGeom_transformXY_r(h, g, swap_xy, NULL) :
                GEOSGeom_clone_r(h, g);

            rc = in != NULL ? geo_transform_geom(norm, in, out, &bad, err) :
                              seterr(err, TA_GEO_EFAIL, "copy failed");
            if (in != NULL)
                GEOSGeom_destroy_r(h, in);
        }
        else
            rc = geo_transform_geom(norm, g, out, &bad, err);
        if (rc == 0 && bad > 0)
        {
            GEOSGeom_destroy_r(h, *out);
            *out = NULL;
            rc = seterr(err, TA_GEO_EDOM, "%lld coordinates have no "
                        "geographic position", bad);
        }
        proj_destroy(op);
        proj_destroy(norm);
    }
    proj_destroy(crs);
    if (rc != 0)
    {
        proj_destroy(*geodetic);
        *geodetic = NULL;
    }
    return rc;
}

int
geo_op_crs(ta_geo_op op, geo_req *r)
{
    switch (op)
    {
        case TA_GEO_OP_CRS_INFO: return op_crs_info(r);
        case TA_GEO_OP_TRANSFORM: return op_transform(r);
        case TA_GEO_OP_TRANSFORM_COORDS: return op_transform_coords(r);
        case TA_GEO_OP_TRANSFORM_BOUNDS: return op_transform_bounds(r);
        default:
            return seterr(r->err, TA_GEO_EINVAL, "not a CRS operation");
    }
}
