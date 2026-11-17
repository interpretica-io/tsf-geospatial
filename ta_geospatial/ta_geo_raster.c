/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial engine: rasters over GDAL
 *
 * Inspection, windowed reads, statistics and comparison, all tile by
 * tile: at most one tile per band per raster is in memory, whatever the
 * size of the image. And a writer of synthetic GeoTIFFs whose contents
 * are a known function of the pixel position, so their statistics are
 * known in closed form.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gdal_utils.h>

#include "ta_geo_int.h"

static geo_ds *
get_raster(geo_req *r, int h)
{
    geo_ds *d = geo_obj_get(h, GEO_O_DS, r->err);

    if (d != NULL && !d->raster)
    {
        seterr(r->err, TA_GEO_EINVAL, "dataset %d was opened as vector "
               "(mode=raster to read pixels)", h);
        return NULL;
    }
    return d;
}

static void
srs_of(GDALDatasetH ds, char *out, size_t len)
{
    OGRSpatialReferenceH s = GDALGetSpatialRef(ds);
    const char *a, *c;

    out[0] = '\0';
    if (s == NULL)
        return;
    a = OSRGetAuthorityName(s, NULL);
    c = OSRGetAuthorityCode(s, NULL);
    if (a == NULL || c == NULL)
    {
        OSRAutoIdentifyEPSG(s);
        a = OSRGetAuthorityName(s, NULL);
        c = OSRGetAuthorityCode(s, NULL);
    }
    if (a != NULL && c != NULL)
        snprintf(out, len, "%s:%s", a, c);
    else
        snprintf(out, len, "%s", "custom");
}

static int
raster_info(geo_req *r)
{
    geo_ds *d = get_raster(r, r->ha);
    GDALDatasetH ds;
    double gt[6];
    char crs[256];
    int nb;
    int i;
    int w, h;
    bool have_gt;

    if (d == NULL)
        return TA_GEO_EINVAL;
    ds = d->ds;
    w = GDALGetRasterXSize(ds);
    h = GDALGetRasterYSize(ds);
    nb = GDALGetRasterCount(ds);
    have_gt = GDALGetGeoTransform(ds, gt) == CE_None;
    srs_of(ds, crs, sizeof(crs));
    sb_kv_str(r->res, "driver", d->driver);
    sb_kv_int(r->res, "width", w);
    sb_kv_int(r->res, "height", h);
    sb_kv_int(r->res, "bands", nb);
    sb_kv_str(r->res, "crs", crs);
    sb_kv_int(r->res, "georeferenced", have_gt && crs[0] != '\0');
    if (have_gt)
    {
        sb_printf(r->res, "geotransform=");
        for (i = 0; i < 6; i++)
        {
            sb_num(r->res, gt[i]);
            sb_puts(r->res, i < 5 ? " " : "\n");
        }
        sb_kv_num(r->res, "res_x", fabs(gt[1]));
        sb_kv_num(r->res, "res_y", fabs(gt[5]));
        sb_kv_int(r->res, "rotated", gt[2] != 0 || gt[4] != 0);
        /* Extent from the four corners (handles rotation). */
        {
            double xs[4], ys[4];
            double c[4][2] = { { 0, 0 }, { w, 0 }, { 0, h }, { w, h } };
            double mnx = INFINITY, mny = INFINITY, mxx = -INFINITY,
                   mxy = -INFINITY;
            int k;

            for (k = 0; k < 4; k++)
            {
                xs[k] = gt[0] + c[k][0] * gt[1] + c[k][1] * gt[2];
                ys[k] = gt[3] + c[k][0] * gt[4] + c[k][1] * gt[5];
                mnx = fmin(mnx, xs[k]);
                mxx = fmax(mxx, xs[k]);
                mny = fmin(mny, ys[k]);
                mxy = fmax(mxy, ys[k]);
            }
            sb_kv_num(r->res, "minx", mnx);
            sb_kv_num(r->res, "miny", mny);
            sb_kv_num(r->res, "maxx", mxx);
            sb_kv_num(r->res, "maxy", mxy);
        }
    }
    for (i = 1; i <= nb; i++)
    {
        GDALRasterBandH b = GDALGetRasterBand(ds, i);
        int has_nd = 0;
        double nd = GDALGetRasterNoDataValue(b, &has_nd);
        int bx = 0, by = 0;

        GDALGetBlockSize(b, &bx, &by);
        sb_printf(r->rep, "%d\t%s\t", i,
                  GDALGetDataTypeName(GDALGetRasterDataType(b)));
        if (has_nd)
            sb_num(r->rep, nd);
        sb_printf(r->rep, "\t%d\t%d\t%d\t", bx, by,
                  GDALGetOverviewCount(b));
        sb_field(r->rep, GDALGetColorInterpretationName(
                             GDALGetRasterColorInterpretation(b)));
        sb_puts(r->rep, "\n");
        if (i == 1)
        {
            sb_kv_str(r->res, "type",
                      GDALGetDataTypeName(GDALGetRasterDataType(b)));
            sb_kv_int(r->res, "has_nodata", has_nd);
            if (has_nd)
                sb_kv_num(r->res, "nodata", nd);
            sb_kv_int(r->res, "block_x", bx);
            sb_kv_int(r->res, "block_y", by);
            sb_kv_int(r->res, "overviews", GDALGetOverviewCount(b));
        }
    }
    {
        char **md = GDALGetMetadata(ds, "IMAGE_STRUCTURE");
        const char *comp = CSLFetchNameValue(md, "COMPRESSION");
        const char *layout = CSLFetchNameValue(md, "LAYOUT");

        sb_kv_str(r->res, "compression", comp != NULL ? comp : "");
        sb_kv_str(r->res, "layout", layout != NULL ? layout : "");
    }
    return TA_GEO_OK;
}

