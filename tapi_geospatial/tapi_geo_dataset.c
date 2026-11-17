/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: vector datasets and rasters
 */

#define TE_LGR_USER     "TAPI GEO DS"

#include "te_config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "logger_api.h"

#include "tapi_geo_dataset.h"
#include "tapi_geo_raster.h"
#include "tapi_geo_internal.h"

static te_errno
ds_handle(rcf_rpc_server *rpcs, const tapi_geo_dataset *ds, int *h)
{
    if (ds == NULL)
    {
        *h = 0;
        return 0;
    }
    if (ds->rpcs != rpcs)
    {
        ERROR("Dataset %d belongs to RPC server %s", ds->handle,
              ds->rpcs != NULL ? ds->rpcs->name : "?");
        return TE_RC(TE_TAPI, TE_EINVAL);
    }
    *h = ds->handle;
    return 0;
}

static void
layer_opt(te_string *p, const char *key, const char *layer)
{
    if (layer != NULL && *layer != '\0')
        te_string_append(p, "%s=%s\n", key, layer);
}

static te_errno
open_any(rcf_rpc_server *rpcs, const char *path, const char *params,
         bool raster, tapi_geo_dataset **out)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    tapi_geo_dataset *d;
    te_errno rc;

    *out = NULL;
    te_string_append(&p, "mode=%s\n%s\n", raster ? "raster" : "vector",
                     params != NULL ? params : "");
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_DS_OPEN, 0, 0, "", path, p.ptr, &r);
    te_string_free(&p);
    if (rc == 0)
    {
        d = calloc(1, sizeof(*d));
        if (d == NULL)
            rc = TE_RC(TE_TAPI, TE_ENOMEM);
        else
        {
            d->rpcs = rpcs;
            d->handle = r.handle;
            d->raster = raster;
            TGEO_STR(&r, "driver", d->driver);
            d->layers = (unsigned int)tapi_geo_res_int(&r, "layers", 0);
            d->bands = (unsigned int)tapi_geo_res_int(&r, "bands", 0);
            d->open_us = tapi_geo_res_dbl(&r, "elapsed_us");
            *out = d;
        }
    }
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo_dataset.h */
te_errno
tapi_geo_dataset_open(rcf_rpc_server *rpcs, const char *path,
                      const char *params, tapi_geo_dataset **ds)
{
    return open_any(rpcs, path, params, false, ds);
}

/* See description in tapi_geo_dataset.h */
te_errno
tapi_geo_dataset_close(rcf_rpc_server *rpcs, tapi_geo_dataset *ds)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc;
    int h;

    if (ds == NULL)
        return 0;
    rc = ds_handle(rpcs, ds, &h);
    if (rc == 0)
        rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_RELEASE, h, 0, "", "", "", &r);
    tapi_geo_rpc_res_free(&r);
    free(ds);
    return rc;
}

static te_errno
ds_info(rcf_rpc_server *rpcs, const tapi_geo_dataset *ds, const char *kind,
        const char *params, tapi_geo_res *r)
{
    te_errno rc;
    int h;

    if ((rc = ds_handle(rpcs, ds, &h)) != 0)
        return rc;
    return tapi_geo_call(rpcs, TAPI_GEO_RPC_DS_INFO, h, 0, kind, "", params,
                         r);
}

