/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: geometries, predicates, measurements
 */

#define TE_LGR_USER     "TAPI GEO"

#include "te_config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "logger_api.h"

#include "tapi_geo.h"
#include "tapi_geo_internal.h"

/* ---- generic ------------------------------------------------------ */

static char last_error[1024];

const char *
tapi_geo_last_error(void)
{
    return last_error;
}

/* See description in tapi_geo.h */
te_errno
tapi_geo_call(rcf_rpc_server *rpcs, tapi_geo_rpc_op op, int ha, int hb,
              const char *kind, const char *text, const char *params,
              tapi_geo_res *res)
{
    tapi_geo_rpc_args a = { ha, hb, kind, text, params };
    te_errno rc;

    rc = rpc_geo_call(rpcs, op, &a, 0, res);
    if (rc != 0)
    {
        snprintf(last_error, sizeof(last_error), "%s",
                 res->errmsg.ptr != NULL ? res->errmsg.ptr : "");
        if (TE_RC_GET_ERROR(rc) == TE_ECORRUPTED && last_error[0] == '\0')
            snprintf(last_error, sizeof(last_error), "%s: RPC failed",
                     tapi_geo_rpc_name(op));
    }
    else
        last_error[0] = '\0';
    return rc;
}

/* See description in tapi_geo.h */
const char *
tapi_geo_res_str(const tapi_geo_res *res, const char *key, char *buf,
                 size_t len)
{
    size_t kl = strlen(key);
    const char *p = res->result.ptr;

    while (p != NULL && *p != '\0')
    {
        const char *eol = strchr(p, '\n');
        size_t l = eol != NULL ? (size_t)(eol - p) : strlen(p);

        if (l > kl && strncmp(p, key, kl) == 0 && p[kl] == '=')
        {
            size_t vl = l - kl - 1;

            if (vl >= len)
                vl = len - 1;
            memcpy(buf, p + kl + 1, vl);
            buf[vl] = '\0';
            return buf;
        }
        p = eol != NULL ? eol + 1 : NULL;
    }
    return NULL;
}

/* See description in tapi_geo.h */
double
tapi_geo_res_dbl(const tapi_geo_res *res, const char *key)
{
    char buf[64];

    if (tapi_geo_res_str(res, key, buf, sizeof(buf)) == NULL)
        return NAN;
    return strtod(buf, NULL);
}

/* See description in tapi_geo.h */
long long
tapi_geo_res_int(const tapi_geo_res *res, const char *key, long long def)
{
    char buf[64];

    if (tapi_geo_res_str(res, key, buf, sizeof(buf)) == NULL)
        return def;
    return strtoll(buf, NULL, 0);
}

static int
hexv(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static void
unescape(char *s)
{
    char *w = s;

    for (; *s != '\0'; s++)
    {
        if (*s == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0)
        {
            *w++ = (char)(hexv(s[1]) << 4 | hexv(s[2]));
            s += 2;
        }
        else
            *w++ = *s;
    }
    *w = '\0';
}

/* See description in tapi_geo.h */
unsigned int
tapi_geo_report_next(char **cursor, char **fields, unsigned int max)
{
    char *line = *cursor;
    char *eol;
    unsigned int n = 0;

    if (line == NULL || *line == '\0')
        return 0;
    eol = strchr(line, '\n');
    if (eol != NULL)
    {
        *eol = '\0';
        *cursor = eol + 1;
    }
    else
        *cursor = line + strlen(line);
    while (n < max)
    {
        char *tab = strchr(line, '\t');

        if (tab != NULL)
            *tab = '\0';
        unescape(line);
        fields[n++] = line;
        if (tab == NULL)
            break;
        line = tab + 1;
    }
    return n;
}

/* ---- engine ------------------------------------------------------- */

/* See description in tapi_geo.h */
te_errno
tapi_geo_available(rcf_rpc_server *rpcs, tapi_geo_engine *engine)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_AVAILABLE, 0, 0, "", "",
                                "", &r);

    if (rc == 0 && engine != NULL)
    {
        memset(engine, 0, sizeof(*engine));
        engine->features = r.features;
        TGEO_STR(&r, "geos", engine->geos);
        TGEO_STR(&r, "proj", engine->proj);
        TGEO_STR(&r, "gdal", engine->gdal);
        engine->drivers = (unsigned int)tapi_geo_res_int(&r, "drivers", 0);
    }
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo.h */
bool
tapi_geo_driver_supported(rcf_rpc_server *rpcs, const char *driver,
                          bool create)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    char *cur;
    char *f[8];
    bool found = false;

    if (tapi_geo_call(rpcs, TAPI_GEO_RPC_AVAILABLE, 0, 0, "", "", "",
                      &r) != 0)
    {
        tapi_geo_rpc_res_free(&r);
        return false;
    }
    cur = r.report.ptr;
    while (!found && tapi_geo_report_next(&cur, f, 8) == 6)
    {
        if (strcmp(f[0], driver) == 0)
            found = !create || strcmp(f[4], "1") == 0 ||
                    strcmp(f[5], "1") == 0;
    }
    tapi_geo_rpc_res_free(&r);
    return found;
}