/* ---- tile walking ------------------------------------------------- */

typedef struct tstat {
    long long valid;
    long long nodata;
    long long nonfinite;
    double min, max;
    double mean, m2;        /* Welford */
} tstat;

static void
tstat_add(tstat *s, double v)
{
    double d;

    s->valid++;
    if (s->valid == 1 || v < s->min)
        s->min = v;
    if (s->valid == 1 || v > s->max)
        s->max = v;
    d = v - s->mean;
    s->mean += d / (double)s->valid;
    s->m2 += d * (v - s->mean);
}

static int
tile_size(kv *opt, GDALRasterBandH b, int *tx, int *ty, char **err)
{
    long long t;
    int rc;

    if ((rc = kv_long(opt, "tile", 0, &t, err)) != 0)
        return rc;
    if (t > 0)
    {
        *tx = *ty = (int)t;
        return TA_GEO_OK;
    }
    GDALGetBlockSize(b, tx, ty);
    /* Strips of one line are slow to read one by one: group them. */
    if (*ty == 1)
        *ty = 64;
    if (*tx <= 0 || *ty <= 0)
        *tx = *ty = 256;
    return TA_GEO_OK;
}

static int
band_arg(geo_req *r, GDALDatasetH ds, long long *band)
{
    int rc = kv_long(r->opt, "band", 1, band, r->err);

    if (rc != 0)
        return rc;
    if (*band < 1 || *band > GDALGetRasterCount(ds))
        return seterr(r->err, TA_GEO_EINVAL, "no band %lld (1..%d)", *band,
                      GDALGetRasterCount(ds));
    return TA_GEO_OK;
}