/* See description in tapi_geo_dataset.h */
te_errno
tapi_geo_dataset_layers(rcf_rpc_server *rpcs, const tapi_geo_dataset *ds,
                        tapi_geo_layer **layers, unsigned int *n,
                        bool *crs_consistent)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc = ds_info(rpcs, ds, "layers", "", &r);
    char *cur;
    char *f[8];
    unsigned int cap = 0;

    *layers = NULL;
    *n = 0;
    if (rc == 0)
    {
        if (crs_consistent != NULL)
            *crs_consistent = tapi_geo_res_int(&r, "crs_consistent", 1) != 0;
        cur = r.report.ptr;
        while (tapi_geo_report_next(&cur, f, 8) >= 5)
        {
            tapi_geo_layer *l;

            if (*n == cap)
            {
                tapi_geo_layer *nl;

                cap = cap == 0 ? 8 : cap * 2;
                nl = realloc(*layers, cap * sizeof(*nl));
                if (nl == NULL)
                {
                    rc = TE_RC(TE_TAPI, TE_ENOMEM);
                    break;
                }
                *layers = nl;
            }
            l = &(*layers)[(*n)++];
            snprintf(l->name, sizeof(l->name), "%s", f[1]);
            snprintf(l->geometry_type, sizeof(l->geometry_type), "%s", f[2]);
            snprintf(l->crs, sizeof(l->crs), "%s", f[3]);
            l->count = strtoll(f[4], NULL, 10);
        }
    }
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo_dataset.h */
te_errno
tapi_geo_dataset_schema(rcf_rpc_server *rpcs, const tapi_geo_dataset *ds,
                        const char *layer, tapi_geo_field **fields,
                        unsigned int *n, char *gt, size_t gt_len)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    te_errno rc;
    char *cur;
    char *f[8];
    unsigned int cap = 0;

    *fields = NULL;
    *n = 0;
    layer_opt(&p, "layer", layer);
    rc = ds_info(rpcs, ds, "schema", p.ptr, &r);
    te_string_free(&p);
    if (rc == 0)
    {
        if (gt != NULL && tapi_geo_res_str(&r, "geometry_type", gt,
                                           gt_len) == NULL)
            gt[0] = '\0';
        cur = r.report.ptr;
        while (tapi_geo_report_next(&cur, f, 8) >= 5)
        {
            tapi_geo_field *fd;

            if (*n == cap)
            {
                tapi_geo_field *nf;

                cap = cap == 0 ? 8 : cap * 2;
                nf = realloc(*fields, cap * sizeof(*nf));
                if (nf == NULL)
                {
                    rc = TE_RC(TE_TAPI, TE_ENOMEM);
                    break;
                }
                *fields = nf;
            }
            fd = &(*fields)[(*n)++];
            snprintf(fd->name, sizeof(fd->name), "%s", f[0]);
            snprintf(fd->type, sizeof(fd->type), "%s", f[1]);
            fd->width = atoi(f[2]);
            fd->precision = atoi(f[3]);
            fd->nullable = atoi(f[4]) != 0;
        }
    }
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo_dataset.h */
te_errno
tapi_geo_dataset_count(rcf_rpc_server *rpcs, const tapi_geo_dataset *ds,
                       const char *layer, bool exact, long long *count)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    te_errno rc;

    layer_opt(&p, "layer", layer);
    te_string_append(&p, "exact=%d\n", exact);
    rc = ds_info(rpcs, ds, "count", p.ptr, &r);
    if (rc == 0)
        *count = tapi_geo_res_int(&r, "count", -1);
    te_string_free(&p);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo_dataset.h */
te_errno
tapi_geo_dataset_extent(rcf_rpc_server *rpcs, const tapi_geo_dataset *ds,
                        const char *layer, tapi_geo_bbox *e)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    te_errno rc;

    layer_opt(&p, "layer", layer);
    rc = ds_info(rpcs, ds, "extent", p.ptr, &r);
    if (rc == 0)
    {
        e->minx = tapi_geo_res_dbl(&r, "minx");
        e->miny = tapi_geo_res_dbl(&r, "miny");
        e->maxx = tapi_geo_res_dbl(&r, "maxx");
        e->maxy = tapi_geo_res_dbl(&r, "maxy");
    }
    te_string_free(&p);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo_dataset.h */
te_errno
tapi_geo_dataset_fetch(rcf_rpc_server *rpcs, const tapi_geo_dataset *ds,
                       const char *layer, long long offset, long long limit,
                       const char *params, te_string *out,
                       unsigned int *returned)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    te_errno rc;
    int h;

    if ((rc = ds_handle(rpcs, ds, &h)) != 0)
        return rc;
    layer_opt(&p, "layer", layer);
    te_string_append(&p, "offset=%lld\nlimit=%lld\n%s\n", offset, limit,
                     params != NULL ? params : "");
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_DS_FETCH, h, 0, "features", "",
                       p.ptr, &r);
    if (rc == 0)
    {
        if (out != NULL && r.report.ptr != NULL)
            te_string_append(out, "%s", r.report.ptr);
        if (returned != NULL)
            *returned = (unsigned int)tapi_geo_res_int(&r, "returned", 0);
    }
    te_string_free(&p);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo_dataset.h */