/* See description in tapi_geo.h */
te_errno
tapi_geo_config(rcf_rpc_server *rpcs, const char *params)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_CONFIG, 0, 0, "", "",
                                params, &r);

    if (rc == 0)
        RING("Geospatial engine limits:\n%s", r.result.ptr);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo.h */
te_errno
tapi_geo_stats_get(rcf_rpc_server *rpcs, tapi_geo_stats *s)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_STATS, 0, 0, "", "", "",
                                &r);

    if (rc == 0)
    {
        s->session = (unsigned int)tapi_geo_res_int(&r, "session", 0);
        s->live = (unsigned int)tapi_geo_res_int(&r, "live", 0);
        s->geometries = (unsigned int)tapi_geo_res_int(&r, "geometries", 0);
        s->datasets = (unsigned int)tapi_geo_res_int(&r, "datasets", 0);
        s->tracks = (unsigned int)tapi_geo_res_int(&r, "tracks", 0);
        s->created = (uint64_t)tapi_geo_res_int(&r, "created", 0);
        s->released = (uint64_t)tapi_geo_res_int(&r, "released", 0);
        s->ops = (uint64_t)tapi_geo_res_int(&r, "ops", 0);
        s->rss_kb = (long)tapi_geo_res_int(&r, "rss_kb", 0);
        s->maxrss_kb = (long)tapi_geo_res_int(&r, "maxrss_kb", 0);
    }
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* ---- geometry handles --------------------------------------------- */

te_errno
tgeo_wrap(rcf_rpc_server *rpcs, const tapi_geo_res *res,
          tapi_geo_geometry **out)
{
    tapi_geo_geometry *g = calloc(1, sizeof(*g));

    if (g == NULL)
        return TE_RC(TE_TAPI, TE_ENOMEM);
    g->rpcs = rpcs;
    g->handle = res->handle;
    TGEO_STR(res, "crs", g->crs);
    TGEO_STR(res, "type", g->type);
    TGEO_STR(res, "dims", g->dims);
    *out = g;
    return 0;
}

te_errno
tgeo_handle(rcf_rpc_server *rpcs, const tapi_geo_geometry *g, int *h)
{
    if (g == NULL)
    {
        *h = 0;
        return 0;
    }
    if (g->rpcs != rpcs)
    {
        ERROR("Geometry %d belongs to RPC server %s, used with %s",
              g->handle, g->rpcs != NULL ? g->rpcs->name : "?",
              rpcs != NULL ? rpcs->name : "?");
        return TE_RC(TE_TAPI, TE_EINVAL);
    }
    *h = g->handle;
    return 0;
}

/* See description in tapi_geo.h */
te_errno
tapi_geo_parse(rcf_rpc_server *rpcs, const char *fmt, const char *text,
               const char *crs, const char *params, tapi_geo_geometry **geom,
               te_string *category)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    te_errno rc;

    *geom = NULL;
    if (crs != NULL && *crs != '\0')
        te_string_append(&p, "crs=%s\n", crs);
    if (params != NULL)
        te_string_append(&p, "%s\n", params);
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_GEOM_PARSE, 0, 0, fmt, text,
                       p.ptr, &r);
    if (rc == 0)
        rc = tgeo_wrap(rpcs, &r, geom);
    else if (category != NULL)
    {
        char cat[64];

        TGEO_STR(&r, "category", cat);
        te_string_append(category, "%s", cat);
    }
    te_string_free(&p);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

