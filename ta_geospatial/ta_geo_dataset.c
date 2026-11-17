/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial engine: vector datasets over GDAL/OGR
 *
 * Opening (with a driver allowlist: an untrusted file does not get to
 * pick VRT and point it at /etc), inspection, bounded fetching,
 * validation and comparison - every pass streams the features once and
 * keeps only per-feature hashes, never the features themselves - and
 * writing generated datasets with planted defects.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gdal_utils.h>

#include "ta_geo_int.h"

/* Drivers a dataset may be opened with unless the caller says
 * drivers=any. No VRT (it can reference any file the agent can read),
 * no drivers that run programs. */
static const char *const safe_drivers[] = {
    "GeoJSON", "GeoJSONSeq", "ESRIJSON", "GPKG", "ESRI Shapefile", "KML",
    "LIBKML", "GML", "FlatGeobuf", "Parquet", "Arrow", "CSV", "SQLite",
    "MBTiles", "OAPIF", "WFS", "WMS", "GTiff", "COG", "PNG", "JPEG", "MEM",
    "Memory", "netCDF", "HFA", "AAIGrid", "XYZ", "GPX", "MapInfo File",
    NULL
};

/* ---- geometry exchange --------------------------------------------- */

/* An OGR geometry as GEOS (via ISO WKB, which carries Z and M). */
GEOSGeometry *
geo_ogr2geos(OGRGeometryH og, char **err)
{
    GEOSContextHandle_t h = geo_geos();
    size_t n = OGR_G_WkbSizeEx(og);
    unsigned char *b = malloc(n > 0 ? n : 1);
    GEOSWKBReader *rd;
    GEOSGeometry *g;

    if (b == NULL)
    {
        seterr(err, TA_GEO_ENOMEM, "out of memory");
        return NULL;
    }
    if (OGR_G_ExportToIsoWkb(og, wkbNDR, b) != OGRERR_NONE)
    {
        free(b);
        seterr(err, TA_GEO_ECORRUPT, "geometry does not export");
        return NULL;
    }
    rd = GEOSWKBReader_create_r(h);
    g = GEOSWKBReader_read_r(h, rd, b, n);
    GEOSWKBReader_destroy_r(h, rd);
    free(b);
    if (g == NULL)
        seterr(err, TA_GEO_ECORRUPT, "geometry does not convert: %s",
               geo_geos_err());
    return g;
}

/* GEOS to OGR. */
OGRGeometryH
geo_geos2ogr(const GEOSGeometry *g)
{
    GEOSContextHandle_t h = geo_geos();
    GEOSWKBWriter *w = GEOSWKBWriter_create_r(h);
    unsigned char *b;
    size_t n = 0;
    OGRGeometryH og = NULL;

    GEOSWKBWriter_setOutputDimension_r(h, w, 4);
    GEOSWKBWriter_setFlavor_r(h, w, GEOS_WKB_ISO);
    b = GEOSWKBWriter_write_r(h, w, g, &n);
    GEOSWKBWriter_destroy_r(h, w);
    if (b == NULL)
        return NULL;
    if (OGR_G_CreateFromWkbEx(b, NULL, &og, n) != OGRERR_NONE)
        og = NULL;
    GEOSFree_r(h, b);
    return og;
}

/* "EPSG:4326" of a spatial reference, or its PROJ string, or "". */
static void
srs_name(OGRSpatialReferenceH srs, char *out, size_t len)
{
    const char *auth;
    const char *code;

    out[0] = '\0';
    if (srs == NULL)
        return;
    auth = OSRGetAuthorityName(srs, NULL);
    code = OSRGetAuthorityCode(srs, NULL);
    if (auth == NULL || code == NULL)
    {
        OSRAutoIdentifyEPSG(srs);
        auth = OSRGetAuthorityName(srs, NULL);
        code = OSRGetAuthorityCode(srs, NULL);
    }
    if (auth != NULL && code != NULL)
        snprintf(out, len, "%s:%s", auth, code);
    else
    {
        char *wkt = NULL;
        const char *opts[] = { "FORMAT=WKT2_2019", "MULTILINE=NO", NULL };

        if (OSRExportToWktEx(srs, &wkt, opts) == OGRERR_NONE && wkt != NULL)
            snprintf(out, len, "%s", wkt);
        CPLFree(wkt);
    }
}

/* ---- opening ------------------------------------------------------- */

static int
open_flags(kv *opt, bool raster, char ***drivers, char ***oo, bool *update,
           char **err)
{
    const char *d = kv_str(opt, "drivers", NULL);
    size_t i;
    int rc;

    *drivers = NULL;
    *oo = NULL;
    if ((rc = kv_bool(opt, "update", false, update, err)) != 0)
        return rc;
    if (d == NULL)
    {
        for (i = 0; safe_drivers[i] != NULL; i++)
            *drivers = CSLAddString(*drivers, safe_drivers[i]);
    }
    else if (strcmp(d, "any") != 0)
    {
        *drivers = CSLTokenizeString2(d, ",", 0);
    }
    for (i = 0; i < opt->n; i++)
    {
        if (strncmp(opt->v[i].key, "oo.", 3) == 0)
        {
            opt->v[i].used = true;
            *oo = CSLSetNameValue(*oo, opt->v[i].key + 3, opt->v[i].val);
        }
    }
    (void)raster;
    return TA_GEO_OK;
}

static int
ds_open(geo_req *r)
{
    const char *mode = kv_str(r->opt, "mode", "vector");
    bool raster = strcmp(mode, "raster") == 0;
    char **drivers = NULL;
    char **oo = NULL;
    bool update = false;
    unsigned int flags;
    GDALDatasetH ds;
    geo_ds *d;
    uint64_t t0 = geo_now_ns();
    int rc;

    if (!raster && strcmp(mode, "vector") != 0)
        return seterr(r->err, TA_GEO_EINVAL, "mode is vector or raster");
    if (*r->text == '\0')
        return seterr(r->err, TA_GEO_EINVAL, "no path");
    if ((rc = open_flags(r->opt, raster, &drivers, &oo, &update,
                         r->err)) != 0)
        return rc;
    flags = (raster ? GDAL_OF_RASTER : GDAL_OF_VECTOR) | GDAL_OF_VERBOSE_ERROR |
            (update ? GDAL_OF_UPDATE : GDAL_OF_READONLY);
    ds = GDALOpenEx(r->text, flags, (const char *const *)drivers,
                    (const char *const *)oo, NULL);
    CSLDestroy(drivers);
    CSLDestroy(oo);
    if (ds == NULL)
    {
        VSIStatBufL st;
        bool exists = VSIStatL(r->text, &st) == 0;

        if (!exists && strstr(r->text, ":") == NULL)
            return seterr(r->err, TA_GEO_ENOENT, "%s: no such file",
                          r->text);
        return seterr(r->err, exists ? TA_GEO_ECORRUPT : TA_GEO_EIO,
                      "%s does not open as a %s dataset: %s", r->text, mode,
                      geo_gdal_err()[0] != '\0' ? geo_gdal_err() :
                      "no allowed driver recognises it");
    }
    if (raster && GDALGetRasterCount(ds) == 0)
    {
        GDALClose(ds);
        return seterr(r->err, TA_GEO_ECORRUPT, "%s has no raster band",
                      r->text);
    }
    d = calloc(1, sizeof(*d));
    if (d == NULL)
    {
        GDALClose(ds);
        return seterr(r->err, TA_GEO_ENOMEM, "out of memory");
    }
    d->ds = ds;
    d->raster = raster;
    d->path = strdup(r->text);
    snprintf(d->driver, sizeof(d->driver), "%s",
             GDALGetDriverShortName(GDALGetDatasetDriver(ds)));
    if ((rc = geo_obj_add(GEO_O_DS, d, r->handle, r->err)) != 0)
    {
        geo_ds_free(d);
        return rc;
    }
    sb_kv_str(r->res, "driver", d->driver);
    sb_kv_str(r->res, "format",
              GDALGetDriverLongName(GDALGetDatasetDriver(ds)));
    sb_kv_int(r->res, "layers", GDALDatasetGetLayerCount(ds));
    sb_kv_int(r->res, "bands", GDALGetRasterCount(ds));
    sb_kv_num(r->res, "elapsed_us", geo_us_since(t0));
    return TA_GEO_OK;
}