te_errno
tapi_geo_dataset_geometry(rcf_rpc_server *rpcs, const tapi_geo_dataset *ds,
                          const char *layer, long long fid,
                          tapi_geo_geometry **geom)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    te_errno rc;
    int h;

    *geom = NULL;
    if ((rc = ds_handle(rpcs, ds, &h)) != 0)
        return rc;
    layer_opt(&p, "layer", layer);
    te_string_append(&p, "fid=%lld\n", fid);
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_DS_FETCH, h, 0, "geometry", "",
                       p.ptr, &r);
    if (rc == 0)
        rc = tgeo_wrap(rpcs, &r, geom);
    te_string_free(&p);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo_dataset.h */
te_errno
tapi_geo_dataset_validate(rcf_rpc_server *rpcs, const tapi_geo_dataset *ds,
                          const tapi_geo_ds_rules *ru,
                          tapi_geo_ds_report *rep)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    te_errno rc;
    int h;

    memset(rep, 0, sizeof(*rep));
    rep->records = (te_string)TE_STRING_INIT;
    if ((rc = ds_handle(rpcs, ds, &h)) != 0)
        return rc;
#define OPT(_k, _v) \
    do {                                                        \
        if ((_v) != NULL && *(_v) != '\0')                      \
            te_string_append(&p, "%s=%s\n", (_k), (_v));        \
    } while (0)
    OPT("layer", ru->layer);
    OPT("id_field", ru->id_field);
    OPT("expect_types", ru->expect_types);
    OPT("expect_crs", ru->expect_crs);
    OPT("bounds", ru->bounds);
    OPT("schema", ru->schema);
    OPT("required", ru->required);
#undef OPT
    if (ru->strict_schema)
        te_string_append(&p, "strict_schema=1\n");
    if (ru->extra != NULL)
        te_string_append(&p, "%s\n", ru->extra);
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_DS_VALIDATE, h, 0, "", "", p.ptr,
                       &r);
    rep->features = tapi_geo_res_int(&r, "features", 0);
    rep->bad_features = tapi_geo_res_int(&r, "bad_features", 0);
    rep->issues = tapi_geo_res_int(&r, "issues", 0);
    rep->ok = rc == 0 && tapi_geo_res_int(&r, "ok", 0) != 0;
    TGEO_STR(&r, "crs", rep->crs);
#define CNT(_f) rep->_f = tapi_geo_res_int(&r, #_f, 0)
    CNT(invalid_geometry);
    CNT(missing_geometry);
    CNT(empty_geometry);
    CNT(duplicate_id);
    CNT(crs_mismatch);
    CNT(out_of_range_coordinate);
    CNT(schema_mismatch);
    CNT(unexpected_geometry_type);
    CNT(invalid_attribute);
    CNT(non_finite);
    CNT(missing_id);
#undef CNT
    if (r.report.ptr != NULL)
        te_string_append(&rep->records, "%s", r.report.ptr);
    te_string_free(&p);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

void
tapi_geo_ds_report_free(tapi_geo_ds_report *rep)
{
    te_string_free(&rep->records);
}

