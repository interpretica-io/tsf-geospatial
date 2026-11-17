/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial engine: OGC services (WMS, WFS, OGC API - Features)
 *
 * What a service says about itself and what it then serves, checked
 * for geospatial sense, not for a 200: capabilities that parse and name
 * their layers, CRS that PROJ knows, bounding boxes that are boxes on
 * the Earth; a map that is an image of the size asked for; features
 * whose geometries are valid and inside the extent the service
 * advertised; paging that returns the same set of features whatever
 * the page size; credentials that are actually required.
 *
 * Requests go out from the agent with libcurl (status codes and content
 * types matter here); features come through GDAL's own WFS and OAPIF
 * drivers, so they are the features a GDAL-based client would see; XML
 * is parsed with GDAL's CPL XML parser.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cpl_minixml.h>
#include <cpl_vsi.h>

#ifdef TA_GEO_HAVE_CURL
#include <curl/curl.h>
#endif

#include "ta_geo_int.h"

typedef struct http_res {
    long status;
    char ctype[128];
    sbuf body;
    bool truncated;
} http_res;

#ifdef TA_GEO_HAVE_CURL
typedef struct wctx {
    sbuf *b;
    size_t max;
    bool *trunc;
} wctx;

static size_t
http_write(char *p, size_t sz, size_t n, void *ud)
{
    wctx *w = ud;
    size_t len = sz * n;

    if (w->b->len + len > w->max)
    {
        *w->trunc = true;
        return 0;
    }
    sb_put(w->b, p, len);
    return len;
}
#endif

/* GET a URL. Transport failures are EIO; any HTTP status is a result. */
static int
http_get(const char *url, const char *user, const char *pass,
         const char *accept, long timeout_ms, size_t max_bytes,
         http_res *res, char **err)
{
#ifdef TA_GEO_HAVE_CURL
    CURL *c = curl_easy_init();
    CURLcode cr;
    struct curl_slist *hdr = NULL;
    char *ct = NULL;
    wctx w;
    char ebuf[CURL_ERROR_SIZE] = "";

    memset(res, 0, sizeof(*res));
    if (c == NULL)
        return seterr(err, TA_GEO_ENOMEM, "curl init failed");
    w.b = &res->body;
    w.max = max_bytes;
    w.trunc = &res->truncated;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, http_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &w);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, ebuf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#endif
    if (user != NULL)
    {
        curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(c, CURLOPT_USERNAME, user);
        curl_easy_setopt(c, CURLOPT_PASSWORD, pass != NULL ? pass : "");
    }
    if (accept != NULL)
    {
        char h[160];

        snprintf(h, sizeof(h), "Accept: %s", accept);
        hdr = curl_slist_append(hdr, h);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    }
    cr = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &res->status);
    curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
    if (ct != NULL)
        snprintf(res->ctype, sizeof(res->ctype), "%s", ct);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    if (cr != CURLE_OK && !(cr == CURLE_WRITE_ERROR && res->truncated))
    {
        sb_free(&res->body);
        return seterr(err, cr == CURLE_OPERATION_TIMEDOUT ?
                      TA_GEO_ETIMEDOUT : TA_GEO_EIO, "GET %s: %s", url,
                      ebuf[0] != '\0' ? ebuf : curl_easy_strerror(cr));
    }
    return TA_GEO_OK;
#else
    (void)url; (void)user; (void)pass; (void)accept; (void)timeout_ms;
    (void)max_bytes;
    memset(res, 0, sizeof(*res));
    return seterr(err, TA_GEO_ENOTSUP, "built without libcurl");
#endif
}

/* ---- issues -------------------------------------------------------- */

typedef struct ogc_iss {
    sbuf *rep;
    long long n;
} ogc_iss;

static void
oissue(ogc_iss *s, const char *cat, const char *fmt, ...) GEO_PRINTF(3, 4);

static void
oissue(ogc_iss *s, const char *cat, const char *fmt, ...)
{
    va_list ap;
    sbuf d = SBUF_INIT;

    va_start(ap, fmt);
    sb_vprintf(&d, fmt, ap);
    va_end(ap);
    if (s->n++ < geo_lim.max_report)
    {
        sb_printf(s->rep, "issue\t%s\t", cat);
        sb_field(s->rep, d.p);
        sb_puts(s->rep, "\n");
    }
    sb_free(&d);
}

static void
url_with(sbuf *u, const char *base, const char *query)
{
    sb_puts(u, base);
    if (strchr(base, '?') == NULL)
        sb_puts(u, "?");
    else if (base[strlen(base) - 1] != '?' && base[strlen(base) - 1] != '&')
        sb_puts(u, "&");
    sb_puts(u, query);
}

/* ---- capabilities -------------------------------------------------- */

/* A CRS as services write it: EPSG:n, urn:ogc:def:crs:EPSG::n, CRS:84,
 * http://www.opengis.net/def/crs/EPSG/0/n. PROJ reads all of them. */
static bool
crs_known(const char *s)
{
    PJ *p;

    if (strcmp(s, "CRS:84") == 0 || strcmp(s, "CRS84") == 0)
        s = "OGC:CRS84";
    p = geo_crs_create(s, NULL);
    proj_destroy(p);
    return p != NULL;
}