static geo_ds *
get_ds(geo_req *r, int h, bool raster)
{
    geo_ds *d = geo_obj_get(h, GEO_O_DS, r->err);

    if (d != NULL && d->raster != raster)
    {
        seterr(r->err, TA_GEO_EINVAL, "dataset %d was opened as %s", h,
               d->raster ? "raster" : "vector");
        return NULL;
    }
    return d;
}

static OGRLayerH
get_layer(geo_req *r, geo_ds *d, const char *key)
{
    const char *l = kv_get(r->opt, key);
    OGRLayerH lay;
    char *end;
    long idx;

    if (l == NULL || *l == '\0')
        lay = GDALDatasetGetLayer(d->ds, 0);
    else
    {
        idx = strtol(l, &end, 10);
        lay = *end == '\0' ? GDALDatasetGetLayer(d->ds, (int)idx) :
                             GDALDatasetGetLayerByName(d->ds, l);
    }
    if (lay == NULL)
        seterr(r->err, TA_GEO_ENOENT, "%s: no layer %s", d->path,
               l != NULL ? l : "0");
    return lay;
}

/* ---- inspection ---------------------------------------------------- */

static const char *
field_type(OGRFieldType t, OGRFieldSubType st)
{
    if (st == OFSTBoolean)
        return "Boolean";
    switch (t)
    {
        case OFTInteger: return "Integer";
        case OFTInteger64: return "Integer64";
        case OFTReal: return "Real";
        case OFTString: return "String";
        case OFTDate: return "Date";
        case OFTTime: return "Time";
        case OFTDateTime: return "DateTime";
        case OFTBinary: return "Binary";
        case OFTIntegerList: return "IntegerList";
        case OFTInteger64List: return "Integer64List";
        case OFTRealList: return "RealList";
        case OFTStringList: return "StringList";
        default: return "Other";
    }
}

static void
put_extent(sbuf *res, OGREnvelope *e)
{
    sb_kv_num(res, "minx", e->MinX);
    sb_kv_num(res, "miny", e->MinY);
    sb_kv_num(res, "maxx", e->MaxX);
    sb_kv_num(res, "maxy", e->MaxY);
}

static int
ds_info(geo_req *r)
{
    geo_ds *d = get_ds(r, r->ha, false);
    const char *k = *r->kind != '\0' ? r->kind : "layers";
    OGRLayerH lay;
    char crs[1024];
    int i;
    int n;

    if (d == NULL)
        return TA_GEO_EINVAL;
    n = GDALDatasetGetLayerCount(d->ds);
    sb_kv_str(r->res, "driver", d->driver);
    sb_kv_int(r->res, "layers", n);
    if (strcmp(k, "layers") == 0)
    {
        char first[1024] = "";
        bool consistent = true;

        for (i = 0; i < n; i++)
        {
            lay = GDALDatasetGetLayer(d->ds, i);
            srs_name(OGR_L_GetSpatialRef(lay), crs, sizeof(crs));
            if (i == 0)
                snprintf(first, sizeof(first), "%s", crs);
            else if (strcmp(first, crs) != 0)
                consistent = false;
            sb_printf(r->rep, "%d\t", i);
            sb_field(r->rep, OGR_L_GetName(lay));
            sb_printf(r->rep, "\t%s\t",
                      OGRGeometryTypeToName(OGR_L_GetGeomType(lay)));
            sb_field(r->rep, crs);
            sb_printf(r->rep, "\t%lld\t", (long long)
                      OGR_L_GetFeatureCount(lay, 0));
            sb_field(r->rep, OGR_L_GetFIDColumn(lay));
            sb_puts(r->rep, "\t");
            sb_field(r->rep, OGR_L_GetGeometryColumn(lay));
            sb_puts(r->rep, "\n");
        }
        sb_kv_int(r->res, "crs_consistent", consistent);
        return TA_GEO_OK;
    }
    lay = get_layer(r, d, "layer");
    if (lay == NULL)
        return TA_GEO_ENOENT;
    sb_kv_str(r->res, "layer", OGR_L_GetName(lay));
    if (strcmp(k, "schema") == 0)
    {
        OGRFeatureDefnH def = OGR_L_GetLayerDefn(lay);
        int nf = OGR_FD_GetFieldCount(def);

        sb_kv_int(r->res, "fields", nf);
        sb_kv_str(r->res, "geometry_type",
                  OGRGeometryTypeToName(OGR_L_GetGeomType(lay)));
        srs_name(OGR_L_GetSpatialRef(lay), crs, sizeof(crs));
        sb_kv_str(r->res, "crs", crs);
        sb_kv_str(r->res, "fid_column", OGR_L_GetFIDColumn(lay));
        for (i = 0; i < nf; i++)
        {
            OGRFieldDefnH f = OGR_FD_GetFieldDefn(def, i);

            sb_field(r->rep, OGR_Fld_GetNameRef(f));
            sb_printf(r->rep, "\t%s\t%d\t%d\t%d\t%d\n",
                      field_type(OGR_Fld_GetType(f), OGR_Fld_GetSubType(f)),
                      OGR_Fld_GetWidth(f), OGR_Fld_GetPrecision(f),
                      OGR_Fld_IsNullable(f), OGR_Fld_IsUnique(f));
        }
        return TA_GEO_OK;
    }
    if (strcmp(k, "count") == 0)
    {
        bool exact = false;
        int rc;
        long long c;

        if ((rc = kv_bool(r->opt, "exact", false, &exact, r->err)) != 0)
            return rc;
        if (exact)
        {
            OGRFeatureH f;

            c = 0;
            OGR_L_ResetReading(lay);
            while ((f = OGR_L_GetNextFeature(lay)) != NULL)
            {
                OGR_F_Destroy(f);
                c++;
                if (geo_deadline_passed())
                    return seterr(r->err, TA_GEO_ETIMEDOUT, "count ran "
                                  "past the deadline");
            }
        }
        else
            c = (long long)OGR_L_GetFeatureCount(lay, 1);
        sb_kv_int(r->res, "count", c);
        sb_kv_int(r->res, "exact", exact);
        return TA_GEO_OK;
    }
    if (strcmp(k, "extent") == 0)
    {
        OGREnvelope e;

        if (OGR_L_GetExtent(lay, &e, 1) != OGRERR_NONE)
            return seterr(r->err, TA_GEO_ENOENT, "layer has no extent "
                          "(no geometries?)");
        put_extent(r->res, &e);
        srs_name(OGR_L_GetSpatialRef(lay), crs, sizeof(crs));
        sb_kv_str(r->res, "crs", crs);
        return TA_GEO_OK;
    }
    if (strcmp(k, "crs") == 0)
    {
        OGRSpatialReferenceH s = OGR_L_GetSpatialRef(lay);
        char *wkt = NULL;
        const char *opts[] = { "FORMAT=WKT2_2019", NULL };

        srs_name(s, crs, sizeof(crs));
        sb_kv_str(r->res, "crs", crs);
        if (s != NULL)
        {
            int dummy = 0;
            const int *map = OSRGetDataAxisToSRSAxisMapping(s, &dummy);

            sb_kv_int(r->res, "data_axis_swapped",
                      dummy >= 2 && map != NULL && map[0] == 2);
            if (OSRExportToWktEx(s, &wkt, opts) == OGRERR_NONE)
                sb_puts(r->rep, wkt);
        }
        CPLFree(wkt);
        return TA_GEO_OK;
    }
    return seterr(r->err, TA_GEO_EINVAL, "unknown dataset query '%s' "
                  "(layers, schema, count, extent, crs)", k);
}

/* ---- fetching ------------------------------------------------------ */

static void
put_json_str(sbuf *b, const char *s)
{
    const unsigned char *p;

    sb_puts(b, "\"");
    for (p = (const unsigned char *)s; *p != '\0'; p++)
    {
        if (*p == '"' || *p == '\\')
            sb_printf(b, "\\%c", *p);
        else if (*p < 0x20)
            sb_printf(b, "\\u%04x", *p);
        else
            sb_put(b, (const char *)p, 1);
    }
    sb_puts(b, "\"");
}