/* See description in tapi_geo_dataset.h */
te_errno
tapi_geo_dataset_compare(rcf_rpc_server *rpcs, const tapi_geo_dataset *a,
                         const tapi_geo_dataset *b,
                         const tapi_geo_ds_cmp_opts *o,
                         tapi_geo_ds_diff *d)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    tapi_geo_ds_cmp_opts def = TAPI_GEO_DS_CMP_OPTS_DEFAULT;
    te_errno rc;
    int ha, hb;

    memset(d, 0, sizeof(*d));
    d->records = (te_string)TE_STRING_INIT;
    if (o == NULL)
        o = &def;
    if ((rc = ds_handle(rpcs, a, &ha)) != 0 ||
        (rc = ds_handle(rpcs, b, &hb)) != 0)
        return rc;
    layer_opt(&p, "layer_a", o->layer_a);
    layer_opt(&p, "layer_b", o->layer_b);
    layer_opt(&p, "id_field", o->id_field);
    te_string_append(&p, "geometry=%s\ntolerance=%.17g\nfloat_tolerance="
                     "%.17g\nattributes=%s\n",
                     o->geometry != NULL ? o->geometry : "topological",
                     o->tolerance, o->float_tolerance,
                     o->attributes != NULL ? o->attributes : "all");
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_DS_COMPARE, ha, hb, "", "", p.ptr,
                       &r);
    if (rc == 0)
    {
        d->crs_compatible = tapi_geo_res_int(&r, "crs_compatible", 0) != 0;
        d->equal = tapi_geo_res_int(&r, "equal", 0) != 0;
        d->count_a = tapi_geo_res_int(&r, "count_a", 0);
        d->count_b = tapi_geo_res_int(&r, "count_b", 0);
        d->matched = tapi_geo_res_int(&r, "matched", 0);
        d->only_a = tapi_geo_res_int(&r, "only_a", 0);
        d->only_b = tapi_geo_res_int(&r, "only_b", 0);
        d->geometry_diff = tapi_geo_res_int(&r, "geometry_diff", 0);
        d->attribute_diff = tapi_geo_res_int(&r, "attribute_diff", 0);
        d->duplicate_id_a = tapi_geo_res_int(&r, "duplicate_id_a", 0);
        d->duplicate_id_b = tapi_geo_res_int(&r, "duplicate_id_b", 0);
        d->max_hausdorff = tapi_geo_res_dbl(&r, "max_hausdorff");
        if (r.report.ptr != NULL)
            te_string_append(&d->records, "%s", r.report.ptr);
    }
    te_string_free(&p);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

void
tapi_geo_ds_diff_free(tapi_geo_ds_diff *d)
{
    te_string_free(&d->records);
}

/* See description in tapi_geo_dataset.h */
te_errno
tapi_geo_dataset_generate(rcf_rpc_server *rpcs, const char *path,
                          const char *driver, const char *crs,
                          const char *gen, const char *gen_params,
                          const char *defects, long long *written,
                          tapi_geo_planted **planted, unsigned int *n_planted)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    te_errno rc;

    if (planted != NULL)
        *planted = NULL;
    if (n_planted != NULL)
        *n_planted = 0;
    te_string_append(&p, "driver=%s\ngen=%s\n", driver, gen);
    if (crs != NULL && *crs != '\0')
        te_string_append(&p, "crs=%s\n", crs);
    if (defects != NULL && *defects != '\0')
        te_string_append(&p, "defects=%s\n", defects);
    if (gen_params != NULL)
        te_string_append(&p, "%s\n", gen_params);
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_DS_WRITE, 0, 0, "generate", path,
                       p.ptr, &r);
    if (rc == 0)
    {
        if (written != NULL)
            *written = tapi_geo_res_int(&r, "written", 0);
        if (planted != NULL && n_planted != NULL)
        {
            char *cur = r.report.ptr;
            char *f[4];
            unsigned int cap = 0;

            while (tapi_geo_report_next(&cur, f, 4) == 3)
            {
                if (*n_planted == cap)
                {
                    tapi_geo_planted *np;

                    cap = cap == 0 ? 16 : cap * 2;
                    np = realloc(*planted, cap * sizeof(*np));
                    if (np == NULL)
                    {
                        rc = TE_RC(TE_TAPI, TE_ENOMEM);
                        break;
                    }
                    *planted = np;
                }
                (*planted)[*n_planted].position = strtoll(f[0], NULL, 10);
                (*planted)[*n_planted].id = strtoll(f[1], NULL, 10);
                snprintf((*planted)[*n_planted].defect,
                         sizeof((*planted)[*n_planted].defect), "%s", f[2]);
                (*n_planted)++;
            }
        }
    }
    te_string_free(&p);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo_dataset.h */