static te_errno
parse_srid(rcf_rpc_server *rpcs, const char *fmt, const char *text, int srid,
           tapi_geo_geometry **geom)
{
    char crs[32] = "";

    if (srid > 0)
        snprintf(crs, sizeof(crs), "EPSG:%d", srid);
    return tapi_geo_parse(rpcs, fmt, text, crs, NULL, geom, NULL);
}

te_errno
tapi_geo_parse_wkt(rcf_rpc_server *rpcs, const char *wkt, int srid,
                   tapi_geo_geometry **geom)
{
    return parse_srid(rpcs, "wkt", wkt, srid, geom);
}

te_errno
tapi_geo_parse_wkb(rcf_rpc_server *rpcs, const char *hex, int srid,
                   tapi_geo_geometry **geom)
{
    return parse_srid(rpcs, "wkb", hex, srid, geom);
}

te_errno
tapi_geo_parse_geojson(rcf_rpc_server *rpcs, const char *json, int srid,
                       tapi_geo_geometry **geom)
{
    return parse_srid(rpcs, "geojson", json, srid, geom);
}

/* See description in tapi_geo.h */
te_errno
tapi_geo_geometry_free(rcf_rpc_server *rpcs, tapi_geo_geometry *geom)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc;
    int h;

    if (geom == NULL)
        return 0;
    rc = tgeo_handle(rpcs, geom, &h);
    if (rc == 0)
        rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_RELEASE, h, 0, "", "", "", &r);
    tapi_geo_rpc_res_free(&r);
    free(geom);
    return rc;
}

static const char *
fmt2str(tapi_geo_format f)
{
    switch (f)
    {
        case TAPI_GEO_FMT_WKB: return "wkb";
        case TAPI_GEO_FMT_EWKB: return "ewkb";
        case TAPI_GEO_FMT_GEOJSON: return "geojson";
        default: return "wkt";
    }
}

/* See description in tapi_geo.h */
te_errno
tapi_geo_serialize(rcf_rpc_server *rpcs, const tapi_geo_geometry *geom,
                   tapi_geo_format fmt, int precision, te_string *out)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    char p[32];
    te_errno rc;
    int h;

    if ((rc = tgeo_handle(rpcs, geom, &h)) != 0)
        return rc;
    snprintf(p, sizeof(p), "precision=%d", precision);
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_GEOM_SERIALIZE, h, 0,
                       fmt2str(fmt), "", p, &r);
    if (rc == 0 && r.report.ptr != NULL)
        te_string_append(out, "%s", r.report.ptr);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* See description in tapi_geo.h */
te_errno
tapi_geo_info_get(rcf_rpc_server *rpcs, const tapi_geo_geometry *geom,
                  tapi_geo_info *info)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc;
    int h;

    if ((rc = tgeo_handle(rpcs, geom, &h)) != 0)
        return rc;
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_GEOM_INFO, h, 0, "", "", "", &r);
    if (rc == 0)
    {
        memset(info, 0, sizeof(*info));
        TGEO_STR(&r, "type", info->type);
        TGEO_STR(&r, "dims", info->dims);
        TGEO_STR(&r, "crs", info->crs);
        info->npoints = tapi_geo_res_int(&r, "npoints", 0);
        info->ngeoms = tapi_geo_res_int(&r, "ngeoms", 0);
        info->empty = tapi_geo_res_int(&r, "empty", 0) != 0;
        info->dimension = (int)tapi_geo_res_int(&r, "dimension", 0);
        info->bbox.minx = tapi_geo_res_dbl(&r, "minx");
        info->bbox.miny = tapi_geo_res_dbl(&r, "miny");
        info->bbox.maxx = tapi_geo_res_dbl(&r, "maxx");
        info->bbox.maxy = tapi_geo_res_dbl(&r, "maxy");
        info->minz = tapi_geo_res_dbl(&r, "minz");
        info->maxz = tapi_geo_res_dbl(&r, "maxz");
        info->minm = tapi_geo_res_dbl(&r, "minm");
        info->maxm = tapi_geo_res_dbl(&r, "maxm");
        info->nonfinite = tapi_geo_res_int(&r, "nonfinite", 0);
    }
    tapi_geo_rpc_res_free(&r);
    return rc;
}

te_errno
tapi_geo_bounds(rcf_rpc_server *rpcs, const tapi_geo_geometry *geom,
                tapi_geo_bbox *bbox)
{
    tapi_geo_info i;
    te_errno rc = tapi_geo_info_get(rpcs, geom, &i);

    if (rc == 0)
        *bbox = i.bbox;
    return rc;
}