typedef struct cap_ctx {
    ogc_iss *iss;
    sbuf *rep;
    long long layers;
    const char *expect_crs;
    bool wms13;
} cap_ctx;

static bool
has_crs(const char *list, const char *want)
{
    char buf[1024];
    char *tok;
    char *save = NULL;

    snprintf(buf, sizeof(buf), "%s", want);
    for (tok = strtok_r(buf, ",", &save); tok != NULL;
         tok = strtok_r(NULL, ",", &save))
    {
        if (strstr(list, tok) == NULL)
            return false;
    }
    return true;
}

static void
check_geo_bbox(cap_ctx *c, const char *layer, double w, double s, double e,
               double n)
{
    if (!(isfinite(w) && isfinite(s) && isfinite(e) && isfinite(n)))
        oissue(c->iss, "bbox", "layer %s: geographic bounding box is "
               "missing or not numeric", layer);
    else if (w < -180 || e > 180 || s < -90 || n > 90)
        oissue(c->iss, "bbox", "layer %s: geographic bounding box %g %g %g "
               "%g is off the Earth (axis order swapped?)", layer, w, s, e,
               n);
    else if (s > n)
        oissue(c->iss, "bbox", "layer %s: south %g above north %g", layer, s,
               n);
    else if (w == e || s == n)
        oissue(c->iss, "bbox", "layer %s: bounding box has no area", layer);
}

/*
 * A WMS 1.3.0 layer states its extent twice: as a geographic box in
 * longitude/latitude (EX_GeographicBoundingBox) and as BoundingBox
 * elements in the axis order of each listed CRS. Transform the first
 * into each CRS and the two must agree; an axis swap in either shows.
 */
static void
check_bbox_crs(cap_ctx *c, const char *layer, CPLXMLNode *bb, double w,
               double s, double e, double n)
{
    const char *crs = CPLGetXMLValue(bb, "CRS", CPLGetXMLValue(bb, "SRS",
                                                               NULL));
    double d[4], o[4];
    PJ *src, *dst, *op;
    char norm[256];
    double tol;
    int k;

    if (crs == NULL || !isfinite(w) || !isfinite(s) || !isfinite(e) ||
        !isfinite(n))
        return;
    d[0] = atof(CPLGetXMLValue(bb, "minx", "nan"));
    d[1] = atof(CPLGetXMLValue(bb, "miny", "nan"));
    d[2] = atof(CPLGetXMLValue(bb, "maxx", "nan"));
    d[3] = atof(CPLGetXMLValue(bb, "maxy", "nan"));
    if (strcmp(crs, "CRS:84") == 0)
        crs = "OGC:CRS84";
    geo_crs_norm(crs, norm, sizeof(norm));
    src = geo_crs_create("OGC:CRS84", NULL);
    dst = geo_crs_create(norm, NULL);
    op = src != NULL && dst != NULL ?
         proj_create_crs_to_crs_from_pj(geo_proj(), src, dst, NULL, NULL) :
         NULL;
    if (op != NULL &&
        proj_trans_bounds(geo_proj(), op, PJ_FWD, w, s, e, n, &o[0], &o[1],
                          &o[2], &o[3], 21))
    {
        /* One per cent of the box, or a millionth of a unit. */
        tol = fmax(fmax(fabs(o[2] - o[0]), fabs(o[3] - o[1])) * 0.01, 1e-6);
        for (k = 0; k < 4; k++)
        {
            if (!(fabs(o[k] - d[k]) <= tol))
            {
                oissue(c->iss, "bbox", "layer %s: BoundingBox in %s is "
                       "%.10g %.10g %.10g %.10g, the geographic box gives "
                       "%.10g %.10g %.10g %.10g there (axis order?)", layer,
                       crs, d[0], d[1], d[2], d[3], o[0], o[1], o[2], o[3]);
                break;
            }
        }
    }
    proj_destroy(op);
    proj_destroy(src);
    proj_destroy(dst);
}

