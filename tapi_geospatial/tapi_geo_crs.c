/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: reference systems and transformations
 */

#define TE_LGR_USER     "TAPI GEO CRS"

#include "te_config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "logger_api.h"

#include "tapi_geo_crs.h"
#include "tapi_geo_internal.h"

/* See description in tapi_geo_crs.h */
te_errno
tapi_geo_crs_lookup(rcf_rpc_server *rpcs, const char *crs,
                    tapi_geo_crs_info *i, te_string *wkt2)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_CRS_INFO, 0, 0, "lookup",
                                crs, "", &r);
    char aou[160];
    unsigned int k;

    if (rc == 0)
    {
        memset(i, 0, sizeof(*i));
        TGEO_STR(&r, "name", i->name);
        TGEO_STR(&r, "type", i->type);
        TGEO_STR(&r, "auth", i->auth);
        TGEO_STR(&r, "code", i->code);
        TGEO_STR(&r, "unit", i->unit);
        TGEO_STR(&r, "datum", i->datum);
        TGEO_STR(&r, "ellipsoid", i->ellipsoid);
        TGEO_STR(&r, "area_name", i->area_name);
        i->deprecated = tapi_geo_res_int(&r, "deprecated", 0) != 0;
        i->geographic = tapi_geo_res_int(&r, "geographic", 0) != 0;
        i->lat_first = tapi_geo_res_int(&r, "lat_first", 0) != 0;
        i->axes = (unsigned int)tapi_geo_res_int(&r, "axes", 0);
        for (k = 0; k < 3 && k < i->axes; k++)
        {
            char key[8];

            snprintf(key, sizeof(key), "axis%u", k + 1);
            if (tapi_geo_res_str(&r, key, i->axis[k],
                                 sizeof(i->axis[k])) == NULL)
                i->axis[k][0] = '\0';
        }
        i->semi_major = tapi_geo_res_dbl(&r, "semi_major");
        i->inv_flattening = tapi_geo_res_dbl(&r, "inv_flattening");
        i->area_of_use.minx = i->area_of_use.miny = NAN;
        i->area_of_use.maxx = i->area_of_use.maxy = NAN;
        if (tapi_geo_res_str(&r, "area_of_use", aou, sizeof(aou)) != NULL)
            sscanf(aou, "%lf %lf %lf %lf", &i->area_of_use.minx,
                   &i->area_of_use.miny, &i->area_of_use.maxx,
                   &i->area_of_use.maxy);
        if (wkt2 != NULL && r.report.ptr != NULL)
            te_string_append(wkt2, "%s", r.report.ptr);
    }
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo_crs.h */
te_errno
tapi_geo_crs_validate(rcf_rpc_server *rpcs, const char *crs, bool *valid,
                      te_string *reason)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_CRS_INFO, 0, 0, "validate",
                                crs, "", &r);

    if (rc == 0)
    {
        char why[512];

        *valid = tapi_geo_res_int(&r, "valid", 0) != 0;
        if (reason != NULL &&
            tapi_geo_res_str(&r, "reason", why, sizeof(why)) != NULL)
            te_string_append(reason, "%s", why);
    }
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo_crs.h */
te_errno
tapi_geo_crs_compare(rcf_rpc_server *rpcs, const char *a, const char *b,
                     tapi_geo_crs_cmp *cmp)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    te_errno rc;

    te_string_append(&p, "other=%s", b);
    /* An option is one line; WKT reads the same with spaces. */
    {
        char *c;

        for (c = p.ptr; c != NULL && *c != '\0'; c++)
            if (*c == '\n' || *c == '\r')
                *c = ' ';
    }
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_CRS_INFO, 0, 0, "compare", a, p.ptr,
                       &r);
    if (rc == 0)
    {
        cmp->strict = tapi_geo_res_int(&r, "strict", 0) != 0;
        cmp->equivalent = tapi_geo_res_int(&r, "equivalent", 0) != 0;
        cmp->equivalent_except_axis =
            tapi_geo_res_int(&r, "equivalent_except_axis", 0) != 0;
        cmp->axis_swapped = tapi_geo_res_int(&r, "axis_swapped", 0) != 0;
        cmp->same_geodetic = tapi_geo_res_int(&r, "same_geodetic", 0) != 0;
    }
    te_string_free(&p);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

static void
xform_params(te_string *p, const char *src, const char *dst,
             tapi_geo_axis axis, const tapi_geo_xform_opts *o)
{
    tapi_geo_xform_opts def = TAPI_GEO_XFORM_OPTS_DEFAULT;

    if (o == NULL)
        o = &def;
    if (src != NULL)
        te_string_append(p, "src=%s\n", src);
    te_string_append(p, "dst=%s\naxis=%s\nallow_ballpark=%d\nonly_best=%d\n"
                     "strict_domain=%d\n", dst, tapi_geo_axis2str(axis),
                     o->allow_ballpark, o->only_best, o->strict_domain);
    if (o->accuracy >= 0)
        te_string_append(p, "accuracy=%.17g\n", o->accuracy);
}