/* The attributes of a feature as a JSON object. */
static void
attrs_json(OGRFeatureH f, sbuf *b)
{
    OGRFeatureDefnH def = OGR_F_GetDefnRef(f);
    int n = OGR_FD_GetFieldCount(def);
    int i;

    sb_puts(b, "{");
    for (i = 0; i < n; i++)
    {
        OGRFieldDefnH fd = OGR_FD_GetFieldDefn(def, i);
        OGRFieldType t = OGR_Fld_GetType(fd);

        if (i > 0)
            sb_puts(b, ",");
        put_json_str(b, OGR_Fld_GetNameRef(fd));
        sb_puts(b, ":");
        if (!OGR_F_IsFieldSetAndNotNull(f, i))
            sb_puts(b, "null");
        else if (t == OFTInteger || t == OFTInteger64)
            sb_printf(b, "%lld", (long long)OGR_F_GetFieldAsInteger64(f, i));
        else if (t == OFTReal)
            sb_num(b, OGR_F_GetFieldAsDouble(f, i));
        else
            put_json_str(b, OGR_F_GetFieldAsString(f, i));
    }
    sb_puts(b, "}");
}

static int
ds_fetch(geo_req *r)
{
    geo_ds *d = get_ds(r, r->ha, false);
    OGRLayerH lay;
    OGRFeatureH f;
    long long offset, limit, i = 0, out = 0;
    const char *where = kv_get(r->opt, "where");
    const char *bbox = kv_get(r->opt, "bbox");
    const char *gfmt = kv_str(r->opt, "geometry", "wkt");
    char crs[1024];
    int rc;

    if (d == NULL)
        return TA_GEO_EINVAL;
    lay = get_layer(r, d, "layer");
    if (lay == NULL)
        return TA_GEO_ENOENT;
    srs_name(OGR_L_GetSpatialRef(lay), crs, sizeof(crs));

    if (strcmp(r->kind, "geometry") == 0)
    {
        long long fid;
        OGRGeometryH og;
        GEOSGeometry *g;

        if ((rc = kv_long(r->opt, "fid", -1, &fid, r->err)) != 0)
            return rc;
        f = OGR_L_GetFeature(lay, (GIntBig)fid);
        if (f == NULL)
            return seterr(r->err, TA_GEO_ENOENT, "no feature %lld", fid);
        og = OGR_F_GetGeometryRef(f);
        if (og == NULL)
        {
            OGR_F_Destroy(f);
            return seterr(r->err, TA_GEO_ENOENT, "feature %lld has no "
                          "geometry", fid);
        }
        g = geo_ogr2geos(og, r->err);
        OGR_F_Destroy(f);
        if (g == NULL)
            return TA_GEO_ECORRUPT;
        if ((rc = geo_geom_new(g, crs, r->handle, r->err)) != 0)
            return rc;
        sb_kv_str(r->res, "crs", crs);
        sb_kv_str(r->res, "type", geo_type_name(GEOSGeomTypeId_r(geo_geos(),
                                                                 g)));
        return TA_GEO_OK;
    }
    if (*r->kind != '\0' && strcmp(r->kind, "features") != 0)
        return seterr(r->err, TA_GEO_EINVAL, "fetch features or geometry");
    if ((rc = kv_long(r->opt, "offset", 0, &offset, r->err)) != 0 ||
        (rc = kv_long(r->opt, "limit", 100, &limit, r->err)) != 0)
        return rc;
    if (limit > geo_lim.max_report)
        limit = geo_lim.max_report;
    if (where != NULL && OGR_L_SetAttributeFilter(lay, where) != OGRERR_NONE)
        return seterr(r->err, TA_GEO_EINVAL, "bad filter '%s': %s", where,
                      geo_gdal_err());
    if (bbox != NULL)
    {
        double b[4];

        if (sscanf(bbox, "%lf %lf %lf %lf", &b[0], &b[1], &b[2], &b[3]) != 4)
            return seterr(r->err, TA_GEO_EINVAL, "bbox is 'minx miny maxx "
                          "maxy'");
        OGR_L_SetSpatialFilterRect(lay, b[0], b[1], b[2], b[3]);
    }
    OGR_L_ResetReading(lay);
    while ((f = OGR_L_GetNextFeature(lay)) != NULL)
    {
        if (i++ >= offset && out < limit)
        {
            OGRGeometryH og = OGR_F_GetGeometryRef(f);

            sb_printf(r->rep, "%lld\t", (long long)OGR_F_GetFID(f));
            if (og != NULL && strcmp(gfmt, "none") != 0)
            {
                char *s = NULL;

                if (strcmp(gfmt, "geojson") == 0)
                    s = OGR_G_ExportToJson(og);
                else
                    OGR_G_ExportToIsoWkt(og, &s);
                sb_field(r->rep, s);
                CPLFree(s);
            }
            sb_puts(r->rep, "\t");
            {
                sbuf a = SBUF_INIT;

                attrs_json(f, &a);
                sb_field(r->rep, a.p);
                sb_free(&a);
            }
            sb_puts(r->rep, "\n");
            out++;
        }
        OGR_F_Destroy(f);
        if (out >= limit)
            break;
    }
    OGR_L_SetAttributeFilter(lay, NULL);
    OGR_L_SetSpatialFilter(lay, NULL);
    sb_kv_int(r->res, "returned", out);
    sb_kv_str(r->res, "crs", crs);
    return TA_GEO_OK;
}

/* ---- validation ---------------------------------------------------- */

typedef struct vstat {
    sbuf *rep;
    long long max;
    long long issues;
    long long n[16];
} vstat;

static const char *const vcat[] = {
    "invalid_geometry", "missing_geometry", "empty_geometry",
    "duplicate_id", "crs_mismatch", "out_of_range_coordinate",
    "schema_mismatch", "unexpected_geometry_type", "invalid_attribute",
    "non_finite", "missing_id", NULL
};

enum {
    V_INVALID, V_MISSING, V_EMPTY, V_DUP, V_CRS, V_RANGE, V_SCHEMA, V_TYPE,
    V_ATTR, V_NONFINITE, V_NOID,
};

static void
vissue(vstat *v, int cat, long long fid, const char *id, const char *fmt,
       ...) GEO_PRINTF(5, 6);

static void
vissue(vstat *v, int cat, long long fid, const char *id, const char *fmt,
       ...)
{
    va_list ap;
    sbuf d = SBUF_INIT;

    v->n[cat]++;
    if (v->issues++ >= v->max)
        return;
    va_start(ap, fmt);
    sb_vprintf(&d, fmt, ap);
    va_end(ap);
    sb_printf(v->rep, "%lld\t", fid);
    sb_field(v->rep, id);
    sb_printf(v->rep, "\t%s\t", vcat[cat]);
    sb_field(v->rep, d.p);
    sb_puts(v->rep, "\n");
    sb_free(&d);
}

/* Does an OGC type name list ("Polygon,MultiPolygon") take this type? */
static bool
type_allowed(const char *list, const char *t)
{
    char buf[512];
    char *tok;
    char *save = NULL;

    snprintf(buf, sizeof(buf), "%s", list);
    for (tok = strtok_r(buf, ",", &save); tok != NULL;
         tok = strtok_r(NULL, ",", &save))
    {
        if (strcasecmp(tok, t) == 0)
            return true;
    }
    return false;
}

/* schema="name:Type[:notnull];..." against the layer definition. */
static void
check_schema(vstat *v, OGRFeatureDefnH def, const char *schema, bool strict)
{
    char buf[4096];
    char *tok;
    char *save = NULL;
    int nf = OGR_FD_GetFieldCount(def);
    int seen = 0;

    snprintf(buf, sizeof(buf), "%s", schema);
    for (tok = strtok_r(buf, ";", &save); tok != NULL;
         tok = strtok_r(NULL, ";", &save))
    {
        char *t = strchr(tok, ':');
        int idx;

        if (t != NULL)
            *t++ = '\0';
        idx = OGR_FD_GetFieldIndex(def, tok);
        if (idx < 0)
        {
            vissue(v, V_SCHEMA, -1, "", "field %s is missing", tok);
            continue;
        }
        seen++;
        if (t != NULL && *t != '\0')
        {
            OGRFieldDefnH fd = OGR_FD_GetFieldDefn(def, idx);
            const char *have = field_type(OGR_Fld_GetType(fd),
                                          OGR_Fld_GetSubType(fd));
            char *want = t;
            char *opt = strchr(t, ':');

            if (opt != NULL)
                *opt = '\0';
            /* Integer and Integer64 are one logical type: drivers pick
             * the width. */
            if (strcasecmp(want, have) != 0 &&
                !(strncasecmp(want, "Integer", 7) == 0 &&
                  strncasecmp(have, "Integer", 7) == 0 &&
                  strstr(have, "List") == NULL && strstr(want, "List") == NULL))
                vissue(v, V_SCHEMA, -1, "", "field %s is %s, expected %s",
                       tok, have, want);
        }
    }
    if (strict && seen < nf)
        vissue(v, V_SCHEMA, -1, "", "%d fields not in the expected schema",
               nf - seen);
}