/* WMS: walk Layer elements, inheriting CRS from the parents. */
static void
wms_layers(cap_ctx *c, CPLXMLNode *n, const char *inherited)
{
    CPLXMLNode *ch;

    for (; n != NULL; n = n->psNext)
    {
        sbuf crs = SBUF_INIT;
        const char *name;

        if (n->eType != CXT_Element || strcmp(n->pszValue, "Layer") != 0)
            continue;
        sb_puts(&crs, inherited);
        for (ch = n->psChild; ch != NULL; ch = ch->psNext)
        {
            if (ch->eType == CXT_Element &&
                (strcmp(ch->pszValue, "CRS") == 0 ||
                 strcmp(ch->pszValue, "SRS") == 0))
            {
                const char *v = CPLGetXMLValue(ch, NULL, "");

                if (crs.len > 0)
                    sb_puts(&crs, ",");
                sb_puts(&crs, v);
                if (!crs_known(v))
                    oissue(c->iss, "crs_unknown", "CRS '%s' is not a "
                           "reference system PROJ knows", v);
            }
        }
        name = CPLGetXMLValue(n, "Name", NULL);
        if (name != NULL)
        {
            double w = atof(CPLGetXMLValue(n,
                "EX_GeographicBoundingBox.westBoundLongitude", "nan"));
            double e = atof(CPLGetXMLValue(n,
                "EX_GeographicBoundingBox.eastBoundLongitude", "nan"));
            double s = atof(CPLGetXMLValue(n,
                "EX_GeographicBoundingBox.southBoundLatitude", "nan"));
            double nn = atof(CPLGetXMLValue(n,
                "EX_GeographicBoundingBox.northBoundLatitude", "nan"));

            if (!c->wms13)
            {
                w = atof(CPLGetXMLValue(n, "LatLonBoundingBox.minx", "nan"));
                s = atof(CPLGetXMLValue(n, "LatLonBoundingBox.miny", "nan"));
                e = atof(CPLGetXMLValue(n, "LatLonBoundingBox.maxx", "nan"));
                nn = atof(CPLGetXMLValue(n, "LatLonBoundingBox.maxy", "nan"));
            }
            c->layers++;
            check_geo_bbox(c, name, w, s, e, nn);
            if (c->wms13)
            {
                for (ch = n->psChild; ch != NULL; ch = ch->psNext)
                {
                    if (ch->eType == CXT_Element &&
                        strcmp(ch->pszValue, "BoundingBox") == 0)
                        check_bbox_crs(c, name, ch, w, s, e, nn);
                }
            }
            if (crs.len == 0)
                oissue(c->iss, "crs_missing", "layer %s declares no CRS",
                       name);
            else if (c->expect_crs != NULL && !has_crs(crs.p, c->expect_crs))
                oissue(c->iss, "crs_missing", "layer %s lacks %s", name,
                       c->expect_crs);
            sb_puts(c->rep, "layer\t");
            sb_field(c->rep, name);
            sb_puts(c->rep, "\t");
            sb_field(c->rep, crs.p);
            sb_printf(c->rep, "\t%.17g %.17g %.17g %.17g\n", w, s, e, nn);
        }
        wms_layers(c, n->psChild, crs.p != NULL ? crs.p : "");
        sb_free(&crs);
    }
}

static void
wfs_layers(cap_ctx *c, CPLXMLNode *root)
{
    CPLXMLNode *list = CPLGetXMLNode(root, "FeatureTypeList");
    CPLXMLNode *n;

    for (n = list != NULL ? list->psChild : NULL; n != NULL; n = n->psNext)
    {
        const char *name;
        const char *def;
        sbuf crs = SBUF_INIT;
        CPLXMLNode *ch;
        double w = NAN, s = NAN, e = NAN, nn = NAN;
        const char *lc, *uc;

        if (n->eType != CXT_Element || strcmp(n->pszValue, "FeatureType") != 0)
            continue;
        name = CPLGetXMLValue(n, "Name", "");
        def = CPLGetXMLValue(n, "DefaultCRS", CPLGetXMLValue(n, "DefaultSRS",
                             CPLGetXMLValue(n, "SRS", "")));
        sb_puts(&crs, def);
        if (*def != '\0' && !crs_known(def))
            oissue(c->iss, "crs_unknown", "CRS '%s' is not a reference "
                   "system PROJ knows", def);
        for (ch = n->psChild; ch != NULL; ch = ch->psNext)
        {
            if (ch->eType == CXT_Element &&
                (strcmp(ch->pszValue, "OtherCRS") == 0 ||
                 strcmp(ch->pszValue, "OtherSRS") == 0))
            {
                const char *v = CPLGetXMLValue(ch, NULL, "");

                sb_puts(&crs, ",");
                sb_puts(&crs, v);
                if (!crs_known(v))
                    oissue(c->iss, "crs_unknown", "CRS '%s' is not a "
                           "reference system PROJ knows", v);
            }
        }
        lc = CPLGetXMLValue(n, "WGS84BoundingBox.LowerCorner", NULL);
        uc = CPLGetXMLValue(n, "WGS84BoundingBox.UpperCorner", NULL);
        if (lc != NULL && uc != NULL)
        {
            sscanf(lc, "%lf %lf", &w, &s);
            sscanf(uc, "%lf %lf", &e, &nn);
        }
        c->layers++;
        check_geo_bbox(c, name, w, s, e, nn);
        if (crs.len == 0)
            oissue(c->iss, "crs_missing", "feature type %s declares no CRS",
                   name);
        else if (c->expect_crs != NULL && !has_crs(crs.p, c->expect_crs))
            oissue(c->iss, "crs_missing", "feature type %s lacks %s", name,
                   c->expect_crs);
        sb_puts(c->rep, "layer\t");
        sb_field(c->rep, name);
        sb_puts(c->rep, "\t");
        sb_field(c->rep, crs.p);
        sb_printf(c->rep, "\t%.17g %.17g %.17g %.17g\n", w, s, e, nn);
        sb_free(&crs);
    }
}

typedef struct ogc_opts {
    const char *service;
    const char *version;
    const char *user;
    const char *pass;
    long long timeout_ms;
    long long max_bytes;
} ogc_opts;