te_errno
tapi_geo_dataset_copy(rcf_rpc_server *rpcs, const tapi_geo_dataset *src,
                      const char *path, const char *driver,
                      const char *dst_crs)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    te_errno rc;
    int h;

    if ((rc = ds_handle(rpcs, src, &h)) != 0)
        return rc;
    te_string_append(&p, "driver=%s\n", driver);
    if (dst_crs != NULL)
        te_string_append(&p, "dst=%s\n", dst_crs);
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_DS_WRITE, h, 0, "copy", path,
                       p.ptr, &r);
    te_string_free(&p);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* ---- rasters ------------------------------------------------------ */

te_errno
tapi_geo_raster_open(rcf_rpc_server *rpcs, const char *path,
                     const char *params, tapi_geo_raster **r)
{
    return open_any(rpcs, path, params, true, r);
}

te_errno
tapi_geo_raster_close(rcf_rpc_server *rpcs, tapi_geo_raster *r)
{
    return tapi_geo_dataset_close(rpcs, r);
}

/* See description in tapi_geo_raster.h */
te_errno
tapi_geo_raster_info_get(rcf_rpc_server *rpcs, const tapi_geo_raster *ras,
                         tapi_geo_raster_info *i)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc;
    int h;
    char gt[512];

    if ((rc = ds_handle(rpcs, ras, &h)) != 0)
        return rc;
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_RASTER_INFO, h, 0, "", "", "", &r);
    if (rc == 0)
    {
        memset(i, 0, sizeof(*i));
        TGEO_STR(&r, "driver", i->driver);
        TGEO_STR(&r, "type", i->type);
        TGEO_STR(&r, "crs", i->crs);
        TGEO_STR(&r, "compression", i->compression);
        i->width = (int)tapi_geo_res_int(&r, "width", 0);
        i->height = (int)tapi_geo_res_int(&r, "height", 0);
        i->bands = (int)tapi_geo_res_int(&r, "bands", 0);
        i->has_nodata = tapi_geo_res_int(&r, "has_nodata", 0) != 0;
        i->nodata = tapi_geo_res_dbl(&r, "nodata");
        i->georeferenced = tapi_geo_res_int(&r, "georeferenced", 0) != 0;
        i->res_x = tapi_geo_res_dbl(&r, "res_x");
        i->res_y = tapi_geo_res_dbl(&r, "res_y");
        i->rotated = tapi_geo_res_int(&r, "rotated", 0) != 0;
        i->extent.minx = tapi_geo_res_dbl(&r, "minx");
        i->extent.miny = tapi_geo_res_dbl(&r, "miny");
        i->extent.maxx = tapi_geo_res_dbl(&r, "maxx");
        i->extent.maxy = tapi_geo_res_dbl(&r, "maxy");
        i->block_x = (int)tapi_geo_res_int(&r, "block_x", 0);
        i->block_y = (int)tapi_geo_res_int(&r, "block_y", 0);
        i->overviews = (int)tapi_geo_res_int(&r, "overviews", 0);
        if (tapi_geo_res_str(&r, "geotransform", gt, sizeof(gt)) != NULL)
            sscanf(gt, "%lf %lf %lf %lf %lf %lf", &i->geotransform[0],
                   &i->geotransform[1], &i->geotransform[2],
                   &i->geotransform[3], &i->geotransform[4],
                   &i->geotransform[5]);
    }
    tapi_geo_rpc_res_free(&r);
    return rc;
}

static void
stats_read(const tapi_geo_res *r, tapi_geo_raster_stats *st)
{
    st->pixels = tapi_geo_res_int(r, "pixels", 0);
    st->valid = tapi_geo_res_int(r, "valid", 0);
    st->nodata = tapi_geo_res_int(r, "nodata", 0);
    st->nonfinite = tapi_geo_res_int(r, "nonfinite", 0);
    st->min = tapi_geo_res_dbl(r, "min");
    st->max = tapi_geo_res_dbl(r, "max");
    st->mean = tapi_geo_res_dbl(r, "mean");
    st->stddev = tapi_geo_res_dbl(r, "stddev");
    st->elapsed_us = tapi_geo_res_dbl(r, "elapsed_us");
    st->mpix_per_s = tapi_geo_res_dbl(r, "mpix_per_s");
}