static int
raster_stats(geo_req *r)
{
    geo_ds *d = get_raster(r, r->ha);
    GDALRasterBandH b;
    long long band;
    int has_nd = 0;
    double nd;
    int w, h, tx, ty, x, y;
    double *buf;
    tstat s;
    uint64_t t0 = geo_now_ns();
    double us;
    int rc;

    if (d == NULL)
        return TA_GEO_EINVAL;
    if ((rc = band_arg(r, d->ds, &band)) != 0)
        return rc;
    b = GDALGetRasterBand(d->ds, (int)band);
    nd = GDALGetRasterNoDataValue(b, &has_nd);
    w = GDALGetRasterXSize(d->ds);
    h = GDALGetRasterYSize(d->ds);
    if ((rc = tile_size(r->opt, b, &tx, &ty, r->err)) != 0)
        return rc;
    if ((long long)tx * ty > geo_lim.max_pixels)
        return seterr(r->err, TA_GEO_E2BIG, "tile of %dx%d over max_pixels",
                      tx, ty);
    buf = malloc((size_t)tx * (size_t)ty * sizeof(*buf));
    if (buf == NULL)
        return seterr(r->err, TA_GEO_ENOMEM, "out of memory");
    memset(&s, 0, sizeof(s));
    for (y = 0; y < h; y += ty)
    {
        for (x = 0; x < w; x += tx)
        {
            int cw = x + tx > w ? w - x : tx;
            int ch = y + ty > h ? h - y : ty;
            int k;

            if (geo_deadline_passed())
            {
                free(buf);
                return seterr(r->err, TA_GEO_ETIMEDOUT, "statistics ran "
                              "past the deadline");
            }
            if (GDALRasterIO(b, GF_Read, x, y, cw, ch, buf, cw, ch,
                             GDT_Float64, 0, 0) != CE_None)
            {
                free(buf);
                return seterr(r->err, TA_GEO_EIO, "read at %d,%d: %s", x, y,
                              geo_gdal_err());
            }
            for (k = 0; k < cw * ch; k++)
            {
                double v = buf[k];

                if (has_nd && (v == nd || (isnan(nd) && isnan(v))))
                    s.nodata++;
                else if (!isfinite(v))
                    s.nonfinite++;
                else
                    tstat_add(&s, v);
            }
        }
    }
    free(buf);
    us = geo_us_since(t0);
    sb_kv_int(r->res, "band", band);
    sb_kv_int(r->res, "pixels", (long long)w * h);
    sb_kv_int(r->res, "valid", s.valid);
    sb_kv_int(r->res, "nodata", s.nodata);
    sb_kv_int(r->res, "nonfinite", s.nonfinite);
    sb_kv_num(r->res, "min", s.valid > 0 ? s.min : NAN);
    sb_kv_num(r->res, "max", s.valid > 0 ? s.max : NAN);
    sb_kv_num(r->res, "mean", s.valid > 0 ? s.mean : NAN);
    sb_kv_num(r->res, "stddev", s.valid > 0 ?
                                sqrt(s.m2 / (double)s.valid) : NAN);
    sb_kv_int(r->res, "tile_x", tx);
    sb_kv_int(r->res, "tile_y", ty);
    sb_kv_num(r->res, "elapsed_us", us);
    sb_kv_num(r->res, "mpix_per_s", us > 0 ? (double)w * h / us : 0);
    return TA_GEO_OK;
}