static int
ogc_opts_read(geo_req *r, ogc_opts *o)
{
    int rc;

    o->service = kv_str(r->opt, "service", "wms");
    o->user = kv_get(r->opt, "user");
    o->pass = kv_get(r->opt, "password");
    if ((rc = kv_long(r->opt, "http_timeout_ms", 30000, &o->timeout_ms,
                      r->err)) != 0 ||
        (rc = kv_long(r->opt, "max_bytes", 64 * 1024 * 1024, &o->max_bytes,
                      r->err)) != 0)
        return rc;
    o->version = kv_str(r->opt, "version",
                        strcmp(o->service, "wfs") == 0 ? "2.0.0" : "1.3.0");
    if (strcmp(o->service, "wms") != 0 && strcmp(o->service, "wfs") != 0 &&
        strcmp(o->service, "oapif") != 0)
        return seterr(r->err, TA_GEO_EINVAL, "service is wms, wfs or oapif");
    return TA_GEO_OK;
}

static int
ogc_capabilities(geo_req *r, ogc_opts *o)
{
    sbuf u = SBUF_INIT;
    char q[256];
    http_res h;
    ogc_iss iss = { r->rep, 0 };
    CPLXMLNode *doc;
    CPLXMLNode *root;
    const char *expect_layer = kv_get(r->opt, "expect_layer");
    cap_ctx c;
    int rc;

    snprintf(q, sizeof(q), "SERVICE=%s&REQUEST=GetCapabilities&VERSION=%s",
             strcmp(o->service, "wfs") == 0 ? "WFS" : "WMS", o->version);
    url_with(&u, r->text, q);
    rc = http_get(u.p, o->user, o->pass, NULL, o->timeout_ms,
                  (size_t)o->max_bytes, &h, r->err);
    sb_kv_str(r->res, "url", u.p);
    sb_free(&u);
    if (rc != 0)
        return rc;
    sb_kv_int(r->res, "http_status", h.status);
    sb_kv_str(r->res, "content_type", h.ctype);
    if (h.status != 200)
        oissue(&iss, "http_status", "GetCapabilities answered %ld",
               h.status);
    if (strstr(h.ctype, "xml") == NULL)
        oissue(&iss, "content_type", "content type '%s' is not XML",
               h.ctype);
    doc = h.body.p != NULL ? CPLParseXMLString(h.body.p) : NULL;
    sb_free(&h.body);
    if (doc == NULL)
    {
        oissue(&iss, "xml", "the capabilities document does not parse");
        goto out;
    }
    CPLStripXMLNamespace(doc, NULL, TRUE);
    /* Skip the XML declaration and processing instructions, which the
     * CPL parser keeps as elements named "?xml"... */
    for (root = doc; root != NULL && (root->eType != CXT_Element ||
                                      root->pszValue[0] == '?');
         root = root->psNext)
        ;
    if (root == NULL)
    {
        oissue(&iss, "xml", "no root element");
        CPLDestroyXMLNode(doc);
        goto out;
    }
    sb_kv_str(r->res, "root", root->pszValue);
    sb_kv_str(r->res, "version", CPLGetXMLValue(root, "version", ""));
    sb_kv_str(r->res, "title", CPLGetXMLValue(root,
              strcmp(o->service, "wfs") == 0 ? "ServiceIdentification.Title" :
                                               "Service.Title", ""));
    if (strstr(root->pszValue, "Exception") != NULL)
        oissue(&iss, "exception", "the service answered with %s: %s",
               root->pszValue, CPLGetXMLValue(root, "ServiceException",
               CPLGetXMLValue(root, "Exception.ExceptionText", "")));
    else if ((strcmp(o->service, "wfs") == 0 &&
              strcmp(root->pszValue, "WFS_Capabilities") != 0) ||
             (strcmp(o->service, "wms") == 0 &&
              strcmp(root->pszValue, "WMS_Capabilities") != 0 &&
              strcmp(root->pszValue, "WMT_MS_Capabilities") != 0))
        oissue(&iss, "root", "root element is %s", root->pszValue);
    if (strcmp(CPLGetXMLValue(root, "version", ""), o->version) != 0)
        oissue(&iss, "version", "asked for version %s, got '%s'", o->version,
               CPLGetXMLValue(root, "version", ""));
    memset(&c, 0, sizeof(c));
    c.iss = &iss;
    c.rep = r->rep;
    c.expect_crs = kv_get(r->opt, "expect_crs");
    c.wms13 = strcmp(o->version, "1.3.0") == 0;
    if (strcmp(o->service, "wfs") == 0)
        wfs_layers(&c, root);
    else
    {
        CPLXMLNode *cap = CPLGetXMLNode(root, "Capability");

        wms_layers(&c, cap != NULL ? cap->psChild : NULL, "");
        if (CPLGetXMLNode(root, "Capability.Request.GetMap") == NULL)
            oissue(&iss, "operation", "no GetMap operation");
    }
    if (c.layers == 0)
        oissue(&iss, "layers", "no named layer");
    if (expect_layer != NULL && r->rep->p != NULL)
    {
        char buf[1024];
        char *tok;
        char *save = NULL;

        snprintf(buf, sizeof(buf), "%s", expect_layer);
        for (tok = strtok_r(buf, ",", &save); tok != NULL;
             tok = strtok_r(NULL, ",", &save))
        {
            char needle[300];

            snprintf(needle, sizeof(needle), "layer\t%s\t", tok);
            if (strstr(r->rep->p, needle) == NULL)
                oissue(&iss, "layers", "layer %s is not advertised", tok);
        }
    }
    sb_kv_int(r->res, "layers", c.layers);
    CPLDestroyXMLNode(doc);
out:
    sb_kv_int(r->res, "issues", iss.n);
    sb_kv_int(r->res, "ok", iss.n == 0);
    return TA_GEO_OK;
}