static void
info_read(const tapi_geo_res *r, tapi_geo_xform_info *i)
{
    if (i == NULL)
        return;
    memset(i, 0, sizeof(*i));
    TGEO_STR(r, "src", i->src);
    TGEO_STR(r, "dst", i->dst);
    TGEO_STR(r, "axis", i->axis);
    TGEO_STR(r, "operation", i->operation);
    TGEO_STR(r, "operation_id", i->operation_id);
    TGEO_STR(r, "grids", i->grids);
    i->accuracy = tapi_geo_res_dbl(r, "accuracy");
    i->best_accuracy = tapi_geo_res_dbl(r, "best_accuracy");
    i->ballpark = tapi_geo_res_int(r, "ballpark", 0) != 0;
    i->exact = tapi_geo_res_int(r, "exact", 0) != 0;
    i->fallback = tapi_geo_res_int(r, "fallback", 0) != 0;
    i->grids_missing = (unsigned int)tapi_geo_res_int(r, "grids_missing", 0);
    i->candidates = (unsigned int)tapi_geo_res_int(r, "candidates", 0);
    i->out_of_domain = (unsigned int)tapi_geo_res_int(r, "out_of_domain", 0);
    i->nonfinite = (unsigned int)tapi_geo_res_int(r, "nonfinite", 0);
}

/* See description in tapi_geo_crs.h */
void
tapi_geo_xform_info_log(const tapi_geo_xform_info *i)
{
    RING("Transformation %s -> %s (axis %s)\n"
         "operation: %s %s\n"
         "accuracy: %.3f m (best candidate %.3f m of %u)\n"
         "exact: %s, ballpark: %s, fallback: %s\n"
         "grids: '%s' (%u missing)\n"
         "out of domain: %u, non-finite: %u",
         i->src, i->dst, i->axis, i->operation, i->operation_id, i->accuracy,
         i->best_accuracy, i->candidates, i->exact ? "yes" : "no",
         i->ballpark ? "yes" : "no", i->fallback ? "yes" : "no", i->grids,
         i->grids_missing, i->out_of_domain, i->nonfinite);
}

/* See description in tapi_geo_crs.h */
te_errno
tapi_geo_transform(rcf_rpc_server *rpcs, const tapi_geo_geometry *geom,
                   const char *src, const char *dst, tapi_geo_axis axis,
                   const tapi_geo_xform_opts *opts, tapi_geo_geometry **out,
                   tapi_geo_xform_info *info)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    te_errno rc;
    int h;

    *out = NULL;
    if ((rc = tgeo_handle(rpcs, geom, &h)) != 0)
        return rc;
    xform_params(&p, src, dst, axis, opts);
    if (opts != NULL && opts->allow_partial)
        te_string_append(&p, "allow_partial=1\n");
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_TRANSFORM, h, 0, "", "", p.ptr, &r);
    info_read(&r, info);
    if (rc == 0)
    {
        rc = tgeo_wrap(rpcs, &r, out);
        if (rc == 0)
        {
            snprintf((*out)->crs, sizeof((*out)->crs), "%s",
                     info != NULL ? info->dst : dst);
            snprintf((*out)->type, sizeof((*out)->type), "%s", geom->type);
            snprintf((*out)->dims, sizeof((*out)->dims), "%s", geom->dims);
        }
    }
    te_string_free(&p);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo_crs.h */
te_errno
tapi_geo_transform_coords(rcf_rpc_server *rpcs, const char *src,
                          const char *dst, tapi_geo_axis axis,
                          const tapi_geo_xform_opts *opts, const double *in,
                          unsigned int n, unsigned int dims, double *out,
                          tapi_geo_xform_info *info)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    te_string t = TE_STRING_INIT;
    te_errno rc;
    unsigned int i;
    const char *cur;

    if (dims != 2 && dims != 3)
        return TE_RC(TE_TAPI, TE_EINVAL);
    for (i = 0; i < n; i++)
    {
        if (dims == 2)
            te_string_append(&t, "%.17g %.17g\n", in[2 * i], in[2 * i + 1]);
        else
            te_string_append(&t, "%.17g %.17g %.17g\n", in[3 * i],
                             in[3 * i + 1], in[3 * i + 2]);
    }
    xform_params(&p, src, dst, axis, opts);
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_TRANSFORM_COORDS, 0, 0, "", t.ptr,
                       p.ptr, &r);
    info_read(&r, info);
    if (rc == 0)
    {
        cur = r.report.ptr;
        for (i = 0; i < n && cur != NULL && *cur != '\0'; i++)
        {
            char *end;
            unsigned int k;

            for (k = 0; k < dims; k++)
            {
                out[dims * i + k] = strtod(cur, &end);
                cur = end;
            }
            cur = strchr(cur, '\n');
            if (cur != NULL)
                cur++;
        }
        if (i != n)
            rc = TE_RC(TE_TAPI, TE_EPROTO);
    }
    te_string_free(&p);
    te_string_free(&t);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo_crs.h */
te_errno
tapi_geo_transform_bounds(rcf_rpc_server *rpcs, const char *src,
                          const char *dst, tapi_geo_axis axis,
                          const tapi_geo_bbox *in, tapi_geo_bbox *out,
                          tapi_geo_xform_info *info)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    char t[160];
    te_errno rc;

    snprintf(t, sizeof(t), "%.17g %.17g %.17g %.17g", in->minx, in->miny,
             in->maxx, in->maxy);
    xform_params(&p, src, dst, axis, NULL);
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_TRANSFORM_BOUNDS, 0, 0, "", t,
                       p.ptr, &r);
    info_read(&r, info);
    if (rc == 0)
    {
        out->minx = tapi_geo_res_dbl(&r, "minx");
        out->miny = tapi_geo_res_dbl(&r, "miny");
        out->maxx = tapi_geo_res_dbl(&r, "maxx");
        out->maxy = tapi_geo_res_dbl(&r, "maxy");
    }
    te_string_free(&p);
    tapi_geo_rpc_res_free(&r);
    return rc;
}