static int
raster_read(geo_req *r)
{
    geo_ds *d = get_raster(r, r->ha);
    GDALRasterBandH b;
    long long band, x0, y0, xs, ys;
    double *buf;
    int has_nd = 0;
    double nd;
    tstat s;
    long long i, j;
    int rc;

    if (d == NULL)
        return TA_GEO_EINVAL;
    if ((rc = band_arg(r, d->ds, &band)) != 0 ||
        (rc = kv_long(r->opt, "x", 0, &x0, r->err)) != 0 ||
        (rc = kv_long(r->opt, "y", 0, &y0, r->err)) != 0 ||
        (rc = kv_long(r->opt, "width", 1, &xs, r->err)) != 0 ||
        (rc = kv_long(r->opt, "height", 1, &ys, r->err)) != 0)
        return rc;
    if (x0 < 0 || y0 < 0 || xs < 1 || ys < 1 ||
        x0 + xs > GDALGetRasterXSize(d->ds) ||
        y0 + ys > GDALGetRasterYSize(d->ds))
        return seterr(r->err, TA_GEO_EINVAL, "window %lld,%lld %lldx%lld "
                      "outside the %dx%d raster", x0, y0, xs, ys,
                      GDALGetRasterXSize(d->ds), GDALGetRasterYSize(d->ds));
    if (xs * ys > geo_lim.max_pixels || xs * ys > geo_lim.max_report * 64)
        return seterr(r->err, TA_GEO_E2BIG, "window of %lld pixels is too "
                      "large to return", xs * ys);
    b = GDALGetRasterBand(d->ds, (int)band);
    nd = GDALGetRasterNoDataValue(b, &has_nd);
    buf = malloc((size_t)(xs * ys) * sizeof(*buf));
    if (buf == NULL)
        return seterr(r->err, TA_GEO_ENOMEM, "out of memory");
    if (GDALRasterIO(b, GF_Read, (int)x0, (int)y0, (int)xs, (int)ys, buf,
                     (int)xs, (int)ys, GDT_Float64, 0, 0) != CE_None)
    {
        free(buf);
        return seterr(r->err, TA_GEO_EIO, "read: %s", geo_gdal_err());
    }
    memset(&s, 0, sizeof(s));
    for (j = 0; j < ys; j++)
    {
        for (i = 0; i < xs; i++)
        {
            double v = buf[j * xs + i];

            if (i > 0)
                sb_puts(r->rep, " ");
            sb_num(r->rep, v);
            if (has_nd && v == nd)
                s.nodata++;
            else if (isfinite(v))
                tstat_add(&s, v);
        }
        sb_puts(r->rep, "\n");
    }
    free(buf);
    sb_kv_int(r->res, "pixels", xs * ys);
    sb_kv_int(r->res, "valid", s.valid);
    sb_kv_int(r->res, "nodata", s.nodata);
    sb_kv_num(r->res, "min", s.valid > 0 ? s.min : NAN);
    sb_kv_num(r->res, "max", s.valid > 0 ? s.max : NAN);
    sb_kv_num(r->res, "mean", s.valid > 0 ? s.mean : NAN);
    return TA_GEO_OK;
}