/* ---- WMS GetMap ---------------------------------------------------- */

static int
ogc_map(geo_req *r, ogc_opts *o)
{
    const char *layer = kv_get(r->opt, "layer");
    const char *crs = kv_str(r->opt, "crs", "EPSG:4326");
    const char *bbox = kv_get(r->opt, "bbox");
    const char *fmt = kv_str(r->opt, "format", "image/png");
    long long w, h;
    sbuf u = SBUF_INIT;
    sbuf q = SBUF_INIT;
    http_res hr;
    ogc_iss iss = { r->rep, 0 };
    int rc;
    bool v13 = strcmp(o->version, "1.3.0") == 0;

    if (layer == NULL || bbox == NULL)
        return seterr(r->err, TA_GEO_EINVAL, "map needs layer and bbox");
    if ((rc = kv_long(r->opt, "width", 256, &w, r->err)) != 0 ||
        (rc = kv_long(r->opt, "height", 256, &h, r->err)) != 0)
        return rc;
    sb_printf(&q, "SERVICE=WMS&REQUEST=GetMap&VERSION=%s&LAYERS=%s&STYLES=&"
              "%s=%s&BBOX=%s&WIDTH=%lld&HEIGHT=%lld&FORMAT=%s", o->version,
              layer, v13 ? "CRS" : "SRS", crs, bbox, w, h, fmt);
    url_with(&u, r->text, q.p);
    sb_free(&q);
    rc = http_get(u.p, o->user, o->pass, NULL, o->timeout_ms,
                  (size_t)o->max_bytes, &hr, r->err);
    sb_kv_str(r->res, "url", u.p);
    sb_free(&u);
    if (rc != 0)
        return rc;
    sb_kv_int(r->res, "http_status", hr.status);
    sb_kv_str(r->res, "content_type", hr.ctype);
    sb_kv_int(r->res, "bytes", (long long)hr.body.len);
    if (hr.status != 200)
        oissue(&iss, "http_status", "GetMap answered %ld", hr.status);
    if (strstr(hr.ctype, "xml") != NULL)
    {
        oissue(&iss, "exception", "GetMap returned XML, not an image: %.200s",
               hr.body.p != NULL ? hr.body.p : "");
        sb_kv_int(r->res, "exception", 1);
    }
    else if (strncmp(hr.ctype, fmt, strlen(fmt)) != 0)
        oissue(&iss, "content_type", "asked for %s, got '%s'", fmt,
               hr.ctype);
    if (hr.body.len > 0 && strstr(hr.ctype, "xml") == NULL)
    {
        char vpath[64];
        VSILFILE *f;
        GDALDatasetH ds;

        snprintf(vpath, sizeof(vpath), "/vsimem/ta_geo_map_%p", (void *)&hr);
        f = VSIFileFromMemBuffer(vpath, (GByte *)hr.body.p,
                                 (vsi_l_offset)hr.body.len, FALSE);
        if (f != NULL)
            VSIFCloseL(f);
        ds = GDALOpenEx(vpath, GDAL_OF_RASTER, NULL, NULL, NULL);
        if (ds == NULL)
            oissue(&iss, "image", "the body does not decode as an image");
        else
        {
            int iw = GDALGetRasterXSize(ds), ih = GDALGetRasterYSize(ds);
            int nb = GDALGetRasterCount(ds);
            int bi;
            bool blank = true;

            sb_kv_int(r->res, "width", iw);
            sb_kv_int(r->res, "height", ih);
            sb_kv_int(r->res, "image_bands", nb);
            if (iw != w || ih != h)
                oissue(&iss, "image", "image is %dx%d, asked for %lldx%lld",
                       iw, ih, w, h);
            /* Blank: every band of one value (or alpha all zero). */
            for (bi = 1; bi <= nb && blank; bi++)
            {
                double mn, mx, mean, sd;
                GDALRasterBandH b = GDALGetRasterBand(ds, bi);

                if (GDALComputeRasterStatistics(b, FALSE, &mn, &mx, &mean,
                                                &sd, NULL, NULL) == CE_None)
                {
                    if (GDALGetRasterColorInterpretation(b) == GCI_AlphaBand)
                    {
                        if (mx == 0)
                            break;
                    }
                    else if (mn != mx)
                        blank = false;
                }
            }
            sb_kv_int(r->res, "blank", blank);
            GDALClose(ds);
        }
        VSIUnlink(vpath);
    }
    sb_free(&hr.body);
    sb_kv_int(r->res, "issues", iss.n);
    sb_kv_int(r->res, "ok", iss.n == 0);
    return TA_GEO_OK;
}

/* ---- features through GDAL ---------------------------------------- */