typedef struct attr_rule {
    int idx;
    bool required;
    bool has_range;
    double lo, hi;
    char *allowed;      /* "a|b|c" */
    char name[128];
} attr_rule;

static int
rules_read(kv *opt, OGRFeatureDefnH def, attr_rule **out, size_t *nout,
           char **err)
{
    attr_rule *r = NULL;
    size_t n = 0;
    size_t i;
    const char *req = kv_get(opt, "required");

#define RULE_FOR(_name, _rp) \
    do {                                                                \
        size_t _k;                                                      \
        int _idx = OGR_FD_GetFieldIndex(def, (_name));                  \
        attr_rule *_nr;                                                 \
        (_rp) = NULL;                                                   \
        if (_idx < 0)                                                   \
        {                                                               \
            free(r);                                                    \
            return seterr(err, TA_GEO_ENOENT, "no field '%s'", (_name));\
        }                                                               \
        for (_k = 0; _k < n; _k++)                                      \
            if (r[_k].idx == _idx)                                      \
                (_rp) = &r[_k];                                         \
        if ((_rp) == NULL)                                              \
        {                                                               \
            _nr = realloc(r, (n + 1) * sizeof(*r));                     \
            if (_nr == NULL)                                            \
            {                                                           \
                free(r);                                                \
                return seterr(err, TA_GEO_ENOMEM, "out of memory");     \
            }                                                           \
            r = _nr;                                                    \
            memset(&r[n], 0, sizeof(r[n]));                             \
            r[n].idx = _idx;                                            \
            snprintf(r[n].name, sizeof(r[n].name), "%s", (_name));      \
            (_rp) = &r[n++];                                            \
        }                                                               \
    } while (0)

    if (req != NULL && *req != '\0')
    {
        char buf[2048];
        char *tok;
        char *save = NULL;

        snprintf(buf, sizeof(buf), "%s", req);
        for (tok = strtok_r(buf, ",", &save); tok != NULL;
             tok = strtok_r(NULL, ",", &save))
        {
            attr_rule *rp;

            RULE_FOR(tok, rp);
            rp->required = true;
        }
    }
    for (i = 0; i < opt->n; i++)
    {
        attr_rule *rp;
        const char *k = opt->v[i].key;

        if (strncmp(k, "range.", 6) == 0)
        {
            opt->v[i].used = true;
            RULE_FOR(k + 6, rp);
            if (sscanf(opt->v[i].val, "%lf:%lf", &rp->lo, &rp->hi) != 2)
            {
                free(r);
                return seterr(err, TA_GEO_EINVAL, "%s is 'min:max'", k);
            }
            rp->has_range = true;
        }
        else if (strncmp(k, "allowed.", 8) == 0)
        {
            opt->v[i].used = true;
            RULE_FOR(k + 8, rp);
            rp->allowed = opt->v[i].val;
        }
    }
#undef RULE_FOR
    *out = r;
    *nout = n;
    return TA_GEO_OK;
}

static bool
value_allowed(const char *list, const char *v)
{
    size_t lv = strlen(v);
    const char *p = list;

    while (p != NULL && *p != '\0')
    {
        const char *bar = strchr(p, '|');
        size_t l = bar != NULL ? (size_t)(bar - p) : strlen(p);

        if (l == lv && strncmp(p, v, l) == 0)
            return true;
        p = bar != NULL ? bar + 1 : NULL;
    }
    return false;
}

static void
feature_id(OGRFeatureH f, int id_idx, char *out, size_t len)
{
    if (id_idx < 0)
        snprintf(out, len, "%lld", (long long)OGR_F_GetFID(f));
    else if (!OGR_F_IsFieldSetAndNotNull(f, id_idx))
        out[0] = '\0';
    else
        snprintf(out, len, "%s", OGR_F_GetFieldAsString(f, id_idx));
}