/* See description in tapi_geo.h */
te_errno
tapi_geo_derive(rcf_rpc_server *rpcs, const tapi_geo_geometry *geom,
                const char *what, const char *params, tapi_geo_geometry **out)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc;
    int h;

    *out = NULL;
    if ((rc = tgeo_handle(rpcs, geom, &h)) != 0)
        return rc;
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_GEOM_DERIVE, h, 0, what, "",
                       params, &r);
    if (rc == 0)
        rc = tgeo_wrap(rpcs, &r, out);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

te_errno
tapi_geo_centroid(rcf_rpc_server *rpcs, const tapi_geo_geometry *geom,
                  tapi_geo_geometry **out)
{
    return tapi_geo_derive(rpcs, geom, "centroid", NULL, out);
}

te_errno
tapi_geo_envelope(rcf_rpc_server *rpcs, const tapi_geo_geometry *geom,
                  tapi_geo_geometry **out)
{
    return tapi_geo_derive(rpcs, geom, "envelope", NULL, out);
}

/* ---- validation ---------------------------------------------------- */

void
tapi_geo_validity_free(tapi_geo_validity *v)
{
    free(v->list);
    v->list = NULL;
    v->n_list = 0;
}

void
tgeo_validity_read(tapi_geo_res *r, tapi_geo_validity *v)
{
    char *cur = r->report.ptr;
    char *f[8];
    unsigned int cap = 0;

    memset(v, 0, sizeof(*v));
    v->valid = tapi_geo_res_int(r, "valid", 1) != 0;
    v->ok = tapi_geo_res_int(r, "ok", 0) != 0;
    v->issues = (unsigned int)tapi_geo_res_int(r, "issues", 0);
    TGEO_STR(r, "category", v->category);
    TGEO_STR(r, "reason", v->reason);
    TGEO_STR(r, "id", v->id);
    v->x = tapi_geo_res_dbl(r, "location_x");
    v->y = tapi_geo_res_dbl(r, "location_y");
    while (tapi_geo_report_next(&cur, f, 8) >= 5)
    {
        tapi_geo_issue *is;

        if (v->n_list == cap)
        {
            tapi_geo_issue *nl;

            cap = cap == 0 ? 16 : cap * 2;
            nl = realloc(v->list, cap * sizeof(*nl));
            if (nl == NULL)
                break;
            v->list = nl;
        }
        is = &v->list[v->n_list++];
        snprintf(is->category, sizeof(is->category), "%s", f[1]);
        is->x = strtod(f[2], NULL);
        is->y = strtod(f[3], NULL);
        snprintf(is->diagnostic, sizeof(is->diagnostic), "%s", f[4]);
    }
}

/* See description in tapi_geo.h */
te_errno
tapi_geo_validate(rcf_rpc_server *rpcs, const tapi_geo_geometry *geom,
                  const char *checks, const char *params,
                  tapi_geo_validity *v)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc;
    int h;

    if ((rc = tgeo_handle(rpcs, geom, &h)) != 0)
        return rc;
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_GEOM_CHECK, h, 0,
                       checks != NULL ? checks : "all", "", params, &r);
    if (rc == 0)
        tgeo_validity_read(&r, v);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

te_errno
tapi_geo_check_validity(rcf_rpc_server *rpcs, const tapi_geo_geometry *geom,
                        const char *id, tapi_geo_validity *v)
{
    te_string p = TE_STRING_INIT;
    te_errno rc;
    tapi_geo_validity fin;

    if (id != NULL)
        te_string_append(&p, "id=%s\n", id);
    rc = tapi_geo_validate(rpcs, geom, "validity", p.ptr, v);
    te_string_free(&p);
    if (rc != 0)
        return rc;
    /* Non-finite coordinates and emptiness are validity findings too. */
    rc = tapi_geo_validate(rpcs, geom, "finite", NULL, &fin);
    if (rc == 0 && !fin.ok)
    {
        v->ok = false;
        v->valid = false;
        v->issues += fin.issues;
        if (v->category[0] == '\0')
        {
            snprintf(v->category, sizeof(v->category), "%s", fin.category);
            snprintf(v->reason, sizeof(v->reason), "%s", fin.reason);
        }
    }
    tapi_geo_validity_free(&fin);
    if (rc == 0)
    {
        rc = tapi_geo_validate(rpcs, geom, "empty", NULL, &fin);
        if (rc == 0 && !fin.ok)
        {
            v->ok = false;
            v->issues += fin.issues;
            if (v->category[0] == '\0')
                snprintf(v->category, sizeof(v->category), "empty");
        }
        tapi_geo_validity_free(&fin);
    }
    return rc;
}