typedef struct feat_sum {
    long long features;
    long long invalid;
    long long empty;
    long long missing;
    long long dups;
    long long outside;
    uint64_t digest;
    double ext[4];
    char crs[256];
    char layer[256];
    double elapsed_us;
} feat_sum;

static int
ogc_read_features(geo_req *r, ogc_opts *o, long long page, feat_sum *s)
{
    GEOSContextHandle_t h = geo_geos();
    char **oo = NULL;
    const char *layer = kv_get(r->opt, "layer");
    long long maxf;
    sbuf src = SBUF_INIT;
    GDALDatasetH ds;
    OGRLayerH lay;
    OGRFeatureH f;
    OGREnvelope adv;
    bool have_adv;
    geo_hmap ids = { NULL, NULL, 0, 0 };
    char pg[32];
    uint64_t t0 = geo_now_ns();
    char userpwd[512];
    int rc = TA_GEO_OK;

    memset(s, 0, sizeof(*s));
    s->ext[0] = s->ext[1] = INFINITY;
    s->ext[2] = s->ext[3] = -INFINITY;
    if ((rc = kv_long(r->opt, "max_features", 100000, &maxf, r->err)) != 0)
        return rc;
    sb_printf(&src, "%s:%s", strcmp(o->service, "wfs") == 0 ? "WFS" : "OAPIF",
              r->text);
    if (page > 0)
    {
        snprintf(pg, sizeof(pg), "%lld", page);
        oo = CSLSetNameValue(oo, "PAGE_SIZE", pg);
        if (strcmp(o->service, "wfs") == 0)
            oo = CSLSetNameValue(oo, "PAGING_ALLOWED", "ON");
    }
    if (o->user != NULL)
    {
        snprintf(userpwd, sizeof(userpwd), "%s:%s", o->user,
                 o->pass != NULL ? o->pass : "");
        CPLSetThreadLocalConfigOption("GDAL_HTTP_USERPWD", userpwd);
        CPLSetThreadLocalConfigOption("GDAL_HTTP_AUTH", "BASIC");
    }
    {
        char tmo[32];

        snprintf(tmo, sizeof(tmo), "%lld", (o->timeout_ms + 999) / 1000);
        CPLSetThreadLocalConfigOption("GDAL_HTTP_TIMEOUT", tmo);
    }
    {
        const char *const drv[] = { "WFS", "OAPIF", NULL };

        ds = GDALOpenEx(src.p, GDAL_OF_VECTOR, drv, (const char *const *)oo,
                        NULL);
    }
    CSLDestroy(oo);
    sb_free(&src);
    if (ds == NULL)
    {
        rc = seterr(r->err, TA_GEO_EIO, "%s service at %s does not open: %s",
                    o->service, r->text, geo_gdal_err());
        goto out;
    }
    lay = layer != NULL ? GDALDatasetGetLayerByName(ds, layer) :
                          GDALDatasetGetLayer(ds, 0);
    if (lay == NULL)
    {
        GDALClose(ds);
        rc = seterr(r->err, TA_GEO_ENOENT, "no layer %s",
                    layer != NULL ? layer : "0");
        goto out;
    }
    snprintf(s->layer, sizeof(s->layer), "%s", OGR_L_GetName(lay));
    {
        OGRSpatialReferenceH sr = OGR_L_GetSpatialRef(lay);
        const char *a = sr != NULL ? OSRGetAuthorityName(sr, NULL) : NULL;
        const char *c = sr != NULL ? OSRGetAuthorityCode(sr, NULL) : NULL;

        if (a != NULL && c != NULL)
            snprintf(s->crs, sizeof(s->crs), "%s:%s", a, c);
    }
    have_adv = OGR_L_GetExtent(lay, &adv, 0) == OGRERR_NONE;
    OGR_L_ResetReading(lay);
    while ((f = OGR_L_GetNextFeature(lay)) != NULL)
    {
        OGRGeometryH og = OGR_F_GetGeometryRef(f);
        char id[64];
        uint64_t fh;

        s->features++;
        /* A feature's identity: its "id" when the service gives one,
         * else its FID, and its geometry. Clients number FIDs as they
         * read, so a page served twice shows only in the content. */
        {
            int idf = OGR_F_GetFieldIndex(f, "id");

            if (idf >= 0 && OGR_F_IsFieldSetAndNotNull(f, idf))
                snprintf(id, sizeof(id), "%s",
                         OGR_F_GetFieldAsString(f, idf));
            else
                snprintf(id, sizeof(id), "fid%lld",
                         (long long)OGR_F_GetFID(f));
        }
        fh = geo_hash(id, strlen(id));
        if (og == NULL)
            s->missing++;
        else
        {
            OGREnvelope e;
            size_t n = OGR_G_WkbSizeEx(og);
            unsigned char *b = malloc(n > 0 ? n : 1);

            if (OGR_G_IsEmpty(og))
                s->empty++;
            else
            {
                OGR_G_GetEnvelope(og, &e);
                s->ext[0] = fmin(s->ext[0], e.MinX);
                s->ext[1] = fmin(s->ext[1], e.MinY);
                s->ext[2] = fmax(s->ext[2], e.MaxX);
                s->ext[3] = fmax(s->ext[3], e.MaxY);
                if (have_adv &&
                    (e.MinX < adv.MinX - 1e-7 || e.MinY < adv.MinY - 1e-7 ||
                     e.MaxX > adv.MaxX + 1e-7 || e.MaxY > adv.MaxY + 1e-7))
                    s->outside++;
            }
            if (b != NULL && OGR_G_ExportToIsoWkb(og, wkbNDR, b) ==
                                 OGRERR_NONE)
            {
                GEOSWKBReader *rd = GEOSWKBReader_create_r(h);
                GEOSGeometry *g = GEOSWKBReader_read_r(h, rd, b, n);

                GEOSWKBReader_destroy_r(h, rd);
                if (g == NULL || (GEOSisEmpty_r(h, g) != 1 &&
                                  GEOSisValid_r(h, g) != 1))
                    s->invalid++;
                if (g != NULL)
                    GEOSGeom_destroy_r(h, g);
                /* Order-independent digest of (id, geometry). */
                fh ^= geo_hash(b, n);
            }
            free(b);
        }
        if (geo_hmap_put(&ids, fh, 1, NULL) == 0)
            s->dups++;
        s->digest += fh * 0x9e3779b97f4a7c15ULL;
        OGR_F_Destroy(f);
        if (s->features >= maxf || geo_deadline_passed())
            break;
    }
    GDALClose(ds);
out:
    CPLSetThreadLocalConfigOption("GDAL_HTTP_USERPWD", NULL);
    CPLSetThreadLocalConfigOption("GDAL_HTTP_AUTH", NULL);
    CPLSetThreadLocalConfigOption("GDAL_HTTP_TIMEOUT", NULL);
    geo_hmap_free(&ids);
    s->elapsed_us = geo_us_since(t0);
    return rc;
}

