/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: OGC services
 */

#define TE_LGR_USER     "TAPI GEO OGC"

#include "te_config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "logger_api.h"

#include "tapi_geo_ogc.h"
#include "tapi_geo_internal.h"

static const char *
svc(tapi_geo_ogc_service s)
{
    return s == TAPI_GEO_OGC_WFS ? "wfs" :
           s == TAPI_GEO_OGC_OAPIF ? "oapif" : "wms";
}

static void
target_params(te_string *p, const tapi_geo_ogc_target *t)
{
    te_string_append(p, "service=%s\n", svc(t->service));
    if (t->version != NULL)
        te_string_append(p, "version=%s\n", t->version);
    if (t->user != NULL)
        te_string_append(p, "user=%s\npassword=%s\n", t->user,
                         t->password != NULL ? t->password : "");
    if (t->timeout_ms != 0)
        te_string_append(p, "http_timeout_ms=%u\n", t->timeout_ms);
}

void
tapi_geo_ogc_result_free(tapi_geo_ogc_result *res)
{
    te_string_free(&res->records);
    tapi_geo_rpc_res_free(&res->raw);
}

bool
tapi_geo_ogc_has_issue(const tapi_geo_ogc_result *res, const char *category)
{
    char needle[96];

    snprintf(needle, sizeof(needle), "issue\t%s\t", category);
    return res->records.ptr != NULL && strstr(res->records.ptr, needle) != NULL;
}

static te_errno
check(rcf_rpc_server *rpcs, const char *kind, const tapi_geo_ogc_target *t,
      const char *extra, tapi_geo_ogc_result *res)
{
    te_string p = TE_STRING_INIT;
    te_errno rc;

    memset(res, 0, sizeof(*res));
    res->records = (te_string)TE_STRING_INIT;
    res->raw = (tapi_geo_res)TAPI_GEO_RES_INIT;
    target_params(&p, t);
    if (extra != NULL)
        te_string_append(&p, "%s\n", extra);
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_OGC, 0, 0, kind, t->url, p.ptr,
                       &res->raw);
    te_string_free(&p);
    if (rc == 0)
    {
        res->ok = tapi_geo_res_int(&res->raw, "ok", 0) != 0;
        res->issues = tapi_geo_res_int(&res->raw, "issues", 0);
        res->http_status = (long)tapi_geo_res_int(&res->raw, "http_status",
                                                  0);
        TGEO_STR(&res->raw, "content_type", res->content_type);
        if (res->raw.report.ptr != NULL)
            te_string_append(&res->records, "%s", res->raw.report.ptr);
        if (res->issues > 0)
            RING("OGC %s check of %s: %lld issue(s)\n%s", kind, t->url,
                 res->issues, res->records.ptr);
    }
    return rc;
}

te_errno
tapi_geo_ogc_capabilities(rcf_rpc_server *rpcs, const tapi_geo_ogc_target *t,
                          const char *expect_layer, const char *expect_crs,
                          tapi_geo_ogc_result *res)
{
    te_string p = TE_STRING_INIT;
    te_errno rc;

    if (expect_layer != NULL)
        te_string_append(&p, "expect_layer=%s\n", expect_layer);
    if (expect_crs != NULL)
        te_string_append(&p, "expect_crs=%s\n", expect_crs);
    rc = check(rpcs, "capabilities", t, p.ptr, res);
    te_string_free(&p);
    return rc;
}

te_errno
tapi_geo_ogc_map(rcf_rpc_server *rpcs, const tapi_geo_ogc_target *t,
                 const char *layer, const char *crs, const char *bbox,
                 unsigned int width, unsigned int height,
                 tapi_geo_ogc_result *res)
{
    te_string p = TE_STRING_INIT;
    te_errno rc;

    te_string_append(&p, "layer=%s\ncrs=%s\nbbox=%s\nwidth=%u\nheight=%u\n",
                     layer, crs, bbox, width, height);
    rc = check(rpcs, "map", t, p.ptr, res);
    te_string_free(&p);
    return rc;
}