/* See description in tapi_geo_raster.h */
te_errno
tapi_geo_raster_statistics(rcf_rpc_server *rpcs, const tapi_geo_raster *ras,
                           int band, int tile, tapi_geo_raster_stats *st)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    char p[64];
    te_errno rc;
    int h;

    if ((rc = ds_handle(rpcs, ras, &h)) != 0)
        return rc;
    snprintf(p, sizeof(p), "band=%d\ntile=%d", band, tile);
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_RASTER_STATS, h, 0, "", "", p, &r);
    if (rc == 0)
        stats_read(&r, st);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo_raster.h */
te_errno
tapi_geo_raster_read(rcf_rpc_server *rpcs, const tapi_geo_raster *ras,
                     int band, int x, int y, int w, int hgt, double *values,
                     tapi_geo_raster_stats *st)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    char p[128];
    te_errno rc;
    int h;

    if ((rc = ds_handle(rpcs, ras, &h)) != 0)
        return rc;
    snprintf(p, sizeof(p), "band=%d\nx=%d\ny=%d\nwidth=%d\nheight=%d", band,
             x, y, w, hgt);
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_RASTER_READ, h, 0, "", "", p, &r);
    if (rc == 0)
    {
        if (st != NULL)
        {
            memset(st, 0, sizeof(*st));
            stats_read(&r, st);
        }
        if (values != NULL)
        {
            const char *cur = r.report.ptr != NULL ? r.report.ptr : "";
            long long k;
            char *end;

            for (k = 0; k < (long long)w * hgt; k++)
            {
                values[k] = strtod(cur, &end);
                if (end == cur)
                {
                    rc = TE_RC(TE_TAPI, TE_EPROTO);
                    break;
                }
                cur = end;
            }
        }
    }
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo_raster.h */
te_errno
tapi_geo_raster_compare(rcf_rpc_server *rpcs, const tapi_geo_raster *a,
                        const tapi_geo_raster *b, double tolerance, int tile,
                        tapi_geo_raster_diff *d)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    char p[96];
    te_errno rc;
    int ha, hb;

    memset(d, 0, sizeof(*d));
    d->records = (te_string)TE_STRING_INIT;
    if ((rc = ds_handle(rpcs, a, &ha)) != 0 ||
        (rc = ds_handle(rpcs, b, &hb)) != 0)
        return rc;
    snprintf(p, sizeof(p), "tolerance=%.17g\ntile=%d", tolerance, tile);
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_RASTER_COMPARE, ha, hb, "", "", p,
                       &r);
    if (rc == 0)
    {
        d->equal = tapi_geo_res_int(&r, "equal", 0) != 0;
        d->metadata_diff = tapi_geo_res_int(&r, "metadata_diff", 0);
        d->compared = tapi_geo_res_int(&r, "compared", 0);
        d->pixel_diff = tapi_geo_res_int(&r, "pixel_diff", 0);
        d->nodata_mismatch = tapi_geo_res_int(&r, "nodata_mismatch", 0);
        d->max_abs_diff = tapi_geo_res_dbl(&r, "max_abs_diff");
        d->mean_diff = tapi_geo_res_dbl(&r, "mean_diff");
        d->rmse = tapi_geo_res_dbl(&r, "rmse");
        if (r.report.ptr != NULL)
            te_string_append(&d->records, "%s", r.report.ptr);
    }
    tapi_geo_rpc_res_free(&r);
    return rc;
}

void
tapi_geo_raster_diff_free(tapi_geo_raster_diff *d)
{
    te_string_free(&d->records);
}

/* See description in tapi_geo_raster.h */
te_errno
tapi_geo_raster_write(rcf_rpc_server *rpcs, const char *path,
                      const char *params)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_RASTER_WRITE, 0, 0, "",
                                path, params, &r);

    tapi_geo_rpc_res_free(&r);
    return rc;
}