static int
raster_compare(geo_req *r)
{
    geo_ds *a = get_raster(r, r->ha);
    geo_ds *bd = a != NULL ? get_raster(r, r->hb) : NULL;
    double tol, gt_tol;
    int w, h, nb, band, x, y, tx, ty;
    double ga[6], gb[6];
    bool ha, hb;
    char ca[256], cb[256];
    long long meta_diff = 0;
    long long ndiff = 0, nd_mismatch = 0, compared = 0, rep_n = 0;
    double max_abs = 0, sum_sq = 0, sum_d = 0;
    double *ba = NULL, *bb = NULL;
    int rc;

    if (a == NULL || bd == NULL)
        return TA_GEO_EINVAL;
    if ((rc = kv_dbl(r->opt, "tolerance", 0, &tol, r->err)) != 0 ||
        (rc = kv_dbl(r->opt, "geotransform_tolerance", 1e-9, &gt_tol,
                     r->err)) != 0)
        return rc;

#define META(_cond, ...) \
    do {                                                        \
        if (!(_cond))                                           \
        {                                                       \
            meta_diff++;                                        \
            if (rep_n++ < geo_lim.max_report)                   \
            {                                                   \
                sb_puts(r->rep, "meta\t");                      \
                sb_printf(r->rep, __VA_ARGS__);                 \
                sb_puts(r->rep, "\n");                          \
            }                                                   \
        }                                                       \
    } while (0)

    w = GDALGetRasterXSize(a->ds);
    h = GDALGetRasterYSize(a->ds);
    nb = GDALGetRasterCount(a->ds);
    META(w == GDALGetRasterXSize(bd->ds) && h == GDALGetRasterYSize(bd->ds),
         "size %dx%d vs %dx%d", w, h, GDALGetRasterXSize(bd->ds),
         GDALGetRasterYSize(bd->ds));
    META(nb == GDALGetRasterCount(bd->ds), "bands %d vs %d", nb,
         GDALGetRasterCount(bd->ds));
    ha = GDALGetGeoTransform(a->ds, ga) == CE_None;
    hb = GDALGetGeoTransform(bd->ds, gb) == CE_None;
    META(ha == hb, "georeferenced %d vs %d", ha, hb);
    if (ha && hb)
    {
        int k;
        bool same = true;

        for (k = 0; k < 6; k++)
            same = same && fabs(ga[k] - gb[k]) <= gt_tol *
                                fmax(1, fmax(fabs(ga[k]), fabs(gb[k])));
        META(same, "geotransform %.17g %.17g %.17g %.17g %.17g %.17g vs "
             "%.17g %.17g %.17g %.17g %.17g %.17g", ga[0], ga[1], ga[2],
             ga[3], ga[4], ga[5], gb[0], gb[1], gb[2], gb[3], gb[4], gb[5]);
    }
    srs_of(a->ds, ca, sizeof(ca));
    srs_of(bd->ds, cb, sizeof(cb));
    if (strcmp(ca, cb) != 0)
    {
        OGRSpatialReferenceH sa = GDALGetSpatialRef(a->ds);
        OGRSpatialReferenceH sb2 = GDALGetSpatialRef(bd->ds);

        META(sa != NULL && sb2 != NULL && OSRIsSame(sa, sb2), "crs %s vs %s",
             ca, cb);
    }
    if (w != GDALGetRasterXSize(bd->ds) || h != GDALGetRasterYSize(bd->ds) ||
        nb != GDALGetRasterCount(bd->ds))
        goto done;

    for (band = 1; band <= nb; band++)
    {
        GDALRasterBandH ra = GDALGetRasterBand(a->ds, band);
        GDALRasterBandH rb = GDALGetRasterBand(bd->ds, band);
        int hna = 0, hnb = 0;
        double nda = GDALGetRasterNoDataValue(ra, &hna);
        double ndb = GDALGetRasterNoDataValue(rb, &hnb);

        META(GDALGetRasterDataType(ra) == GDALGetRasterDataType(rb),
             "band %d type %s vs %s", band,
             GDALGetDataTypeName(GDALGetRasterDataType(ra)),
             GDALGetDataTypeName(GDALGetRasterDataType(rb)));
        META(hna == hnb && (!hna || nda == ndb || (isnan(nda) && isnan(ndb))),
             "band %d nodata %s%.17g vs %s%.17g", band, hna ? "" : "none ",
             nda, hnb ? "" : "none ", ndb);
        if ((rc = tile_size(r->opt, ra, &tx, &ty, r->err)) != 0)
            goto out;
        if ((long long)tx * ty > geo_lim.max_pixels)
        {
            rc = seterr(r->err, TA_GEO_E2BIG, "tile over max_pixels");
            goto out;
        }
        free(ba);
        free(bb);
        ba = malloc((size_t)tx * (size_t)ty * sizeof(*ba));
        bb = malloc((size_t)tx * (size_t)ty * sizeof(*bb));
        if (ba == NULL || bb == NULL)
        {
            rc = seterr(r->err, TA_GEO_ENOMEM, "out of memory");
            goto out;
        }
        for (y = 0; y < h; y += ty)
        {
            for (x = 0; x < w; x += tx)
            {
                int cw = x + tx > w ? w - x : tx;
                int ch = y + ty > h ? h - y : ty;
                int k;

                if (geo_deadline_passed())
                {
                    rc = seterr(r->err, TA_GEO_ETIMEDOUT, "comparison ran "
                                "past the deadline");
                    goto out;
                }
                if (GDALRasterIO(ra, GF_Read, x, y, cw, ch, ba, cw, ch,
                                 GDT_Float64, 0, 0) != CE_None ||
                    GDALRasterIO(rb, GF_Read, x, y, cw, ch, bb, cw, ch,
                                 GDT_Float64, 0, 0) != CE_None)
                {
                    rc = seterr(r->err, TA_GEO_EIO, "read at %d,%d: %s", x,
                                y, geo_gdal_err());
                    goto out;
                }
                for (k = 0; k < cw * ch; k++)
                {
                    bool na = hna && ba[k] == nda;
                    bool nbb = hnb && bb[k] == ndb;
                    double dd;

                    if (na || nbb)
                    {
                        if (na != nbb)
                        {
                            nd_mismatch++;
                            if (rep_n++ < geo_lim.max_report)
                                sb_printf(r->rep, "nodata\t%d\t%d\t%d\t"
                                          "%.17g\t%.17g\n", band, x + k % cw,
                                          y + k / cw, ba[k], bb[k]);
                        }
                        continue;
                    }
                    compared++;
                    dd = ba[k] - bb[k];
                    sum_d += dd;
                    sum_sq += dd * dd;
                    if (fabs(dd) > max_abs)
                        max_abs = fabs(dd);
                    if (fabs(dd) > tol)
                    {
                        ndiff++;
                        if (rep_n++ < geo_lim.max_report)
                            sb_printf(r->rep, "pixel\t%d\t%d\t%d\t%.17g\t"
                                      "%.17g\n", band, x + k % cw,
                                      y + k / cw, ba[k], bb[k]);
                    }
                }
            }
        }
    }
#undef META

done:
    rc = TA_GEO_OK;
out:
    free(ba);
    free(bb);
    sb_kv_int(r->res, "metadata_diff", meta_diff);
    sb_kv_int(r->res, "compared", compared);
    sb_kv_int(r->res, "pixel_diff", ndiff);
    sb_kv_int(r->res, "nodata_mismatch", nd_mismatch);
    sb_kv_num(r->res, "max_abs_diff", max_abs);
    sb_kv_num(r->res, "mean_diff", compared > 0 ? sum_d / compared : 0);
    sb_kv_num(r->res, "rmse", compared > 0 ? sqrt(sum_sq / compared) : 0);
    sb_kv_int(r->res, "equal", rc == 0 && meta_diff == 0 && ndiff == 0 &&
              nd_mismatch == 0);
    return rc;
}