static void
put_feat(sbuf *res, const feat_sum *s, const char *pfx)
{
    char k[64];

#define K(_n) (snprintf(k, sizeof(k), "%s%s", pfx, _n), k)
    sb_kv_int(res, K("features"), s->features);
    sb_kv_int(res, K("invalid"), s->invalid);
    sb_kv_int(res, K("empty"), s->empty);
    sb_kv_int(res, K("missing_geometry"), s->missing);
    sb_kv_int(res, K("duplicates"), s->dups);
    sb_kv_int(res, K("outside_extent"), s->outside);
    sb_printf(res, "%s=%016llx\n", K("digest"),
              (unsigned long long)s->digest);
    sb_kv_num(res, K("elapsed_us"), s->elapsed_us);
#undef K
}

static int
ogc_features(geo_req *r, ogc_opts *o)
{
    feat_sum s;
    long long page;
    int rc;

    if (strcmp(o->service, "wms") == 0)
        return seterr(r->err, TA_GEO_EINVAL, "features come from wfs or "
                      "oapif");
    if ((rc = kv_long(r->opt, "page_size", 0, &page, r->err)) != 0)
        return rc;
    if ((rc = ogc_read_features(r, o, page, &s)) != 0)
        return rc;
    sb_kv_str(r->res, "layer", s.layer);
    sb_kv_str(r->res, "crs", s.crs);
    put_feat(r->res, &s, "");
    if (s.features > 0)
    {
        sb_kv_num(r->res, "minx", s.ext[0]);
        sb_kv_num(r->res, "miny", s.ext[1]);
        sb_kv_num(r->res, "maxx", s.ext[2]);
        sb_kv_num(r->res, "maxy", s.ext[3]);
    }
    sb_kv_int(r->res, "ok", s.invalid + s.dups + s.outside + s.missing == 0);
    return TA_GEO_OK;
}

static int
ogc_paging(geo_req *r, ogc_opts *o)
{
    const char *ps = kv_str(r->opt, "page_sizes", "7,1000");
    long long p1 = 0, p2 = 0;
    feat_sum a, b;
    int rc;

    if (sscanf(ps, "%lld,%lld", &p1, &p2) != 2 || p1 < 1 || p2 < 1)
        return seterr(r->err, TA_GEO_EINVAL, "page_sizes is 'a,b'");
    if ((rc = ogc_read_features(r, o, p1, &a)) != 0 ||
        (rc = ogc_read_features(r, o, p2, &b)) != 0)
        return rc;
    put_feat(r->res, &a, "a_");
    put_feat(r->res, &b, "b_");
    sb_kv_int(r->res, "consistent", a.features == b.features &&
              a.digest == b.digest && a.dups == 0 && b.dups == 0);
    return TA_GEO_OK;
}

/* ---- access control ------------------------------------------------- */