static int
ds_validate(geo_req *r)
{
    GEOSContextHandle_t h = geo_geos();
    geo_ds *d = get_ds(r, r->ha, false);
    OGRLayerH lay;
    OGRFeatureDefnH def;
    OGRFeatureH f;
    const char *id_field = kv_get(r->opt, "id_field");
    const char *types = kv_get(r->opt, "expect_types");
    const char *want_crs = kv_get(r->opt, "expect_crs");
    const char *schema = kv_get(r->opt, "schema");
    const char *bounds = kv_get(r->opt, "bounds");
    bool strict_schema = false;
    bool validity = true;
    double b[4] = { 0, 0, 0, 0 };
    int id_idx = -1;
    attr_rule *rules = NULL;
    size_t nrules = 0;
    geo_hmap ids = { NULL, NULL, 0, 0 };
    vstat v;
    long long nfeat = 0;
    long long nbad_feat = 0;
    char crs[1024];
    size_t i;
    int rc;

    if (d == NULL)
        return TA_GEO_EINVAL;
    lay = get_layer(r, d, "layer");
    if (lay == NULL)
        return TA_GEO_ENOENT;
    def = OGR_L_GetLayerDefn(lay);
    memset(&v, 0, sizeof(v));
    v.rep = r->rep;
    v.max = geo_lim.max_report;
    if ((rc = kv_bool(r->opt, "strict_schema", false, &strict_schema,
                      r->err)) != 0 ||
        (rc = kv_bool(r->opt, "validity", true, &validity, r->err)) != 0)
        return rc;
    if (bounds != NULL &&
        (sscanf(bounds, "%lf %lf %lf %lf", &b[0], &b[1], &b[2], &b[3]) != 4 ||
         b[0] > b[2] || b[1] > b[3]))
        return seterr(r->err, TA_GEO_EINVAL, "bounds is 'minx miny maxx "
                      "maxy'");
    if (id_field != NULL && *id_field != '\0')
    {
        id_idx = OGR_FD_GetFieldIndex(def, id_field);
        if (id_idx < 0)
            return seterr(r->err, TA_GEO_ENOENT, "no id field '%s'",
                          id_field);
    }
    if ((rc = rules_read(r->opt, def, &rules, &nrules, r->err)) != 0)
        return rc;

    /* Dataset-level checks: CRS against the expectation and across the
     * layers, the schema. */
    srs_name(OGR_L_GetSpatialRef(lay), crs, sizeof(crs));
    if (want_crs != NULL)
    {
        char norm[256];

        geo_crs_norm(want_crs, norm, sizeof(norm));
        if (strcmp(norm, crs) != 0)
        {
            PJ *a = crs[0] != '\0' ? geo_crs_create(crs, NULL) : NULL;
            PJ *bb = geo_crs_create(norm, NULL);

            if (a == NULL || bb == NULL ||
                !proj_is_equivalent_to_with_ctx(geo_proj(), a, bb,
                                                PJ_COMP_EQUIVALENT))
                vissue(&v, V_CRS, -1, "", "layer CRS is '%s', expected %s",
                       crs, norm);
            proj_destroy(a);
            proj_destroy(bb);
        }
    }
    {
        int nl = GDALDatasetGetLayerCount(d->ds);
        int li;

        for (li = 0; li < nl; li++)
        {
            char other[1024];
            OGRLayerH l2 = GDALDatasetGetLayer(d->ds, li);

            if (l2 == lay)
                continue;
            srs_name(OGR_L_GetSpatialRef(l2), other, sizeof(other));
            if (strcmp(other, crs) != 0)
                vissue(&v, V_CRS, -1, "", "layer %s is in '%s', this layer "
                       "in '%s'", OGR_L_GetName(l2), other, crs);
        }
    }
    if (schema != NULL)
        check_schema(&v, def, schema, strict_schema);

    OGR_L_ResetReading(lay);
    while ((f = OGR_L_GetNextFeature(lay)) != NULL)
    {
        long long fid = (long long)OGR_F_GetFID(f);
        long long before = v.issues;
        OGRGeometryH og = OGR_F_GetGeometryRef(f);
        char id[256];

        nfeat++;
        if (nfeat > geo_lim.max_features)
        {
            OGR_F_Destroy(f);
            rc = seterr(r->err, TA_GEO_E2BIG, "more than max_features "
                        "(%lld) features", geo_lim.max_features);
            goto out;
        }
        if (geo_deadline_passed())
        {
            OGR_F_Destroy(f);
            rc = seterr(r->err, TA_GEO_ETIMEDOUT, "validation ran past the "
                        "deadline after %lld features", nfeat);
            goto out;
        }
        feature_id(f, id_idx, id, sizeof(id));
        if (id_idx >= 0)
        {
            if (id[0] == '\0')
                vissue(&v, V_NOID, fid, "", "no identifier");
            else
            {
                int64_t first = 0;
                int put = geo_hmap_put(&ids, geo_hash(id, strlen(id)), fid,
                                       &first);

                if (put < 0)
                {
                    OGR_F_Destroy(f);
                    rc = seterr(r->err, TA_GEO_ENOMEM, "out of memory");
                    goto out;
                }
                if (put == 0)
                    vissue(&v, V_DUP, fid, id, "identifier also on feature "
                           "%lld", (long long)first);
            }
        }
        for (i = 0; i < nrules; i++)
        {
            attr_rule *ar = &rules[i];

            if (!OGR_F_IsFieldSetAndNotNull(f, ar->idx))
            {
                if (ar->required)
                    vissue(&v, V_ATTR, fid, id, "%s is null", ar->name);
                continue;
            }
            if (ar->has_range)
            {
                double x = OGR_F_GetFieldAsDouble(f, ar->idx);

                if (!(x >= ar->lo && x <= ar->hi))
                    vissue(&v, V_ATTR, fid, id, "%s = %.17g outside "
                           "[%g, %g]", ar->name, x, ar->lo, ar->hi);
            }
            if (ar->allowed != NULL &&
                !value_allowed(ar->allowed,
                               OGR_F_GetFieldAsString(f, ar->idx)))
                vissue(&v, V_ATTR, fid, id, "%s = '%s' not allowed",
                       ar->name, OGR_F_GetFieldAsString(f, ar->idx));
        }
        if (og == NULL)
        {
            vissue(&v, V_MISSING, fid, id, "no geometry");
        }
        else
        {
            GEOSGeometry *g = geo_ogr2geos(og, NULL);
            const char *tn = OGRGeometryTypeToName(
                                 wkbFlatten(OGR_G_GetGeometryType(og)));

            if (types != NULL && !type_allowed(types, tn))
                vissue(&v, V_TYPE, fid, id, "%s, expected %s", tn, types);
            if (g == NULL)
            {
                vissue(&v, V_INVALID, fid, id, "does not convert");
            }
            else
            {
                double gb[4];
                bool nonfinite = false;

                if (GEOSisEmpty_r(h, g) == 1)
                    vissue(&v, V_EMPTY, fid, id, "empty geometry");
                else if (geo_bounds(g, gb))
                {
                    nonfinite = !isfinite(gb[0]) || !isfinite(gb[1]) ||
                                !isfinite(gb[2]) || !isfinite(gb[3]);
                    if (nonfinite)
                        vissue(&v, V_NONFINITE, fid, id, "coordinates are "
                               "NaN or infinite");
                    else if (bounds != NULL &&
                             (gb[0] < b[0] || gb[1] < b[1] || gb[2] > b[2] ||
                              gb[3] > b[3]))
                        vissue(&v, V_RANGE, fid, id, "extent %.17g %.17g "
                               "%.17g %.17g outside the bounds", gb[0], gb[1],
                               gb[2], gb[3]);
                }
                if (validity && !nonfinite && GEOSisEmpty_r(h, g) != 1)
                {
                    char *reason = NULL;
                    GEOSGeometry *loc = NULL;

                    if (GEOSisValidDetail_r(h, g, 0, &reason, &loc) == 0)
                        vissue(&v, V_INVALID, fid, id, "%s: %s",
                               geo_validity_category(reason != NULL ?
                                                     reason : ""),
                               reason != NULL ? reason : "invalid");
                    if (reason != NULL)
                        GEOSFree_r(h, reason);
                    if (loc != NULL)
                        GEOSGeom_destroy_r(h, loc);
                }
                GEOSGeom_destroy_r(h, g);
            }
        }
        if (v.issues > before)
            nbad_feat++;
        OGR_F_Destroy(f);
    }
    rc = TA_GEO_OK;

out:
    sb_kv_int(r->res, "features", nfeat);
    sb_kv_int(r->res, "bad_features", nbad_feat);
    sb_kv_int(r->res, "issues", v.issues);
    sb_kv_int(r->res, "ok", v.issues == 0);
    sb_kv_str(r->res, "crs", crs);
    for (i = 0; vcat[i] != NULL; i++)
        sb_kv_int(r->res, vcat[i], v.n[i]);
    sb_kv_int(r->res, "distinct_ids", (long long)ids.n);
    geo_hmap_free(&ids);
    free(rules);
    return rc;
}

/* ---- comparison ---------------------------------------------------- */

static bool
num_field(OGRFieldType t)
{
    return t == OFTInteger || t == OFTInteger64 || t == OFTReal;
}