/* ---- writing ------------------------------------------------------ */

/*
 * Pattern values, as functions of (column x, row y, band b):
 *   gradient: a + bx * x + by * y + 1000 * (b - 1)
 *   checker:  ((x / cell + y / cell) % 2) * hi
 *   constant: a
 *   random:   uniform in [lo, hi), seeded, row-major order
 * Every nodata_every-th pixel (row-major, counting from 0, the first
 * one included) is set to the nodata value.
 */
static int
raster_write(geo_req *r)
{
    const char *driver = kv_str(r->opt, "driver", "GTiff");
    const char *pattern = kv_str(r->opt, "pattern", "gradient");
    const char *type = kv_str(r->opt, "type", "Float32");
    const char *crs = kv_str(r->opt, "crs", "EPSG:4326");
    const char *gts = kv_get(r->opt, "geotransform");
    long long w, h, nbands, nd_every, cell, seed, block, noise_n;
    double a, bx, by, lo, hi, nodata, noise;
    bool has_nd;
    bool tiled;
    double gt[6] = { 0, 1, 0, 0, 0, -1 };
    GDALDriverH drv;
    GDALDriverH mem;
    GDALDatasetH ds;
    GDALDataType dt;
    char **co = NULL;
    char bs[32];
    float *row = NULL;
    double *drow = NULL;
    geo_rng rng;
    geo_rng nrng;
    long long x, y, band;
    size_t i;
    int rc;
    bool via_copy;

    if (*r->text == '\0')
        return seterr(r->err, TA_GEO_EINVAL, "no output path");
    if ((rc = kv_long(r->opt, "width", 256, &w, r->err)) != 0 ||
        (rc = kv_long(r->opt, "height", 256, &h, r->err)) != 0 ||
        (rc = kv_long(r->opt, "bands", 1, &nbands, r->err)) != 0 ||
        (rc = kv_long(r->opt, "nodata_every", 0, &nd_every, r->err)) != 0 ||
        (rc = kv_long(r->opt, "cell", 16, &cell, r->err)) != 0 ||
        (rc = kv_long(r->opt, "seed", 1, &seed, r->err)) != 0 ||
        (rc = kv_long(r->opt, "block", 256, &block, r->err)) != 0 ||
        (rc = kv_long(r->opt, "noise_pixels", 0, &noise_n, r->err)) != 0 ||
        (rc = kv_dbl(r->opt, "a", 0, &a, r->err)) != 0 ||
        (rc = kv_dbl(r->opt, "bx", 1, &bx, r->err)) != 0 ||
        (rc = kv_dbl(r->opt, "by", 0, &by, r->err)) != 0 ||
        (rc = kv_dbl(r->opt, "lo", 0, &lo, r->err)) != 0 ||
        (rc = kv_dbl(r->opt, "hi", 255, &hi, r->err)) != 0 ||
        (rc = kv_dbl(r->opt, "noise", 0, &noise, r->err)) != 0 ||
        (rc = kv_dbl(r->opt, "nodata", NAN, &nodata, r->err)) != 0 ||
        (rc = kv_bool(r->opt, "tiled", true, &tiled, r->err)) != 0)
        return rc;
    has_nd = !isnan(nodata);
    if (w < 1 || h < 1 || nbands < 1 || nbands > 64 || cell < 1 ||
        w * h > 4LL * 1024 * 1024 * 1024)
        return seterr(r->err, TA_GEO_EINVAL, "bad raster dimensions");
    if (gts != NULL &&
        sscanf(gts, "%lf %lf %lf %lf %lf %lf", &gt[0], &gt[1], &gt[2],
               &gt[3], &gt[4], &gt[5]) != 6)
        return seterr(r->err, TA_GEO_EINVAL, "geotransform is six numbers");
    dt = GDALGetDataTypeByName(type);
    if (dt == GDT_Unknown)
        return seterr(r->err, TA_GEO_EINVAL, "unknown pixel type '%s'", type);
    if (strcmp(pattern, "gradient") != 0 && strcmp(pattern, "checker") != 0 &&
        strcmp(pattern, "constant") != 0 && strcmp(pattern, "random") != 0)
        return seterr(r->err, TA_GEO_EINVAL, "pattern is gradient, checker, "
                      "constant or random");
    drv = GDALGetDriverByName(driver);
    if (drv == NULL)
        return seterr(r->err, TA_GEO_ENOTSUP, "no raster driver '%s'",
                      driver);
    /* Drivers without Create (PNG, MBTiles, COG) are written through an
     * in-memory dataset and CreateCopy. */
    via_copy = GDALGetMetadataItem(drv, GDAL_DCAP_CREATE, NULL) == NULL;
    if (via_copy &&
        GDALGetMetadataItem(drv, GDAL_DCAP_CREATECOPY, NULL) == NULL)
        return seterr(r->err, TA_GEO_ENOTSUP, "driver '%s' writes nothing",
                      driver);
    for (i = 0; i < r->opt->n; i++)
    {
        if (strncmp(r->opt->v[i].key, "co.", 3) == 0)
        {
            r->opt->v[i].used = true;
            co = CSLSetNameValue(co, r->opt->v[i].key + 3, r->opt->v[i].val);
        }
    }
    if (strcmp(driver, "GTiff") == 0 && tiled)
    {
        snprintf(bs, sizeof(bs), "%lld", block);
        co = CSLSetNameValue(co, "TILED", "YES");
        co = CSLSetNameValue(co, "BLOCKXSIZE", bs);
        co = CSLSetNameValue(co, "BLOCKYSIZE", bs);
    }
    VSIUnlink(r->text);
    mem = via_copy ? GDALGetDriverByName("MEM") : drv;
    ds = GDALCreate(mem, via_copy ? "" : r->text, (int)w, (int)h,
                    (int)nbands, dt, via_copy ? NULL : co);
    if (ds == NULL)
    {
        CSLDestroy(co);
        return seterr(r->err, TA_GEO_EIO, "%s: cannot create: %s", r->text,
                      geo_gdal_err());
    }
    if (crs[0] != '\0')
    {
        OGRSpatialReferenceH s = OSRNewSpatialReference(NULL);
        char norm[256];

        geo_crs_norm(crs, norm, sizeof(norm));
        if (OSRSetFromUserInput(s, norm) != OGRERR_NONE)
        {
            OSRDestroySpatialReference(s);
            GDALClose(ds);
            CSLDestroy(co);
            return seterr(r->err, TA_GEO_ENOENT, "CRS '%s' unknown", norm);
        }
        GDALSetSpatialRef(ds, s);
        OSRDestroySpatialReference(s);
    }
    GDALSetGeoTransform(ds, gt);
    drow = malloc((size_t)w * sizeof(*drow));
    row = (float *)drow;
    if (drow == NULL)
    {
        GDALClose(ds);
        CSLDestroy(co);
        return seterr(r->err, TA_GEO_ENOMEM, "out of memory");
    }
    for (band = 1; band <= nbands; band++)
    {
        GDALRasterBandH rb = GDALGetRasterBand(ds, (int)band);

        geo_rng_seed(&rng, (uint64_t)seed + (uint64_t)band);
        geo_rng_seed(&nrng, (uint64_t)seed * 7919 + (uint64_t)band);
        if (has_nd)
            GDALSetRasterNoDataValue(rb, nodata);
        for (y = 0; y < h; y++)
        {
            for (x = 0; x < w; x++)
            {
                long long idx = y * w + x;
                double v;

                if (strcmp(pattern, "gradient") == 0)
                    v = a + bx * (double)x + by * (double)y +
                        1000.0 * (double)(band - 1);
                else if (strcmp(pattern, "checker") == 0)
                    v = (double)(((x / cell) + (y / cell)) % 2) * hi;
                else if (strcmp(pattern, "constant") == 0)
                    v = a;
                else
                    v = geo_rng_range(&rng, lo, hi);
                if (has_nd && nd_every > 0 && idx % nd_every == 0)
                    v = nodata;
                drow[x] = v;
            }
            if (GDALRasterIO(rb, GF_Write, 0, (int)y, (int)w, 1, drow,
                             (int)w, 1, GDT_Float64, 0, 0) != CE_None)
            {
                free(drow);
                GDALClose(ds);
                CSLDestroy(co);
                return seterr(r->err, TA_GEO_EIO, "write: %s",
                              geo_gdal_err());
            }
        }
        /* Perturbation: noise_pixels pixels (seeded positions, never a
         * nodata one) get +noise added. */
        for (i = 0; (long long)i < noise_n; i++)
        {
            long long px = (long long)(geo_rng_next(&nrng) % (uint64_t)w);
            long long py = (long long)(geo_rng_next(&nrng) % (uint64_t)h);
            double v;

            if (has_nd && nd_every > 0 && (py * w + px) % nd_every == 0)
                continue;
            if (GDALRasterIO(rb, GF_Read, (int)px, (int)py, 1, 1, &v, 1, 1,
                             GDT_Float64, 0, 0) != CE_None)
                continue;
            v += noise;
            if (GDALRasterIO(rb, GF_Write, (int)px, (int)py, 1, 1, &v, 1, 1,
                             GDT_Float64, 0, 0) != CE_None)
                continue;
        }
    }
    (void)row;
    free(drow);
    if (via_copy)
    {
        GDALDatasetH out = GDALCreateCopy(drv, r->text, ds, 0, co, NULL,
                                          NULL);

        GDALClose(ds);
        CSLDestroy(co);
        if (out == NULL)
            return seterr(r->err, TA_GEO_EIO, "%s: copy failed: %s",
                          r->text, geo_gdal_err());
        GDALClose(out);
    }
    else
    {
        CSLDestroy(co);
        GDALClose(ds);
    }
    sb_kv_str(r->res, "path", r->text);
    sb_kv_int(r->res, "width", w);
    sb_kv_int(r->res, "height", h);
    sb_kv_int(r->res, "bands", nbands);
    sb_kv_str(r->res, "type", type);
    return TA_GEO_OK;
}

int
geo_op_raster(ta_geo_op op, geo_req *r)
{
    switch (op)
    {
        case TA_GEO_OP_RASTER_INFO: return raster_info(r);
        case TA_GEO_OP_RASTER_READ: return raster_read(r);
        case TA_GEO_OP_RASTER_STATS: return raster_stats(r);
        case TA_GEO_OP_RASTER_COMPARE: return raster_compare(r);
        case TA_GEO_OP_RASTER_WRITE: return raster_write(r);
        default:
            return seterr(r->err, TA_GEO_EINVAL, "not a raster operation");
    }
}