static int
ogc_auth(geo_req *r, ogc_opts *o)
{
    http_res a, g, bad;
    int rc;
    bool refused_anon, refused_bad;

    if (o->user == NULL)
        return seterr(r->err, TA_GEO_EINVAL, "auth needs user and password");
    if ((rc = http_get(r->text, NULL, NULL, NULL, o->timeout_ms, 1 << 20, &a,
                       r->err)) != 0)
        return rc;
    sb_free(&a.body);
    if ((rc = http_get(r->text, o->user, "tsf-geo-wrong-password", NULL,
                       o->timeout_ms, 1 << 20, &bad, r->err)) != 0)
        return rc;
    sb_free(&bad.body);
    if ((rc = http_get(r->text, o->user, o->pass, NULL, o->timeout_ms,
                       1 << 20, &g, r->err)) != 0)
        return rc;
    sb_free(&g.body);
    refused_anon = a.status == 401 || a.status == 403;
    refused_bad = bad.status == 401 || bad.status == 403;
    sb_kv_int(r->res, "status_anonymous", a.status);
    sb_kv_int(r->res, "status_wrong_password", bad.status);
    sb_kv_int(r->res, "status_authenticated", g.status);
    sb_kv_int(r->res, "anonymous_refused", refused_anon);
    sb_kv_int(r->res, "wrong_password_refused", refused_bad);
    sb_kv_int(r->res, "enforced", refused_anon && refused_bad &&
              g.status == 200);
    return TA_GEO_OK;
}

/* ---- OGC API - Features landing / conformance ---------------------- */

static int
ogc_conformance(geo_req *r, ogc_opts *o)
{
    static const char *const core =
        "http://www.opengis.net/spec/ogcapi-features-1/1.0/conf/core";
    static const char *const geojson =
        "http://www.opengis.net/spec/ogcapi-features-1/1.0/conf/geojson";
    sbuf u = SBUF_INIT;
    http_res land, conf, coll;
    ogc_iss iss = { r->rep, 0 };
    const char *base = r->text;
    size_t bl = strlen(base);
    int rc;

    while (bl > 0 && base[bl - 1] == '/')
        bl--;
    sb_put(&u, base, bl);
    sb_puts(&u, "/?f=json");
    rc = http_get(u.p, o->user, o->pass, "application/json", o->timeout_ms,
                  (size_t)o->max_bytes, &land, r->err);
    sb_free(&u);
    if (rc != 0)
        return rc;
    sb_put(&u, base, bl);
    sb_puts(&u, "/conformance?f=json");
    rc = http_get(u.p, o->user, o->pass, "application/json", o->timeout_ms,
                  (size_t)o->max_bytes, &conf, r->err);
    sb_free(&u);
    if (rc != 0)
    {
        sb_free(&land.body);
        return rc;
    }
    sb_put(&u, base, bl);
    sb_puts(&u, "/collections?f=json");
    rc = http_get(u.p, o->user, o->pass, "application/json", o->timeout_ms,
                  (size_t)o->max_bytes, &coll, r->err);
    sb_free(&u);
    if (rc != 0)
    {
        sb_free(&land.body);
        sb_free(&conf.body);
        return rc;
    }
    sb_kv_int(r->res, "landing_status", land.status);
    sb_kv_int(r->res, "conformance_status", conf.status);
    sb_kv_int(r->res, "collections_status", coll.status);
    if (land.status != 200)
        oissue(&iss, "http_status", "landing page answered %ld", land.status);
    if (conf.status != 200)
        oissue(&iss, "http_status", "conformance answered %ld", conf.status);
    if (coll.status != 200)
        oissue(&iss, "http_status", "collections answered %ld", coll.status);
    if (strstr(conf.ctype, "json") == NULL)
        oissue(&iss, "content_type", "conformance is '%s', not JSON",
               conf.ctype);
    sb_kv_int(r->res, "conforms_core", conf.body.p != NULL &&
              strstr(conf.body.p, core) != NULL);
    sb_kv_int(r->res, "conforms_geojson", conf.body.p != NULL &&
              strstr(conf.body.p, geojson) != NULL);
    if (conf.body.p == NULL || strstr(conf.body.p, core) == NULL)
        oissue(&iss, "conformance", "core conformance class not declared");
    if (land.body.p == NULL || strstr(land.body.p, "\"links\"") == NULL)
        oissue(&iss, "landing", "landing page has no links");
    if (coll.body.p == NULL || strstr(coll.body.p, "\"collections\"") == NULL)
        oissue(&iss, "collections", "no collections array");
    sb_free(&land.body);
    sb_free(&conf.body);
    sb_free(&coll.body);
    sb_kv_int(r->res, "issues", iss.n);
    sb_kv_int(r->res, "ok", iss.n == 0);
    return TA_GEO_OK;
}

int
geo_op_ogc(geo_req *r)
{
    ogc_opts o;
    int rc;

    if (*r->text == '\0')
        return seterr(r->err, TA_GEO_EINVAL, "no service URL");
    if ((rc = ogc_opts_read(r, &o)) != 0)
        return rc;
    if (strcmp(r->kind, "capabilities") == 0)
        return ogc_capabilities(r, &o);
    if (strcmp(r->kind, "map") == 0)
        return ogc_map(r, &o);
    if (strcmp(r->kind, "features") == 0)
        return ogc_features(r, &o);
    if (strcmp(r->kind, "paging") == 0)
        return ogc_paging(r, &o);
    if (strcmp(r->kind, "auth") == 0)
        return ogc_auth(r, &o);
    if (strcmp(r->kind, "conformance") == 0)
        return ogc_conformance(r, &o);
    return seterr(r->err, TA_GEO_EINVAL, "unknown OGC check '%s' "
                  "(capabilities, map, features, paging, auth, conformance)",
                  r->kind);
}