static int
ds_compare(geo_req *r)
{
    GEOSContextHandle_t h = geo_geos();
    geo_ds *da = get_ds(r, r->ha, false);
    geo_ds *db = da != NULL ? get_ds(r, r->hb, false) : NULL;
    OGRLayerH la, lb;
    OGRFeatureDefnH defa, defb;
    OGRFeatureH f;
    const char *id_field = kv_get(r->opt, "id_field");
    const char *gmode = kv_str(r->opt, "geometry", "topological");
    const char *attrs = kv_str(r->opt, "attributes", "all");
    double tol = 0;
    double ftol = 0;
    int ida = -1, idb = -1;
    geo_hmap mb = { NULL, NULL, 0, 0 };
    geo_hmap ma = { NULL, NULL, 0, 0 };
    char *matched = NULL;
    int64_t *bfids = NULL;
    size_t nb = 0, capb = 0;
    long long na = 0, nmatch = 0, only_a = 0, only_b = 0, gdiff = 0,
              adiff = 0, dup_a = 0, dup_b = 0;
    double max_hd = 0;
    char ca[1024], cb[1024];
    bool crs_ok = true;
    long long rep_n = 0;
    int rc = TA_GEO_OK;
    int i;

    if (da == NULL || db == NULL)
        return TA_GEO_EINVAL;
    la = get_layer(r, da, "layer_a");
    lb = la != NULL ? get_layer(r, db, "layer_b") : NULL;
    if (la == NULL || lb == NULL)
        return TA_GEO_ENOENT;
    defa = OGR_L_GetLayerDefn(la);
    defb = OGR_L_GetLayerDefn(lb);
    if ((rc = kv_dbl(r->opt, "tolerance", 0, &tol, r->err)) != 0 ||
        (rc = kv_dbl(r->opt, "float_tolerance", 0, &ftol, r->err)) != 0)
        return rc;
    if (strcmp(gmode, "topological") != 0 && strcmp(gmode, "exact") != 0 &&
        strcmp(gmode, "hausdorff") != 0 && strcmp(gmode, "none") != 0)
        return seterr(r->err, TA_GEO_EINVAL, "geometry is topological, "
                      "exact, hausdorff or none");
    if (id_field != NULL && *id_field != '\0')
    {
        ida = OGR_FD_GetFieldIndex(defa, id_field);
        idb = OGR_FD_GetFieldIndex(defb, id_field);
        if (ida < 0 || idb < 0)
            return seterr(r->err, TA_GEO_ENOENT, "id field '%s' missing in "
                          "%s", id_field, ida < 0 ? "A" : "B");
    }

    srs_name(OGR_L_GetSpatialRef(la), ca, sizeof(ca));
    srs_name(OGR_L_GetSpatialRef(lb), cb, sizeof(cb));
    if (strcmp(ca, cb) != 0)
    {
        PJ *pa = ca[0] != '\0' ? geo_crs_create(ca, NULL) : NULL;
        PJ *pb = cb[0] != '\0' ? geo_crs_create(cb, NULL) : NULL;

        crs_ok = pa != NULL && pb != NULL &&
                 proj_is_equivalent_to_with_ctx(geo_proj(), pa, pb,
                                                PJ_COMP_EQUIVALENT);
        proj_destroy(pa);
        proj_destroy(pb);
    }
    sb_kv_int(r->res, "crs_compatible", crs_ok);
    sb_kv_str(r->res, "crs_a", ca);
    sb_kv_str(r->res, "crs_b", cb);
    if (!crs_ok)
    {
        sb_kv_int(r->res, "equal", 0);
        return TA_GEO_OK;
    }

#define REP(...) \
    do {                                                    \
        if (rep_n++ < geo_lim.max_report)                   \
            sb_printf(r->rep, __VA_ARGS__);                 \
    } while (0)

    /* Index B: identifier hash -> position, FIDs by position. Only the
     * hashes and FIDs are kept, never the features. */
    OGR_L_ResetReading(lb);
    while ((f = OGR_L_GetNextFeature(lb)) != NULL)
    {
        char id[256];
        int put;

        feature_id(f, idb, id, sizeof(id));
        if (nb == capb)
        {
            int64_t *n2;

            capb = capb == 0 ? 1024 : capb * 2;
            n2 = realloc(bfids, capb * sizeof(*bfids));
            if (n2 == NULL)
            {
                OGR_F_Destroy(f);
                rc = seterr(r->err, TA_GEO_ENOMEM, "out of memory");
                goto out;
            }
            bfids = n2;
        }
        bfids[nb] = (int64_t)OGR_F_GetFID(f);
        put = geo_hmap_put(&mb, geo_hash(id, strlen(id)), (int64_t)nb, NULL);
        if (put < 0)
        {
            OGR_F_Destroy(f);
            rc = seterr(r->err, TA_GEO_ENOMEM, "out of memory");
            goto out;
        }
        if (put == 0)
        {
            dup_b++;
            REP("duplicate_id_b\t%s\t\t\t\n", id);
        }
        nb++;
        OGR_F_Destroy(f);
        if ((long long)nb > geo_lim.max_features)
        {
            rc = seterr(r->err, TA_GEO_E2BIG, "B has more than "
                        "max_features features");
            goto out;
        }
    }
    matched = calloc(nb > 0 ? nb : 1, 1);
    if (matched == NULL)
    {
        rc = seterr(r->err, TA_GEO_ENOMEM, "out of memory");
        goto out;
    }

    OGR_L_ResetReading(la);
    while ((f = OGR_L_GetNextFeature(la)) != NULL)
    {
        char id[256];
        int64_t pos;
        OGRFeatureH fb;
        bool diff = false;

        na++;
        if (geo_deadline_passed())
        {
            OGR_F_Destroy(f);
            rc = seterr(r->err, TA_GEO_ETIMEDOUT, "comparison ran past the "
                        "deadline");
            goto out;
        }
        feature_id(f, ida, id, sizeof(id));
        if (geo_hmap_put(&ma, geo_hash(id, strlen(id)), 0, NULL) == 0)
        {
            dup_a++;
            REP("duplicate_id_a\t%s\t\t\t\n", id);
        }
        if (!geo_hmap_get(&mb, geo_hash(id, strlen(id)), &pos) ||
            matched[pos])
        {
            only_a++;
            REP("only_a\t%s\t\t\t\n", id);
            OGR_F_Destroy(f);
            continue;
        }
        matched[pos] = 1;
        nmatch++;
        fb = OGR_L_GetFeature(lb, (GIntBig)bfids[pos]);
        if (fb == NULL)
        {
            OGR_F_Destroy(f);
            rc = seterr(r->err, TA_GEO_EIO, "B feature %lld vanished",
                        (long long)bfids[pos]);
            goto out;
        }
        if (strcmp(gmode, "none") != 0)
        {
            OGRGeometryH oa = OGR_F_GetGeometryRef(f);
            OGRGeometryH ob = OGR_F_GetGeometryRef(fb);
            bool same;

            if (oa == NULL || ob == NULL)
                same = oa == ob;
            else
            {
                GEOSGeometry *ga = geo_ogr2geos(oa, NULL);
                GEOSGeometry *gb = geo_ogr2geos(ob, NULL);
                double hd = 0;

                if (ga == NULL || gb == NULL)
                    same = false;
                else if (strcmp(gmode, "exact") == 0)
                    same = GEOSEqualsExact_r(h, ga, gb, tol) == 1;
                else if (strcmp(gmode, "hausdorff") == 0 ||
                         (tol > 0 && strcmp(gmode, "topological") == 0))
                {
                    same = GEOSHausdorffDistance_r(h, ga, gb, &hd) == 1 &&
                           hd <= tol;
                    if (hd > max_hd)
                        max_hd = hd;
                }
                else
                {
                    /* Topological equality is undefined for invalid
                     * input; fall back to exact equality then. */
                    char e = GEOSEquals_r(h, ga, gb);

                    same = e == 2 ? GEOSEqualsExact_r(h, ga, gb, 0) == 1 :
                                    e == 1;
                }
                if (ga != NULL)
                    GEOSGeom_destroy_r(h, ga);
                if (gb != NULL)
                    GEOSGeom_destroy_r(h, gb);
            }
            if (!same)
            {
                gdiff++;
                diff = true;
                REP("geometry\t%s\t\t\t\n", id);
            }
        }
        if (strcmp(attrs, "none") != 0)
        {
            int nfa = OGR_FD_GetFieldCount(defa);

            for (i = 0; i < nfa; i++)
            {
                OGRFieldDefnH fd = OGR_FD_GetFieldDefn(defa, i);
                const char *nm = OGR_Fld_GetNameRef(fd);
                int j = OGR_FD_GetFieldIndex(defb, nm);
                bool na_ = !OGR_F_IsFieldSetAndNotNull(f, i);
                bool nb_;
                bool same;

                if (strcmp(attrs, "all") != 0 && !type_allowed(attrs, nm))
                    continue;
                if (j < 0)
                    continue;   /* reported once below as a schema diff */
                nb_ = !OGR_F_IsFieldSetAndNotNull(fb, j);
                if (na_ || nb_)
                    same = na_ == nb_;
                else if (num_field(OGR_Fld_GetType(fd)) &&
                         num_field(OGR_Fld_GetType(OGR_FD_GetFieldDefn(defb,
                                                                       j))))
                    same = fabs(OGR_F_GetFieldAsDouble(f, i) -
                                OGR_F_GetFieldAsDouble(fb, j)) <= ftol;
                else
                    same = strcmp(OGR_F_GetFieldAsString(f, i),
                                  OGR_F_GetFieldAsString(fb, j)) == 0;
                if (!same)
                {
                    adiff++;
                    diff = true;
                    if (rep_n++ < geo_lim.max_report)
                    {
                        sb_printf(r->rep, "attribute\t%s\t", id);
                        sb_field(r->rep, nm);
                        sb_puts(r->rep, "\t");
                        sb_field(r->rep, na_ ? "NULL" :
                                 OGR_F_GetFieldAsString(f, i));
                        sb_puts(r->rep, "\t");
                        sb_field(r->rep, nb_ ? "NULL" :
                                 OGR_F_GetFieldAsString(fb, j));
                        sb_puts(r->rep, "\n");
                    }
                }
            }
        }
        (void)diff;
        OGR_F_Destroy(fb);
        OGR_F_Destroy(f);
    }
    {
        size_t k;

        for (k = 0; k < nb; k++)
        {
            if (!matched[k])
            {
                OGRFeatureH fbb = OGR_L_GetFeature(lb, (GIntBig)bfids[k]);
                char id[256] = "?";

                if (fbb != NULL)
                {
                    feature_id(fbb, idb, id, sizeof(id));
                    OGR_F_Destroy(fbb);
                }
                only_b++;
                REP("only_b\t%s\t\t\t\n", id);
            }
        }
    }
    /* Fields one side has and the other not. */
    if (strcmp(attrs, "none") != 0)
    {
        int nfa = OGR_FD_GetFieldCount(defa);
        int nfb = OGR_FD_GetFieldCount(defb);

        for (i = 0; i < nfa; i++)
        {
            const char *nm = OGR_Fld_GetNameRef(OGR_FD_GetFieldDefn(defa, i));

            if (OGR_FD_GetFieldIndex(defb, nm) < 0)
            {
                adiff++;
                REP("schema_a_only\t\t%s\t\t\n", nm);
            }
        }
        for (i = 0; i < nfb; i++)
        {
            const char *nm = OGR_Fld_GetNameRef(OGR_FD_GetFieldDefn(defb, i));

            if (OGR_FD_GetFieldIndex(defa, nm) < 0)
            {
                adiff++;
                REP("schema_b_only\t\t%s\t\t\n", nm);
            }
        }
    }
#undef REP

out:
    sb_kv_int(r->res, "count_a", na);
    sb_kv_int(r->res, "count_b", (long long)nb);
    sb_kv_int(r->res, "matched", nmatch);
    sb_kv_int(r->res, "only_a", only_a);
    sb_kv_int(r->res, "only_b", only_b);
    sb_kv_int(r->res, "geometry_diff", gdiff);
    sb_kv_int(r->res, "attribute_diff", adiff);
    sb_kv_int(r->res, "duplicate_id_a", dup_a);
    sb_kv_int(r->res, "duplicate_id_b", dup_b);
    sb_kv_num(r->res, "max_hausdorff", max_hd);
    sb_kv_int(r->res, "differences", rep_n);
    sb_kv_int(r->res, "equal", rc == 0 && rep_n == 0 && na == (long long)nb);
    free(matched);
    free(bfids);
    geo_hmap_free(&ma);
    geo_hmap_free(&mb);
    return rc;
}