static te_errno
count_check(rcf_rpc_server *rpcs, const tapi_geo_geometry *geom,
            const char *check, const char *params, const char *key,
            unsigned int *out)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc;
    int h;

    if ((rc = tgeo_handle(rpcs, geom, &h)) != 0)
        return rc;
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_GEOM_CHECK, h, 0, check, "", params,
                       &r);
    if (rc == 0)
        *out = (unsigned int)tapi_geo_res_int(&r, key, 0);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

te_errno
tapi_geo_check_orientation(rcf_rpc_server *rpcs,
                           const tapi_geo_geometry *geom, bool ccw,
                           unsigned int *misoriented)
{
    return count_check(rpcs, geom, "orientation",
                       ccw ? "orientation=ccw" : "orientation=cw",
                       "misoriented", misoriented);
}

te_errno
tapi_geo_check_duplicates(rcf_rpc_server *rpcs,
                          const tapi_geo_geometry *geom, double tolerance,
                          unsigned int *duplicates)
{
    char p[64];

    snprintf(p, sizeof(p), "tolerance=%.17g", tolerance);
    return count_check(rpcs, geom, "duplicates", p, "duplicates",
                       duplicates);
}

te_errno
tapi_geo_check_bounds(rcf_rpc_server *rpcs, const tapi_geo_geometry *geom,
                      const tapi_geo_bbox *b, unsigned int *outside)
{
    char p[160];

    snprintf(p, sizeof(p), "bounds=%.17g %.17g %.17g %.17g", b->minx, b->miny,
             b->maxx, b->maxy);
    return count_check(rpcs, geom, "bounds", p, "out_of_bounds", outside);
}

/* See description in tapi_geo.h */
te_errno
tapi_geo_repair(rcf_rpc_server *rpcs, const tapi_geo_geometry *geom,
                tapi_geo_repair_method method, tapi_geo_geometry **out)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    const char *k = "make_valid";
    const char *p = "";
    te_errno rc;
    int h;

    *out = NULL;
    switch (method)
    {
        case TAPI_GEO_REPAIR_MAKE_VALID_STRUCTURE:
            p = "method=structure";
            break;
        case TAPI_GEO_REPAIR_BUFFER0:
            k = "buffer0";
            break;
        case TAPI_GEO_REPAIR_ORIENT_CCW:
            k = "orient";
            p = "orientation=ccw";
            break;
        case TAPI_GEO_REPAIR_REMOVE_DUPLICATES:
            k = "remove_duplicates";
            break;
        default:
            break;
    }
    if ((rc = tgeo_handle(rpcs, geom, &h)) != 0)
        return rc;
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_GEOM_REPAIR, h, 0, k, "", p, &r);
    if (rc == 0)
        rc = tgeo_wrap(rpcs, &r, out);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* ---- predicates ---------------------------------------------------- */

/* See description in tapi_geo.h */
te_errno
tapi_geo_predicate(rcf_rpc_server *rpcs, const char *name,
                   const tapi_geo_geometry *a, const tapi_geo_geometry *b,
                   const char *params, bool *res, char *de9im)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc;
    int ha, hb;

    if ((rc = tgeo_handle(rpcs, a, &ha)) != 0 ||
        (rc = tgeo_handle(rpcs, b, &hb)) != 0)
        return rc;
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_GEOM_PREDICATE, ha, hb, name, "",
                       params, &r);
    if (rc == 0)
    {
        if (res != NULL)
            *res = tapi_geo_res_int(&r, "value", 0) != 0;
        if (de9im != NULL &&
            tapi_geo_res_str(&r, "de9im", de9im, 10) == NULL)
            de9im[0] = '\0';
    }
    tapi_geo_rpc_res_free(&r);
    return rc;
}

