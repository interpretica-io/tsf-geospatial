/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial engine: dispatch, objects, library contexts
 *
 * The one entry point (ta_geo_call), the registry of objects the
 * caller holds handles to, the GEOS/PROJ/GDAL contexts with their error
 * capture, the limits and the deadline of the current call, and the
 * operations that are about the engine itself: available, config,
 * stats, release.
 *
 * Calls are serialised by one mutex: the RPC server may run several
 * threads, and GEOS's interrupt and GDAL's error handler are global.
 */

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "ta_geo_int.h"

#ifdef TA_GEO_HAVE_CURL
#include <curl/curl.h>
#endif

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- limits ------------------------------------------------------ */

/* Defaults are generous for a lab machine and finite for every input. */
geo_limits geo_lim = {
    .max_vertices = 10000000,
    .max_depth = 64,
    .max_features = 100000000,
    .max_input = 256 * 1024 * 1024,
    .max_report = 100000,
    .max_handles = 4095,
    .timeout_ms = 0,
    .max_pixels = 64 * 1024 * 1024,
};

static int
limits_read(kv *k, geo_limits *l, char **err)
{
    int rc;

#define LIM(_n) \
    do {                                                            \
        if ((rc = kv_long(k, #_n, l->_n, &l->_n, err)) != 0)        \
            return rc;                                              \
        if (l->_n < 0)                                              \
            return seterr(err, TA_GEO_EINVAL, "%s < 0", #_n);       \
    } while (0)
    LIM(max_vertices);
    LIM(max_depth);
    LIM(max_features);
    LIM(max_input);
    LIM(max_report);
    LIM(max_handles);
    LIM(timeout_ms);
    LIM(max_pixels);
#undef LIM
    if (l->max_handles > 0xfff)
        return seterr(err, TA_GEO_EINVAL, "max_handles > 4095");
    return TA_GEO_OK;
}

int
geo_limits_override(kv *k, geo_limits *l, char **err)
{
    *l = geo_lim;
    return limits_read(k, l, err);
}

uint64_t
geo_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t deadline_ns;

void
geo_deadline_arm(long long timeout_ms)
{
    deadline_ns = timeout_ms > 0 ?
                  geo_now_ns() + (uint64_t)timeout_ms * 1000000ULL : 0;
}

bool
geo_deadline_passed(void)
{
    return deadline_ns != 0 && geo_now_ns() > deadline_ns;
}

/* ---- library contexts -------------------------------------------- */

static GEOSContextHandle_t geos_ctx;
static char geos_msg[1024];
static PJ_CONTEXT *proj_ctx;
static char gdal_msg[1024];
static bool gdal_ready;

static void
geos_err_handler(const char *msg, void *data)
{
    (void)data;
    if (geos_msg[0] == '\0')
        snprintf(geos_msg, sizeof(geos_msg), "%s", msg);
}

static void
geos_notice_handler(const char *msg, void *data)
{
    (void)msg;
    (void)data;
}

static void
geos_interrupt_cb(void)
{
    if (geo_deadline_passed())
        GEOS_interruptRequest();
}

GEOSContextHandle_t
geo_geos(void)
{
    if (geos_ctx == NULL)
    {
        geos_ctx = GEOS_init_r();
        GEOSContext_setErrorMessageHandler_r(geos_ctx, geos_err_handler,
                                             NULL);
        GEOSContext_setNoticeMessageHandler_r(geos_ctx, geos_notice_handler,
                                              NULL);
        GEOS_interruptRegisterCallback(geos_interrupt_cb);
    }
    return geos_ctx;
}

const char *
geo_geos_err(void)
{
    return geos_msg;
}

void
geo_geos_clear(void)
{
    geos_msg[0] = '\0';
}

PJ_CONTEXT *
geo_proj(void)
{
    if (proj_ctx == NULL)
    {
        proj_ctx = proj_context_create();
        /* Grids come from the local data directory only, unless a test
         * asks for the network on purpose (config proj_network=1): a
         * transformation must not change because the CDN answered. */
        proj_context_set_enable_network(proj_ctx, 0);
        proj_log_level(proj_ctx, PJ_LOG_NONE);
    }
    return proj_ctx;
}

const char *
geo_proj_err(void)
{
    int e = proj_context_errno(geo_proj());

    return e == 0 ? "" : proj_context_errno_string(geo_proj(), e);
}

static void CPL_STDCALL
gdal_err_handler(CPLErr cls, CPLErrorNum num, const char *msg)
{
    (void)num;
    if (cls >= CE_Failure && gdal_msg[0] == '\0')
        snprintf(gdal_msg, sizeof(gdal_msg), "%s", msg);
}

void
geo_gdal_init(void)
{
    if (gdal_ready)
        return;
    GDALAllRegister();
    CPLSetErrorHandler(gdal_err_handler);
    /* Untrusted inputs: no embedded Python in VRT, no directory scans
     * on open, no prompts to the network for sidecar files. */
    CPLSetConfigOption("GDAL_VRT_ENABLE_PYTHON", "NO");
    CPLSetConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "EMPTY_DIR");
    CPLSetConfigOption("GDAL_HTTP_TIMEOUT", "30");
    CPLSetConfigOption("CPL_VSIL_CURL_ALLOWED_EXTENSIONS", "NONE");
    CPLSetConfigOption("OGR_GEOJSON_MAX_OBJ_SIZE", "200");
    gdal_ready = true;
}

const char *
geo_gdal_err(void)
{
    return gdal_msg;
}

void
geo_gdal_clear(void)
{
    gdal_msg[0] = '\0';
    CPLErrorReset();
}

/* ---- objects ------------------------------------------------------ */

#define OBJ_SLOTS   4096
#define OBJ_IDX(h)  ((h) & 0xfff)
#define OBJ_GEN(h)  ((h) >> 12)

typedef struct obj_slot {
    geo_otype type;
    unsigned int gen;
    void *obj;
} obj_slot;

static obj_slot slots[OBJ_SLOTS];
static unsigned int n_live;
static unsigned long long n_ops;
static unsigned long long n_created;
static unsigned long long n_released;

static const char *
otype_name(geo_otype t)
{
    switch (t)
    {
        case GEO_O_GEOM: return "geometry";
        case GEO_O_DS: return "dataset";
        case GEO_O_TRACK: return "track";
        default: return "nothing";
    }
}

/*
 * Generations start at random values per process, so that a handle
 * issued by an RPC server that has since been restarted is refused
 * by the new one (except by a 1 in 2^18 chance) instead of naming
 * whatever the new process put in the same slot.
 */
static bool gens_seeded;
static unsigned int session_tag;

static void
gens_seed(void)
{
    geo_rng r;
    struct timespec ts;
    unsigned int i;

    clock_gettime(CLOCK_REALTIME, &ts);
    geo_rng_seed(&r, (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 20) ^
                     ((uint64_t)getpid() << 40));
    for (i = 0; i < OBJ_SLOTS; i++)
        slots[i].gen = (unsigned int)(geo_rng_next(&r) & 0x3ffff);
    session_tag = (unsigned int)(geo_rng_next(&r) & 0x7fffffff);
    gens_seeded = true;
}

int
geo_obj_add(geo_otype type, void *obj, int *handle, char **err)
{
    unsigned int i;

    if (!gens_seeded)
        gens_seed();

    if (n_live >= (unsigned int)geo_lim.max_handles)
        return seterr(err, TA_GEO_E2BIG, "too many objects (max_handles=%lld)",
                      geo_lim.max_handles);
    for (i = 1; i < OBJ_SLOTS && slots[i].type != GEO_O_FREE; i++)
        ;
    if (i >= OBJ_SLOTS)
        return seterr(err, TA_GEO_E2BIG, "no free object slot");
    slots[i].type = type;
    slots[i].gen = (slots[i].gen + 1) & 0x3ffff;
    if (slots[i].gen == 0)
        slots[i].gen = 1;
    slots[i].obj = obj;
    n_live++;
    n_created++;
    *handle = (int)((slots[i].gen << 12) | i);
    return TA_GEO_OK;
}

void *
geo_obj_get(int handle, geo_otype type, char **err)
{
    unsigned int i = OBJ_IDX((unsigned int)handle);
    unsigned int gen = OBJ_GEN((unsigned int)handle);

    if (handle <= 0 || i == 0 || slots[i].type == GEO_O_FREE ||
        slots[i].gen != gen)
    {
        seterr(err, TA_GEO_EINVAL, "no such %s handle %d (never made, or "
               "released)", otype_name(type), handle);
        return NULL;
    }
    if (slots[i].type != type)
    {
        seterr(err, TA_GEO_EINVAL, "handle %d is a %s, not a %s", handle,
               otype_name(slots[i].type), otype_name(type));
        return NULL;
    }
    return slots[i].obj;
}

void
geo_geom_free(geo_geom *g)
{
    if (g == NULL)
        return;
    if (g->g != NULL)
        GEOSGeom_destroy_r(geo_geos(), g->g);
    free(g);
}

void
geo_ds_free(geo_ds *d)
{
    if (d == NULL)
        return;
    if (d->ds != NULL)
        GDALClose(d->ds);
    free(d->path);
    free(d);
}

void
geo_track_free(geo_track *t)
{
    if (t == NULL)
        return;
    free(t->t);
    free(t->x);
    free(t->y);
    free(t->z);
    free(t);
}

static void
slot_free(unsigned int i)
{
    switch (slots[i].type)
    {
        case GEO_O_GEOM: geo_geom_free(slots[i].obj); break;
        case GEO_O_DS: geo_ds_free(slots[i].obj); break;
        case GEO_O_TRACK: geo_track_free(slots[i].obj); break;
        default: break;
    }
    slots[i].type = GEO_O_FREE;
    slots[i].obj = NULL;
    n_live--;
    n_released++;
}

int
geo_obj_release(int handle, char **err)
{
    unsigned int i = OBJ_IDX((unsigned int)handle);

    if (handle <= 0 || i == 0 || slots[i].type == GEO_O_FREE ||
        slots[i].gen != OBJ_GEN((unsigned int)handle))
        return seterr(err, TA_GEO_EINVAL, "no such handle %d (never made, "
                      "or released already)", handle);
    slot_free(i);
    return TA_GEO_OK;
}

void
geo_obj_counts(unsigned int counts[4])
{
    unsigned int i;

    memset(counts, 0, 4 * sizeof(*counts));
    for (i = 1; i < OBJ_SLOTS; i++)
        counts[slots[i].type]++;
}

void
geo_obj_release_all(void)
{
    unsigned int i;

    for (i = 1; i < OBJ_SLOTS; i++)
    {
        if (slots[i].type != GEO_O_FREE)
            slot_free(i);
    }
}

/* ---- helpers ------------------------------------------------------ */

int
geo_geom_new(GEOSGeometry *g, const char *crs, int *handle, char **err)
{
    geo_geom *o = calloc(1, sizeof(*o));
    int rc;

    if (o == NULL)
    {
        GEOSGeom_destroy_r(geo_geos(), g);
        return seterr(err, TA_GEO_ENOMEM, "out of memory");
    }
    o->g = g;
    snprintf(o->crs, sizeof(o->crs), "%s", crs != NULL ? crs : "");
    rc = geo_obj_add(GEO_O_GEOM, o, handle, err);
    if (rc != 0)
        geo_geom_free(o);
    return rc;
}

const char *
geo_type_name(int t)
{
    switch (t)
    {
        case GEOS_POINT: return "Point";
        case GEOS_LINESTRING: return "LineString";
        case GEOS_LINEARRING: return "LinearRing";
        case GEOS_POLYGON: return "Polygon";
        case GEOS_MULTIPOINT: return "MultiPoint";
        case GEOS_MULTILINESTRING: return "MultiLineString";
        case GEOS_MULTIPOLYGON: return "MultiPolygon";
        case GEOS_GEOMETRYCOLLECTION: return "GeometryCollection";
        default: return "Unknown";
    }
}

const char *
geo_dims_name(const GEOSGeometry *g)
{
    GEOSContextHandle_t h = geo_geos();
    bool z = GEOSHasZ_r(h, g) == 1;
    bool m = false;

#if GEOS_AT_LEAST(3, 12)
    m = GEOSHasM_r(h, g) == 1;
#endif
    return z ? (m ? "xyzm" : "xyz") : (m ? "xym" : "xy");
}

char *
geo_wkt(const GEOSGeometry *g, int prec)
{
    GEOSContextHandle_t h = geo_geos();
    GEOSWKTWriter *w = GEOSWKTWriter_create_r(h);
    char *s;
    char *out;

    GEOSWKTWriter_setOutputDimension_r(h, w, 4);
    if (prec >= 0)
    {
        GEOSWKTWriter_setTrim_r(h, w, 1);
        GEOSWKTWriter_setRoundingPrecision_r(h, w, prec);
    }
    else
    {
        /* Trim + no rounding: the shortest text that reads back to the
         * same doubles. */
        GEOSWKTWriter_setTrim_r(h, w, 1);
        GEOSWKTWriter_setRoundingPrecision_r(h, w, -1);
    }
    s = GEOSWKTWriter_write_r(h, w, g);
    GEOSWKTWriter_destroy_r(h, w);
    if (s == NULL)
        return NULL;
    out = strdup(s);
    GEOSFree_r(h, s);
    return out;
}

long long
geo_npoints(const GEOSGeometry *g)
{
    int n = GEOSGetNumCoordinates_r(geo_geos(), g);

    return n < 0 ? 0 : n;
}

bool
geo_bounds(const GEOSGeometry *g, double b[4])
{
    GEOSContextHandle_t h = geo_geos();

    if (GEOSisEmpty_r(h, g) == 1)
        return false;
    return GEOSGeom_getXMin_r(h, g, &b[0]) == 1 &&
           GEOSGeom_getYMin_r(h, g, &b[1]) == 1 &&
           GEOSGeom_getXMax_r(h, g, &b[2]) == 1 &&
           GEOSGeom_getYMax_r(h, g, &b[3]) == 1;
}

void
geo_crs_norm(const char *in, char *out, size_t len)
{
    const char *p;

    if (in == NULL || *in == '\0')
    {
        snprintf(out, len, "%s", "");
        return;
    }
    for (p = in; *p >= '0' && *p <= '9'; p++)
        ;
    if (*p == '\0')
        snprintf(out, len, "EPSG:%s", in);
    else
        snprintf(out, len, "%s", in);
}

PJ *
geo_crs_create(const char *def, char **err)
{
    char norm[4096];
    PJ *crs;

    if (def == NULL || *def == '\0')
    {
        seterr(err, TA_GEO_EINVAL, "no CRS given");
        return NULL;
    }
    geo_crs_norm(def, norm, sizeof(norm));
    proj_context_errno(geo_proj());
    crs = proj_create(geo_proj(), norm);
    if (crs == NULL)
    {
        seterr(err, TA_GEO_ENOENT, "CRS '%s' not recognised: %s", norm,
               geo_proj_err());
        return NULL;
    }
    if (!proj_is_crs(crs))
    {
        proj_destroy(crs);
        seterr(err, TA_GEO_EINVAL, "'%s' is not a coordinate reference "
               "system", norm);
        return NULL;
    }
    return crs;
}

bool
geo_crs_is_geographic(PJ *crs)
{
    PJ_TYPE t = proj_get_type(crs);

    return t == PJ_TYPE_GEOGRAPHIC_2D_CRS || t == PJ_TYPE_GEOGRAPHIC_3D_CRS;
}

/* The coordinate system of a CRS (the horizontal part of a compound). */
static PJ *
crs_cs(PJ *crs)
{
    PJ_CONTEXT *c = geo_proj();
    PJ *h = NULL;
    PJ *cs;

    if (proj_get_type(crs) == PJ_TYPE_COMPOUND_CRS)
    {
        h = proj_crs_get_sub_crs(c, crs, 0);
        cs = h != NULL ? proj_crs_get_coordinate_system(c, h) : NULL;
        proj_destroy(h);
        return cs;
    }
    return proj_crs_get_coordinate_system(c, crs);
}

void
geo_crs_unit(PJ *crs, char *out, size_t len)
{
    PJ *cs = crs_cs(crs);
    const char *unit = NULL;

    snprintf(out, len, "%s", "unknown");
    if (cs == NULL)
        return;
    if (proj_cs_get_axis_info(geo_proj(), cs, 0, NULL, NULL, NULL, NULL,
                              &unit, NULL, NULL) && unit != NULL)
        snprintf(out, len, "%s", unit);
    proj_destroy(cs);
}

bool
geo_crs_lat_first(PJ *crs)
{
    PJ *cs = crs_cs(crs);
    const char *dir = NULL;
    bool lat = false;

    if (cs == NULL)
        return false;
    if (proj_cs_get_axis_info(geo_proj(), cs, 0, NULL, NULL, &dir, NULL,
                              NULL, NULL, NULL) && dir != NULL)
        lat = strcmp(dir, "north") == 0 || strcmp(dir, "south") == 0;
    proj_destroy(cs);
    return lat;
}

/* ---- operations about the engine ---------------------------------- */

static int
op_available(geo_req *r)
{
    unsigned int f = TA_GEO_FEAT_GEOS | TA_GEO_FEAT_PROJ | TA_GEO_FEAT_GDAL;
    PJ_INFO pi = proj_info();
    int i;
    int n;

    geo_gdal_init();
#if GEOS_AT_LEAST(3, 12)
    f |= TA_GEO_FEAT_M;
#endif
    f |= TA_GEO_FEAT_GEOJSON;   /* through OGR */
    if (proj_context_is_network_enabled(geo_proj()))
        f |= TA_GEO_FEAT_NETWORK;
    if (GDALGetDriverByName("WFS") != NULL ||
        GDALGetDriverByName("OAPIF") != NULL)
        f |= TA_GEO_FEAT_HTTP;
#ifdef TA_GEO_HAVE_CURL
    f |= TA_GEO_FEAT_CURL;
#endif
    *r->features = f;
    sb_kv_str(r->res, "geos", GEOSversion());
    sb_kv_str(r->res, "proj", pi.release);
    sb_kv_str(r->res, "proj_searchpath", pi.searchpath);
    sb_kv_str(r->res, "gdal", GDALVersionInfo("RELEASE_NAME"));
#ifdef TA_GEO_HAVE_CURL
    sb_kv_str(r->res, "curl", curl_version_info(CURLVERSION_NOW)->version);
#endif
    sb_kv_int(r->res, "m", (f & TA_GEO_FEAT_M) != 0);
    sb_kv_int(r->res, "geojson", (f & TA_GEO_FEAT_GEOJSON) != 0);
    sb_kv_int(r->res, "http", (f & TA_GEO_FEAT_HTTP) != 0);
    sb_kv_int(r->res, "network", (f & TA_GEO_FEAT_NETWORK) != 0);

    /* Drivers: short name, long name, vector, raster, create, update. */
    n = GDALGetDriverCount();
    sb_kv_int(r->res, "drivers", n);
    for (i = 0; i < n; i++)
    {
        GDALDriverH d = GDALGetDriver(i);
        const char *vec = GDALGetMetadataItem(d, GDAL_DCAP_VECTOR, NULL);
        const char *ras = GDALGetMetadataItem(d, GDAL_DCAP_RASTER, NULL);
        const char *cre = GDALGetMetadataItem(d, GDAL_DCAP_CREATE, NULL);
        const char *cpy = GDALGetMetadataItem(d, GDAL_DCAP_CREATECOPY, NULL);

        sb_field(r->rep, GDALGetDriverShortName(d));
        sb_puts(r->rep, "\t");
        sb_field(r->rep, GDALGetDriverLongName(d));
        sb_printf(r->rep, "\t%d\t%d\t%d\t%d\n", vec != NULL, ras != NULL,
                  cre != NULL, cpy != NULL);
    }
    return TA_GEO_OK;
}

static int
op_config(geo_req *r)
{
    geo_limits l = geo_lim;
    int rc;
    bool net;
    const char *v;

    if ((rc = limits_read(r->opt, &l, r->err)) != 0)
        return rc;
    v = kv_get(r->opt, "proj_network");
    if (v != NULL)
    {
        if ((rc = kv_bool(r->opt, "proj_network", false, &net, r->err)) != 0)
            return rc;
        proj_context_set_enable_network(geo_proj(), net);
    }
    v = kv_get(r->opt, "proj_data");
    if (v != NULL)
    {
        const char *paths[1] = { v };

        proj_context_set_search_paths(geo_proj(), *v != '\0' ? 1 : 0,
                                      *v != '\0' ? paths : NULL);
    }
    if ((rc = kv_unused(r->opt, r->err)) != 0)
        return rc;
    geo_lim = l;
    sb_kv_int(r->res, "max_vertices", l.max_vertices);
    sb_kv_int(r->res, "max_depth", l.max_depth);
    sb_kv_int(r->res, "max_features", l.max_features);
    sb_kv_int(r->res, "max_input", l.max_input);
    sb_kv_int(r->res, "max_report", l.max_report);
    sb_kv_int(r->res, "max_handles", l.max_handles);
    sb_kv_int(r->res, "timeout_ms", l.timeout_ms);
    sb_kv_int(r->res, "max_pixels", l.max_pixels);
    sb_kv_int(r->res, "proj_network",
              proj_context_is_network_enabled(geo_proj()));
    return TA_GEO_OK;
}

static int
op_stats(geo_req *r)
{
    struct rusage ru;
    long rss_pages = 0;
    long size_pages = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    long kb = sysconf(_SC_PAGESIZE) / 1024;
    unsigned int counts[4];

    if (f != NULL)
    {
        if (fscanf(f, "%ld %ld", &size_pages, &rss_pages) != 2)
            rss_pages = size_pages = 0;
        fclose(f);
    }
    getrusage(RUSAGE_SELF, &ru);
    geo_obj_counts(counts);
    if (!gens_seeded)
        gens_seed();
    sb_kv_int(r->res, "session", session_tag);
    sb_kv_int(r->res, "live", n_live);
    sb_kv_int(r->res, "geometries", counts[GEO_O_GEOM]);
    sb_kv_int(r->res, "datasets", counts[GEO_O_DS]);
    sb_kv_int(r->res, "tracks", counts[GEO_O_TRACK]);
    sb_kv_int(r->res, "created", (long long)n_created);
    sb_kv_int(r->res, "released", (long long)n_released);
    sb_kv_int(r->res, "ops", (long long)n_ops);
    sb_kv_int(r->res, "rss_kb", rss_pages * kb);
    sb_kv_int(r->res, "vsz_kb", size_pages * kb);
    sb_kv_int(r->res, "maxrss_kb", ru.ru_maxrss);
    sb_kv_int(r->res, "gdal_open_datasets", counts[GEO_O_DS]);
    return TA_GEO_OK;
}

static int
op_release(geo_req *r)
{
    int rc = geo_obj_release(r->ha, r->err);

    if (rc == 0 && r->hb != 0)
        rc = geo_obj_release(r->hb, r->err);
    return rc;
}

/* ---- dispatch ----------------------------------------------------- */

static const char *const op_names[TA_GEO_OP__COUNT] = {
    [TA_GEO_OP_AVAILABLE] = "available",
    [TA_GEO_OP_CONFIG] = "config",
    [TA_GEO_OP_STATS] = "stats",
    [TA_GEO_OP_RELEASE] = "release",
    [TA_GEO_OP_GEOM_PARSE] = "geom_parse",
    [TA_GEO_OP_GEOM_INFO] = "geom_info",
    [TA_GEO_OP_GEOM_SERIALIZE] = "geom_serialize",
    [TA_GEO_OP_GEOM_DERIVE] = "geom_derive",
    [TA_GEO_OP_GEOM_CHECK] = "geom_check",
    [TA_GEO_OP_GEOM_REPAIR] = "geom_repair",
    [TA_GEO_OP_GEOM_PREDICATE] = "geom_predicate",
    [TA_GEO_OP_GEOM_CONSTRUCT] = "geom_construct",
    [TA_GEO_OP_GEOM_MEASURE] = "geom_measure",
    [TA_GEO_OP_CRS_INFO] = "crs_info",
    [TA_GEO_OP_TRANSFORM] = "transform",
    [TA_GEO_OP_TRANSFORM_COORDS] = "transform_coords",
    [TA_GEO_OP_TRANSFORM_BOUNDS] = "transform_bounds",
    [TA_GEO_OP_DS_OPEN] = "ds_open",
    [TA_GEO_OP_DS_INFO] = "ds_info",
    [TA_GEO_OP_DS_FETCH] = "ds_fetch",
    [TA_GEO_OP_DS_VALIDATE] = "ds_validate",
    [TA_GEO_OP_DS_COMPARE] = "ds_compare",
    [TA_GEO_OP_DS_WRITE] = "ds_write",
    [TA_GEO_OP_RASTER_INFO] = "raster_info",
    [TA_GEO_OP_RASTER_READ] = "raster_read",
    [TA_GEO_OP_RASTER_STATS] = "raster_stats",
    [TA_GEO_OP_RASTER_COMPARE] = "raster_compare",
    [TA_GEO_OP_RASTER_WRITE] = "raster_write",
    [TA_GEO_OP_TRACK_LOAD] = "track_load",
    [TA_GEO_OP_TRACK_EVAL] = "track_eval",
    [TA_GEO_OP_GEN] = "gen",
    [TA_GEO_OP_OGC] = "ogc",
    [TA_GEO_OP_PERF] = "perf",
};

const char *
ta_geo_op_name(ta_geo_op op)
{
    return (unsigned int)op < TA_GEO_OP__COUNT ? op_names[op] : NULL;
}

const char *
ta_geo_strerror(int rc)
{
    static const char *const n[] = {
        "OK", "EINVAL", "ENOENT", "EIO", "ECORRUPT", "E2BIG", "ENOMEM",
        "ENOTSUP", "ETIMEDOUT", "EDOM", "EACCES", "EFAIL",
    };

    return rc >= 0 && rc <= TA_GEO_EFAIL ? n[rc] : "E?";
}

static int
dispatch(ta_geo_op op, geo_req *r)
{
    switch (op)
    {
        case TA_GEO_OP_AVAILABLE: return op_available(r);
        case TA_GEO_OP_CONFIG: return op_config(r);
        case TA_GEO_OP_STATS: return op_stats(r);
        case TA_GEO_OP_RELEASE: return op_release(r);

        case TA_GEO_OP_GEOM_PARSE:
        case TA_GEO_OP_GEOM_INFO:
        case TA_GEO_OP_GEOM_SERIALIZE:
        case TA_GEO_OP_GEOM_DERIVE:
        case TA_GEO_OP_GEOM_CHECK:
        case TA_GEO_OP_GEOM_REPAIR:
        case TA_GEO_OP_GEOM_PREDICATE:
        case TA_GEO_OP_GEOM_CONSTRUCT:
        case TA_GEO_OP_GEOM_MEASURE:
            return geo_op_geom(op, r);

        case TA_GEO_OP_CRS_INFO:
        case TA_GEO_OP_TRANSFORM:
        case TA_GEO_OP_TRANSFORM_COORDS:
        case TA_GEO_OP_TRANSFORM_BOUNDS:
            return geo_op_crs(op, r);

        case TA_GEO_OP_DS_OPEN:
        case TA_GEO_OP_DS_INFO:
        case TA_GEO_OP_DS_FETCH:
        case TA_GEO_OP_DS_VALIDATE:
        case TA_GEO_OP_DS_COMPARE:
        case TA_GEO_OP_DS_WRITE:
            geo_gdal_init();
            return geo_op_ds(op, r);

        case TA_GEO_OP_RASTER_INFO:
        case TA_GEO_OP_RASTER_READ:
        case TA_GEO_OP_RASTER_STATS:
        case TA_GEO_OP_RASTER_COMPARE:
        case TA_GEO_OP_RASTER_WRITE:
            geo_gdal_init();
            return geo_op_raster(op, r);

        case TA_GEO_OP_TRACK_LOAD:
        case TA_GEO_OP_TRACK_EVAL:
            geo_gdal_init();
            return geo_op_track(op, r);

        case TA_GEO_OP_GEN: return geo_op_gen(r);
        case TA_GEO_OP_OGC:
            geo_gdal_init();
            return geo_op_ogc(r);
        case TA_GEO_OP_PERF: return geo_op_perf(r);

        default:
            return seterr(r->err, TA_GEO_EINVAL, "unknown operation %d", op);
    }
}

/* See description in ta_geo_core.h */
int
ta_geo_call(ta_geo_op op, int ha, int hb, const char *kind, const char *text,
            const char *params, int *handle, unsigned int *features,
            char **result, char **report, char **errmsg)
{
    sbuf res = SBUF_INIT;
    sbuf rep = SBUF_INIT;
    char *err = NULL;
    kv opt = { NULL, 0 };
    int dummy_h = 0;
    unsigned int dummy_f = 0;
    geo_req r;
    int rc;
    size_t tlen = text != NULL ? strlen(text) : 0;

    pthread_mutex_lock(&lock);
    n_ops++;
    geo_geos();
    geo_geos_clear();
    geo_gdal_clear();
    proj_context_errno(geo_proj());

    rc = kv_parse(params, &opt, &err);
    if (rc == 0 && (long long)tlen > geo_lim.max_input)
        rc = seterr(&err, TA_GEO_E2BIG, "input of %zu bytes over max_input "
                    "(%lld)", tlen, geo_lim.max_input);
    if (rc == 0)
    {
        long long tmo = geo_lim.timeout_ms;

        rc = kv_long(&opt, "timeout_ms", geo_lim.timeout_ms, &tmo, &err);
        geo_deadline_arm(tmo);
    }
    if (rc == 0)
    {
        r.ha = ha;
        r.hb = hb;
        r.kind = kind != NULL ? kind : "";
        r.text = text != NULL ? text : "";
        r.opt = &opt;
        r.handle = handle != NULL ? handle : &dummy_h;
        r.features = features != NULL ? features : &dummy_f;
        r.res = &res;
        r.rep = &rep;
        r.err = &err;
        rc = dispatch(op, &r);
        if (rc == 0)
            rc = kv_unused(&opt, &err);
        if (rc == 0 && (res.oom || rep.oom))
            rc = seterr(&err, TA_GEO_ENOMEM, "out of memory building the "
                        "result");
        if (rc != 0 && op != TA_GEO_OP_RELEASE && op != TA_GEO_OP_CONFIG &&
            handle != NULL && *handle != 0)
        {
            /* An operation that failed after making its object (an
             * unknown option noticed at the end) does not leak it. */
            char *e2 = NULL;

            geo_obj_release(*handle, &e2);
            free(e2);
            *handle = 0;
        }
    }
    geo_deadline_arm(0);
    kv_free(&opt);
    pthread_mutex_unlock(&lock);

    if (rc != 0 && err == NULL)
        err = strdup(ta_geo_strerror(rc));
    *result = sb_take(&res);
    *report = sb_take(&rep);
    *errmsg = err != NULL ? err : strdup("");
    return rc;
}

void
ta_geo_shutdown(void)
{
    pthread_mutex_lock(&lock);
    geo_obj_release_all();
    if (geos_ctx != NULL)
    {
        GEOS_interruptRegisterCallback(NULL);
        GEOS_finish_r(geos_ctx);
        geos_ctx = NULL;
    }
    if (proj_ctx != NULL)
    {
        proj_context_destroy(proj_ctx);
        proj_ctx = NULL;
    }
    if (gdal_ready)
    {
        GDALDestroyDriverManager();
        gdal_ready = false;
    }
    proj_cleanup();
    pthread_mutex_unlock(&lock);
}