/* ---- writing ------------------------------------------------------- */

/* A deliberately invalid polygon near (x, y): a bow-tie. */
static GEOSGeometry *
bowtie(double x, double y, double s)
{
    char wkt[512];
    GEOSWKTReader *rd = GEOSWKTReader_create_r(geo_geos());
    GEOSGeometry *g;

    snprintf(wkt, sizeof(wkt), "POLYGON ((%.17g %.17g, %.17g %.17g, %.17g "
             "%.17g, %.17g %.17g, %.17g %.17g))", x, y, x + s, y + s, x + s,
             y, x, y + s, x, y);
    g = GEOSWKTReader_read_r(geo_geos(), rd, wkt);
    GEOSWKTReader_destroy_r(geo_geos(), rd);
    return g;
}

typedef struct defect_plan {
    char name[32];
    long long count;
} defect_plan;

static int
defects_read(const char *s, defect_plan *p, size_t max, size_t *n,
             char **err)
{
    char buf[1024];
    char *tok;
    char *save = NULL;
    static const char *const known[] = {
        "invalid", "dup_id", "missing_geom", "out_of_range", "wrong_type",
        "bad_attr", "null_attr", "empty_geom", NULL
    };

    *n = 0;
    if (s == NULL || *s == '\0')
        return TA_GEO_OK;
    snprintf(buf, sizeof(buf), "%s", s);
    for (tok = strtok_r(buf, ",", &save); tok != NULL;
         tok = strtok_r(NULL, ",", &save))
    {
        char *c = strchr(tok, ':');
        size_t i;
        bool ok = false;

        if (*n >= max)
            return seterr(err, TA_GEO_EINVAL, "too many defect kinds");
        if (c != NULL)
            *c++ = '\0';
        for (i = 0; known[i] != NULL; i++)
            ok = ok || strcmp(known[i], tok) == 0;
        if (!ok)
            return seterr(err, TA_GEO_EINVAL, "unknown defect '%s' (invalid, "
                          "dup_id, missing_geom, out_of_range, wrong_type, "
                          "bad_attr, null_attr, empty_geom)", tok);
        snprintf(p[*n].name, sizeof(p[*n].name), "%s", tok);
        p[*n].count = c != NULL ? atoll(c) : 1;
        (*n)++;
    }
    return TA_GEO_OK;
}