#define PRED(_name) \
te_errno                                                                \
tapi_geo_##_name(rcf_rpc_server *rpcs, const tapi_geo_geometry *a,      \
                 const tapi_geo_geometry *b, bool *res)                 \
{                                                                       \
    return tapi_geo_predicate(rpcs, #_name, a, b, NULL, res, NULL);     \
}

PRED(intersects)
PRED(contains)
PRED(within)
PRED(touches)
PRED(crosses)
PRED(overlaps)
PRED(disjoint)
PRED(equals)
#undef PRED

/* ---- construction -------------------------------------------------- */

/* See description in tapi_geo.h */
te_errno
tapi_geo_construct(rcf_rpc_server *rpcs, const char *name,
                   const tapi_geo_geometry *a, const tapi_geo_geometry *b,
                   const char *params, tapi_geo_geometry **out)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc;
    int ha, hb;

    *out = NULL;
    if ((rc = tgeo_handle(rpcs, a, &ha)) != 0 ||
        (rc = tgeo_handle(rpcs, b, &hb)) != 0)
        return rc;
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_GEOM_CONSTRUCT, ha, hb, name, "",
                       params, &r);
    if (rc == 0)
        rc = tgeo_wrap(rpcs, &r, out);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

#define CONS(_fn, _name) \
te_errno                                                                \
tapi_geo_##_fn(rcf_rpc_server *rpcs, const tapi_geo_geometry *a,        \
               const tapi_geo_geometry *b, tapi_geo_geometry **out)     \
{                                                                       \
    return tapi_geo_construct(rpcs, _name, a, b, NULL, out);            \
}

CONS(intersection, "intersection")
CONS(union, "union")
CONS(difference, "difference")
CONS(symmetric_difference, "sym_difference")
#undef CONS

te_errno
tapi_geo_buffer(rcf_rpc_server *rpcs, const tapi_geo_geometry *geom,
                double distance, unsigned int quad_segs,
                tapi_geo_geometry **out)
{
    char p[96];

    snprintf(p, sizeof(p), "distance=%.17g\nquad_segs=%u", distance,
             quad_segs != 0 ? quad_segs : 8);
    return tapi_geo_construct(rpcs, "buffer", geom, NULL, p, out);
}

te_errno
tapi_geo_simplify(rcf_rpc_server *rpcs, const tapi_geo_geometry *geom,
                  double tolerance, bool preserve_topology,
                  tapi_geo_geometry **out)
{
    char p[96];

    snprintf(p, sizeof(p), "tolerance=%.17g\npreserve_topology=%d", tolerance,
             preserve_topology);
    return tapi_geo_construct(rpcs, "simplify", geom, NULL, p, out);
}

/* ---- measurement --------------------------------------------------- */

const char *
tapi_geo_axis2str(tapi_geo_axis axis)
{
    return axis == TAPI_GEO_AXIS_AUTHORITY ? "authority" : "gis";
}

/* See description in tapi_geo.h */
te_errno
tapi_geo_measure_any(rcf_rpc_server *rpcs, const char *what,
                     const tapi_geo_geometry *a, const tapi_geo_geometry *b,
                     tapi_geo_mode mode, tapi_geo_axis axis,
                     const char *params, tapi_geo_measure *m)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    te_errno rc;
    int ha, hb;

    if ((rc = tgeo_handle(rpcs, a, &ha)) != 0 ||
        (rc = tgeo_handle(rpcs, b, &hb)) != 0)
        return rc;
    te_string_append(&p, "mode=%s\n", mode == TAPI_GEO_GEODESIC ?
                                      "geodesic" : "planar");
    if (mode == TAPI_GEO_GEODESIC)
        te_string_append(&p, "axis=%s\n", tapi_geo_axis2str(axis));
    if (params != NULL)
        te_string_append(&p, "%s\n", params);
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_GEOM_MEASURE, ha, hb, what, "",
                       p.ptr, &r);
    if (rc == 0)
    {
        memset(m, 0, sizeof(*m));
        m->value = tapi_geo_res_dbl(&r, "value");
        m->mode = mode;
        TGEO_STR(&r, "unit", m->unit);
        TGEO_STR(&r, "method", m->method);
        TGEO_STR(&r, "ignored", m->ignored);
        m->geographic = tapi_geo_res_int(&r, "geographic", 0) != 0;
    }
    te_string_free(&p);
    tapi_geo_rpc_res_free(&r);
    return rc;
}

te_errno
tapi_geo_distance(rcf_rpc_server *rpcs, const tapi_geo_geometry *a,
                  const tapi_geo_geometry *b, tapi_geo_mode mode,
                  tapi_geo_axis axis, tapi_geo_measure *m)
{
    return tapi_geo_measure_any(rpcs, "distance", a, b, mode, axis, NULL, m);
}

