/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: OGC services (WMS, WFS, OGC API - Features)
 *
 * Checks of what a service says and serves, made from the agent
 * (requests with libcurl, features through GDAL's own WFS and OAPIF
 * clients). Each check returns facts and a list of issues; a service
 * that answers 200 with nonsense has issues. The HTTP-level posture of
 * the same endpoint (headers, methods, TLS) is tsf-http's job.
 */

#ifndef __TAPI_GEO_OGC_H__
#define __TAPI_GEO_OGC_H__

#include "tapi_geo.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum tapi_geo_ogc_service {
    TAPI_GEO_OGC_WMS,
    TAPI_GEO_OGC_WFS,
    TAPI_GEO_OGC_OAPIF,     /**< OGC API - Features */
} tapi_geo_ogc_service;

/** How to reach the service. */
typedef struct tapi_geo_ogc_target {
    tapi_geo_ogc_service service;
    const char *url;        /**< endpoint (WMS/WFS) or landing page */
    const char *version;    /**< NULL: WMS 1.3.0, WFS 2.0.0 */
    const char *user;       /**< basic auth, or NULL */
    const char *password;
    unsigned int timeout_ms;    /**< 0 = 30 s */
} tapi_geo_ogc_target;

/** Issues and facts of a check. */
typedef struct tapi_geo_ogc_result {
    bool ok;                /**< no issue */
    long long issues;
    long http_status;
    char content_type[128];
    te_string records;      /**< "layer TAB name TAB crs TAB bbox" and
                                 "issue TAB category TAB detail" */
    tapi_geo_res raw;       /**< every fact the engine returned */
} tapi_geo_ogc_result;

extern void tapi_geo_ogc_result_free(tapi_geo_ogc_result *res);

/** Does the issue list carry @p category? */
extern bool tapi_geo_ogc_has_issue(const tapi_geo_ogc_result *res,
                                   const char *category);

/**
 * GetCapabilities: document validity, version, layers, CRS PROJ knows,
 * geographic bounding boxes on the Earth, GetMap present (WMS).
 * @p expect_layer / @p expect_crs: comma lists that must be there.
 */
extern te_errno tapi_geo_ogc_capabilities(rcf_rpc_server *rpcs,
                                          const tapi_geo_ogc_target *t,
                                          const char *expect_layer,
                                          const char *expect_crs,
                                          tapi_geo_ogc_result *res);

/** WMS GetMap: an image of the size and format asked for, not blank. */
extern te_errno tapi_geo_ogc_map(rcf_rpc_server *rpcs,
                                 const tapi_geo_ogc_target *t,
                                 const char *layer, const char *crs,
                                 const char *bbox, unsigned int width,
                                 unsigned int height,
                                 tapi_geo_ogc_result *res);

/** Features of a WFS / OAPIF layer, checked as they stream. */
typedef struct tapi_geo_ogc_features {
    long long features;
    long long invalid;
    long long empty;
    long long missing_geometry;
    long long duplicates;
    long long outside_extent;   /**< beyond the advertised extent */
    char digest[20];            /**< order-independent */
    char crs[64];
    tapi_geo_bbox extent;
    double elapsed_us;
    bool ok;
} tapi_geo_ogc_features;

/** @p page_size: 0 = the client's default. */
extern te_errno tapi_geo_ogc_features_get(rcf_rpc_server *rpcs,
                                          const tapi_geo_ogc_target *t,
                                          const char *layer,
                                          unsigned int page_size,
                                          unsigned int max_features,
                                          tapi_geo_ogc_features *f);

/** The same features whatever the page size? */
extern te_errno tapi_geo_ogc_paging(rcf_rpc_server *rpcs,
                                    const tapi_geo_ogc_target *t,
                                    const char *layer, unsigned int page_a,
                                    unsigned int page_b, bool *consistent,
                                    tapi_geo_ogc_features *a,
                                    tapi_geo_ogc_features *b);

/** Access control at @p url: anonymous, wrong password, right one. */
typedef struct tapi_geo_ogc_auth {
    long status_anonymous;
    long status_wrong_password;
    long status_authenticated;
    bool enforced;
} tapi_geo_ogc_auth;

extern te_errno tapi_geo_ogc_access(rcf_rpc_server *rpcs, const char *url,
                                    const char *user, const char *password,
                                    tapi_geo_ogc_auth *auth);

/** OGC API - Features landing page, conformance, collections. */
extern te_errno tapi_geo_ogc_conformance(rcf_rpc_server *rpcs,
                                         const tapi_geo_ogc_target *t,
                                         bool *core, bool *geojson,
                                         tapi_geo_ogc_result *res);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_GEO_OGC_H__ */