int
geo_ds_write_items(const char *path, const char *driver, const char *crs,
                   const char *layer, geo_gen_item *items, size_t n, kv *opt,
                   sbuf *res, sbuf *rep, char **err)
{
    GEOSContextHandle_t h = geo_geos();
    GDALDriverH drv = GDALGetDriverByName(driver);
    GDALDatasetH ds;
    OGRLayerH lay;
    OGRSpatialReferenceH srs = NULL;
    OGRFieldDefnH fd;
    OGRwkbGeometryType gt = wkbUnknown;
    defect_plan plan[8];
    size_t nplan = 0;
    char *defect = NULL;
    long long seed;
    geo_rng rng;
    size_t i;
    size_t k;
    int rc;
    long long written = 0;
    char **lco = NULL;
    sbuf planted = SBUF_INIT;

    if (drv == NULL || GDALGetMetadataItem(drv, GDAL_DCAP_CREATE, NULL) == NULL)
        return seterr(err, TA_GEO_ENOTSUP, "driver '%s' is not here or "
                      "cannot create datasets", driver);
    if ((rc = defects_read(kv_get(opt, "defects"), plan, 8, &nplan,
                           err)) != 0 ||
        (rc = kv_long(opt, "defect_seed", 7, &seed, err)) != 0)
        return rc;
    for (i = 0; i < opt->n; i++)
    {
        if (strncmp(opt->v[i].key, "lco.", 4) == 0)
        {
            opt->v[i].used = true;
            lco = CSLSetNameValue(lco, opt->v[i].key + 4, opt->v[i].val);
        }
    }
    /* Which item gets which defect: distinct positions, deterministic. */
    defect = calloc(n > 0 ? n : 1, 1);
    if (defect == NULL)
    {
        CSLDestroy(lco);
        return seterr(err, TA_GEO_ENOMEM, "out of memory");
    }
    geo_rng_seed(&rng, (uint64_t)seed);
    for (k = 0; k < nplan; k++)
    {
        long long c;

        for (c = 0; c < plan[k].count; c++)
        {
            size_t pos;
            size_t tries = 0;

            /* dup_id copies the previous item's id: never position 0. */
            do {
                pos = n > 1 ? 1 + (size_t)(geo_rng_next(&rng) % (n - 1)) : 0;
            } while (defect[pos] != 0 && ++tries < 10 * n);
            if (defect[pos] != 0)
            {
                free(defect);
                CSLDestroy(lco);
                return seterr(err, TA_GEO_EINVAL, "more defects than "
                              "features");
            }
            defect[pos] = (char)(k + 1);
        }
    }
    if (n > 0)
    {
        GEOSWKTReader *rd = GEOSWKTReader_create_r(h);
        GEOSGeometry *g = GEOSWKTReader_read_r(h, rd, items[0].text);

        GEOSWKTReader_destroy_r(h, rd);
        if (g != NULL)
        {
            OGRGeometryH og = geo_geos2ogr(g);

            if (og != NULL)
            {
                gt = OGR_G_GetGeometryType(og);
                OGR_G_DestroyGeometry(og);
            }
            GEOSGeom_destroy_r(h, g);
        }
        /* A shapefile has one geometry type; a polygon layer takes
         * multipolygons too. */
        if (strcmp(driver, "ESRI Shapefile") != 0)
            gt = wkbUnknown;
    }

    VSIUnlink(path);
    ds = GDALCreate(drv, path, 0, 0, 0, GDT_Unknown, NULL);
    if (ds == NULL)
    {
        free(defect);
        CSLDestroy(lco);
        return seterr(err, TA_GEO_EIO, "%s: cannot create: %s", path,
                      geo_gdal_err());
    }
    if (crs != NULL && *crs != '\0')
    {
        char norm[256];

        geo_crs_norm(crs, norm, sizeof(norm));
        srs = OSRNewSpatialReference(NULL);
        if (OSRSetFromUserInput(srs, norm) != OGRERR_NONE)
        {
            OSRDestroySpatialReference(srs);
            GDALClose(ds);
            free(defect);
            CSLDestroy(lco);
            return seterr(err, TA_GEO_ENOENT, "CRS '%s' unknown to GDAL",
                          norm);
        }
        OSRSetAxisMappingStrategy(srs, OAMS_TRADITIONAL_GIS_ORDER);
    }
    lay = GDALDatasetCreateLayer(ds, layer, srs, gt, lco);
    CSLDestroy(lco);
    if (srs != NULL)
        OSRDestroySpatialReference(srs);
    if (lay == NULL)
    {
        GDALClose(ds);
        free(defect);
        return seterr(err, TA_GEO_EIO, "cannot create layer: %s",
                      geo_gdal_err());
    }
    fd = OGR_Fld_Create("id", OFTInteger64);
    OGR_L_CreateField(lay, fd, 1);
    OGR_Fld_Destroy(fd);
    fd = OGR_Fld_Create("name", OFTString);
    OGR_Fld_SetWidth(fd, 64);
    OGR_L_CreateField(lay, fd, 1);
    OGR_Fld_Destroy(fd);
    fd = OGR_Fld_Create("value", OFTReal);
    OGR_L_CreateField(lay, fd, 1);
    OGR_Fld_Destroy(fd);
    fd = OGR_Fld_Create("expect", OFTString);
    OGR_Fld_SetWidth(fd, 32);
    OGR_L_CreateField(lay, fd, 1);
    OGR_Fld_Destroy(fd);
    fd = OGR_Fld_Create("t", OFTReal);
    OGR_L_CreateField(lay, fd, 1);
    OGR_Fld_Destroy(fd);

    if (GDALDatasetTestCapability(ds, ODsCTransactions))
        GDALDatasetStartTransaction(ds, 0);
    for (i = 0; i < n; i++)
    {
        OGRFeatureH f = OGR_F_Create(OGR_L_GetLayerDefn(lay));
        OGRGeometryH og = NULL;
        const char *dk = defect[i] != 0 ? plan[defect[i] - 1].name : NULL;
        long long id = items[i].id;
        char name[64];
        double value = (double)(items[i].id % 1000);
        GEOSGeometry *g = NULL;

        if (strcmp(items[i].fmt, "wkt") == 0)
        {
            GEOSWKTReader *rd = GEOSWKTReader_create_r(h);

            g = GEOSWKTReader_read_r(h, rd, items[i].text);
            GEOSWKTReader_destroy_r(h, rd);
        }
        else
        {
            int srid;

            geo_parse_text("wkb", items[i].text, &geo_lim, &g, &srid, NULL);
        }
        if (dk != NULL && strcmp(dk, "dup_id") == 0)
            id = items[i - 1].id;
        snprintf(name, sizeof(name), "f%lld", id);
        if (dk != NULL && g != NULL)
        {
            double b[4];

            if (strcmp(dk, "invalid") == 0 && geo_bounds(g, b))
            {
                GEOSGeom_destroy_r(h, g);
                g = bowtie(b[0], b[1], (b[2] - b[0]) > 0 ?
                                       (b[2] - b[0]) / 2 : 0.001);
            }
            else if (strcmp(dk, "missing_geom") == 0)
            {
                GEOSGeom_destroy_r(h, g);
                g = NULL;
            }
            else if (strcmp(dk, "empty_geom") == 0)
            {
                GEOSGeom_destroy_r(h, g);
                g = GEOSGeom_createEmptyPolygon_r(h);
            }
            else if (strcmp(dk, "out_of_range") == 0 && geo_bounds(g, b))
            {
                /* Far off any sensible extent, in degrees or metres. */
                GEOSGeometry *c = GEOSGeom_createPointFromXY_r(h,
                                                               b[0] + 1e7,
                                                               b[1] + 1e7);
                GEOSGeom_destroy_r(h, g);
                g = c;
            }
            else if (strcmp(dk, "wrong_type") == 0 && geo_bounds(g, b))
            {
                GEOSCoordSequence *s = GEOSCoordSeq_create_r(h, 2, 2);

                GEOSCoordSeq_setXY_r(h, s, 0, b[0], b[1]);
                GEOSCoordSeq_setXY_r(h, s, 1, b[2], b[3]);
                GEOSGeom_destroy_r(h, g);
                g = GEOSGeom_createLineString_r(h, s);
            }
            else if (strcmp(dk, "bad_attr") == 0)
                value = -1;
        }
        OGR_F_SetFieldInteger64(f, 0, id);
        if (dk == NULL || strcmp(dk, "null_attr") != 0)
            OGR_F_SetFieldString(f, 1, name);
        else
            OGR_F_SetFieldNull(f, 1);
        OGR_F_SetFieldDouble(f, 2, value);
        OGR_F_SetFieldString(f, 3, items[i].expect);
        OGR_F_SetFieldDouble(f, 4, items[i].t);
        if (g != NULL)
        {
            og = geo_geos2ogr(g);
            GEOSGeom_destroy_r(h, g);
            if (og != NULL)
                OGR_F_SetGeometryDirectly(f, og);
        }
        if (OGR_L_CreateFeature(lay, f) != OGRERR_NONE)
        {
            OGR_F_Destroy(f);
            GDALClose(ds);
            free(defect);
            sb_free(&planted);
            return seterr(err, TA_GEO_EIO, "feature %zu not written: %s", i,
                          geo_gdal_err());
        }
        if (dk != NULL)
            sb_printf(&planted, "%zu\t%lld\t%s\n", i, id, dk);
        OGR_F_Destroy(f);
        written++;
    }
    if (GDALDatasetTestCapability(ds, ODsCTransactions))
        GDALDatasetCommitTransaction(ds);
    GDALClose(ds);
    free(defect);
    sb_kv_int(res, "written", written);
    sb_kv_str(res, "path", path);
    sb_kv_str(res, "driver", driver);
    for (k = 0; k < nplan; k++)
    {
        char key[48];

        snprintf(key, sizeof(key), "planted_%s", plan[k].name);
        sb_kv_int(res, key, plan[k].count);
    }
    /* The planted defects: position, id, defect. */
    if (planted.p != NULL)
        sb_put(rep, planted.p, planted.len);
    sb_free(&planted);
    return TA_GEO_OK;
}

static int
ds_write(geo_req *r)
{
    const char *k = *r->kind != '\0' ? r->kind : "generate";
    const char *driver = kv_str(r->opt, "driver", "GeoJSON");

    if (*r->text == '\0')
        return seterr(r->err, TA_GEO_EINVAL, "no output path");
    if (strcmp(k, "generate") == 0)
    {
        geo_gen_item *items = NULL;
        size_t n = 0;
        int rc = geo_generate(kv_str(r->opt, "gen", "polygons"), r->opt,
                              &items, &n, r->err);

        if (rc != 0)
            return rc;
        rc = geo_ds_write_items(r->text, driver, kv_str(r->opt, "crs", ""),
                                kv_str(r->opt, "layer_name", "features"),
                                items, n, r->opt, r->res, r->rep, r->err);
        geo_gen_items_free(items, n);
        return rc;
    }
    if (strcmp(k, "copy") == 0)
    {
        geo_ds *src = get_ds(r, r->ha, false);
        char **argv = NULL;
        GDALVectorTranslateOptions *o;
        GDALDatasetH out;
        GDALDatasetH srcs[1];
        int usage = 0;
        const char *dst = kv_get(r->opt, "dst");

        if (src == NULL)
            return TA_GEO_EINVAL;
        argv = CSLAddString(argv, "-f");
        argv = CSLAddString(argv, driver);
        if (dst != NULL)
        {
            char norm[256];

            geo_crs_norm(dst, norm, sizeof(norm));
            argv = CSLAddString(argv, "-t_srs");
            argv = CSLAddString(argv, norm);
        }
        o = GDALVectorTranslateOptionsNew(argv, NULL);
        CSLDestroy(argv);
        if (o == NULL)
            return seterr(r->err, TA_GEO_EINVAL, "bad translate options");
        VSIUnlink(r->text);
        srcs[0] = src->ds;
        out = GDALVectorTranslate(r->text, NULL, 1, srcs, o, &usage);
        GDALVectorTranslateOptionsFree(o);
        if (out == NULL)
            return seterr(r->err, TA_GEO_EIO, "copy to %s failed: %s",
                          r->text, geo_gdal_err());
        GDALClose(out);
        sb_kv_str(r->res, "path", r->text);
        sb_kv_str(r->res, "driver", driver);
        return TA_GEO_OK;
    }
    return seterr(r->err, TA_GEO_EINVAL, "write generate or copy");
}

int
geo_op_ds(ta_geo_op op, geo_req *r)
{
    switch (op)
    {
        case TA_GEO_OP_DS_OPEN: return ds_open(r);
        case TA_GEO_OP_DS_INFO: return ds_info(r);
        case TA_GEO_OP_DS_FETCH: return ds_fetch(r);
        case TA_GEO_OP_DS_VALIDATE: return ds_validate(r);
        case TA_GEO_OP_DS_COMPARE: return ds_compare(r);
        case TA_GEO_OP_DS_WRITE: return ds_write(r);
        default:
            return seterr(r->err, TA_GEO_EINVAL, "not a dataset operation");
    }
}