te_errno
tapi_geo_length(rcf_rpc_server *rpcs, const tapi_geo_geometry *g,
                tapi_geo_mode mode, tapi_geo_axis axis, tapi_geo_measure *m)
{
    return tapi_geo_measure_any(rpcs, "length", g, NULL, mode, axis, NULL, m);
}

te_errno
tapi_geo_area(rcf_rpc_server *rpcs, const tapi_geo_geometry *g,
              tapi_geo_mode mode, tapi_geo_axis axis, tapi_geo_measure *m)
{
    return tapi_geo_measure_any(rpcs, "area", g, NULL, mode, axis, NULL, m);
}

te_errno
tapi_geo_perimeter(rcf_rpc_server *rpcs, const tapi_geo_geometry *g,
                   tapi_geo_mode mode, tapi_geo_axis axis,
                   tapi_geo_measure *m)
{
    return tapi_geo_measure_any(rpcs, "perimeter", g, NULL, mode, axis, NULL,
                                m);
}

/* ---- generation ---------------------------------------------------- */

void
tapi_geo_gen_items_free(tapi_geo_gen_item *items, unsigned int n)
{
    unsigned int i;

    for (i = 0; items != NULL && i < n; i++)
        free(items[i].text);
    free(items);
}

/* See description in tapi_geo.h */
te_errno
tapi_geo_generate(rcf_rpc_server *rpcs, const char *kind, const char *params,
                  tapi_geo_gen_item **items, unsigned int *n,
                  te_string *digest)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_errno rc;
    char *cur;
    char *f[6];
    unsigned int cap = 0;

    *items = NULL;
    *n = 0;
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_GEN, 0, 0, kind, "", params, &r);
    if (rc != 0)
        goto out;
    if (digest != NULL)
    {
        char d[32];

        TGEO_STR(&r, "digest", d);
        te_string_append(digest, "%s", d);
    }
    cur = r.report.ptr;
    while (tapi_geo_report_next(&cur, f, 5) == 5)
    {
        tapi_geo_gen_item *it;

        if (*n == cap)
        {
            tapi_geo_gen_item *ni;

            cap = cap == 0 ? 64 : cap * 2;
            ni = realloc(*items, cap * sizeof(*ni));
            if (ni == NULL)
            {
                rc = TE_RC(TE_TAPI, TE_ENOMEM);
                break;
            }
            *items = ni;
        }
        it = &(*items)[(*n)++];
        it->id = strtoll(f[0], NULL, 10);
        snprintf(it->fmt, sizeof(it->fmt), "%s", f[1]);
        snprintf(it->expect, sizeof(it->expect), "%s", f[2]);
        it->t = strtod(f[3], NULL);
        it->text = strdup(f[4]);
    }
out:
    tapi_geo_rpc_res_free(&r);
    return rc;
}

/* ---- performance --------------------------------------------------- */

/* See description in tapi_geo.h */
te_errno
tapi_geo_perf_run(rcf_rpc_server *rpcs, const char *what,
                  const tapi_geo_geometry *a, const tapi_geo_geometry *b,
                  const char *text, const char *params, tapi_geo_perf *perf)
{
    tapi_geo_res r = TAPI_GEO_RES_INIT;
    te_string p = TE_STRING_INIT;
    te_errno rc;
    int ha, hb;

    if ((rc = tgeo_handle(rpcs, a, &ha)) != 0 ||
        (rc = tgeo_handle(rpcs, b, &hb)) != 0)
        return rc;
    te_string_append(&p, "what=%s\n%s\n", what, params != NULL ? params : "");
    rc = tapi_geo_call(rpcs, TAPI_GEO_RPC_PERF, ha, hb, "", text, p.ptr, &r);
    if (rc == 0)
    {
        perf->iterations = tapi_geo_res_int(&r, "iterations", 0);
        perf->cold_us = tapi_geo_res_dbl(&r, "cold_us");
        perf->warm_us = tapi_geo_res_dbl(&r, "warm_us");
        perf->ops_per_s = tapi_geo_res_dbl(&r, "ops_per_s");
        perf->items_per_s = tapi_geo_res_dbl(&r, "items_per_s");
        perf->work_items = tapi_geo_res_int(&r, "work_items", 0);
    }
    te_string_free(&p);
    tapi_geo_rpc_res_free(&r);
    return rc;
}