static void
feat_read(const tapi_geo_res *r, const char *pfx, tapi_geo_ogc_features *f)
{
    char k[64];

#define K(_n) (snprintf(k, sizeof(k), "%s%s", pfx, _n), k)
    f->features = tapi_geo_res_int(r, K("features"), 0);
    f->invalid = tapi_geo_res_int(r, K("invalid"), 0);
    f->empty = tapi_geo_res_int(r, K("empty"), 0);
    f->missing_geometry = tapi_geo_res_int(r, K("missing_geometry"), 0);
    f->duplicates = tapi_geo_res_int(r, K("duplicates"), 0);
    f->outside_extent = tapi_geo_res_int(r, K("outside_extent"), 0);
    f->elapsed_us = tapi_geo_res_dbl(r, K("elapsed_us"));
    if (tapi_geo_res_str(r, K("digest"), f->digest, sizeof(f->digest)) ==
        NULL)
        f->digest[0] = '\0';
#undef K
    f->ok = f->invalid + f->duplicates + f->outside_extent +
            f->missing_geometry == 0;
}

te_errno
tapi_geo_ogc_features_get(rcf_rpc_server *rpcs, const tapi_geo_ogc_target *t,
                          const char *layer, unsigned int page_size,
                          unsigned int max_features,
                          tapi_geo_ogc_features *f)
{
    tapi_geo_ogc_result res;
    te_string p = TE_STRING_INIT;
    te_errno rc;

    memset(f, 0, sizeof(*f));
    if (layer != NULL)
        te_string_append(&p, "layer=%s\n", layer);
    te_string_append(&p, "page_size=%u\nmax_features=%u\n", page_size,
                     max_features != 0 ? max_features : 100000);
    rc = check(rpcs, "features", t, p.ptr, &res);
    te_string_free(&p);
    if (rc == 0)
    {
        feat_read(&res.raw, "", f);
        TGEO_STR(&res.raw, "crs", f->crs);
        f->extent.minx = tapi_geo_res_dbl(&res.raw, "minx");
        f->extent.miny = tapi_geo_res_dbl(&res.raw, "miny");
        f->extent.maxx = tapi_geo_res_dbl(&res.raw, "maxx");
        f->extent.maxy = tapi_geo_res_dbl(&res.raw, "maxy");
    }
    tapi_geo_ogc_result_free(&res);
    return rc;
}

te_errno
tapi_geo_ogc_paging(rcf_rpc_server *rpcs, const tapi_geo_ogc_target *t,
                    const char *layer, unsigned int page_a,
                    unsigned int page_b, bool *consistent,
                    tapi_geo_ogc_features *a, tapi_geo_ogc_features *b)
{
    tapi_geo_ogc_result res;
    te_string p = TE_STRING_INIT;
    te_errno rc;

    if (layer != NULL)
        te_string_append(&p, "layer=%s\n", layer);
    te_string_append(&p, "page_sizes=%u,%u\n", page_a, page_b);
    rc = check(rpcs, "paging", t, p.ptr, &res);
    te_string_free(&p);
    if (rc == 0)
    {
        *consistent = tapi_geo_res_int(&res.raw, "consistent", 0) != 0;
        if (a != NULL)
            feat_read(&res.raw, "a_", a);
        if (b != NULL)
            feat_read(&res.raw, "b_", b);
    }
    tapi_geo_ogc_result_free(&res);
    return rc;
}

te_errno
tapi_geo_ogc_access(rcf_rpc_server *rpcs, const char *url, const char *user,
                    const char *password, tapi_geo_ogc_auth *auth)
{
    tapi_geo_ogc_target t = { TAPI_GEO_OGC_WMS, url, NULL, user, password,
                              0 };
    tapi_geo_ogc_result res;
    te_errno rc = check(rpcs, "auth", &t, NULL, &res);

    if (rc == 0)
    {
        auth->status_anonymous =
            (long)tapi_geo_res_int(&res.raw, "status_anonymous", 0);
        auth->status_wrong_password =
            (long)tapi_geo_res_int(&res.raw, "status_wrong_password", 0);
        auth->status_authenticated =
            (long)tapi_geo_res_int(&res.raw, "status_authenticated", 0);
        auth->enforced = tapi_geo_res_int(&res.raw, "enforced", 0) != 0;
    }
    tapi_geo_ogc_result_free(&res);
    return rc;
}

te_errno
tapi_geo_ogc_conformance(rcf_rpc_server *rpcs, const tapi_geo_ogc_target *t,
                         bool *core, bool *geojson, tapi_geo_ogc_result *res)
{
    te_errno rc = check(rpcs, "conformance", t, NULL, res);

    if (rc == 0)
    {
        if (core != NULL)
            *core = tapi_geo_res_int(&res->raw, "conforms_core", 0) != 0;
        if (geojson != NULL)
            *geojson = tapi_geo_res_int(&res->raw, "conforms_geojson", 0) !=
                       0;
    }
    return rc;
}
