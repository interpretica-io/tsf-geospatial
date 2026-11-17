/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: assertions with TE verdicts
 */

#define TE_LGR_USER     "TAPI GEO ASSERT"

#include "te_config.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "logger_api.h"
#include "tapi_test_log.h"

#include "tapi_geo_assert.h"

/* Log the evidence, emit the stable verdict, return TE_EFAIL. */
static te_errno
failed(const char *evidence, const char *fmt, ...) TE_LIKE_PRINTF(2, 3);

static te_errno
failed(const char *evidence, const char *fmt, ...)
{
    te_string v = TE_STRING_INIT;
    va_list ap;

    va_start(ap, fmt);
    te_string_append_va(&v, fmt, ap);
    va_end(ap);
    ERROR("Geospatial assertion failed: %s\n%s", v.ptr,
          evidence != NULL ? evidence : "");
    ERROR_VERDICT("%s", v.ptr);
    te_string_free(&v);
    return TE_RC(TE_TAPI, TE_EFAIL);
}

static const char *
nm(const char *what, const char *def)
{
    return what != NULL ? what : def;
}

static void
geom_line(te_string *e, const char *label, rcf_rpc_server *rpcs,
          const tapi_geo_geometry *g)
{
    te_string w = TE_STRING_INIT;

    if (tapi_geo_serialize(rpcs, g, TAPI_GEO_FMT_WKT, 9, &w) == 0)
    {
        if (w.len > 400)
            te_string_cut(&w, w.len - 400);
        te_string_append(e, "%s: %s%s [%s]\n", label, w.ptr,
                         w.len >= 400 ? "..." : "", g->crs);
    }
    te_string_free(&w);
}

te_errno
tapi_geo_assert_valid(rcf_rpc_server *rpcs, const tapi_geo_geometry *g,
                      const char *what)
{
    tapi_geo_validity v;
    te_string e = TE_STRING_INIT;
    te_errno rc = tapi_geo_check_validity(rpcs, g, what, &v);

    if (rc != 0)
        return rc;
    if (!v.ok)
    {
        geom_line(&e, "geometry", rpcs, g);
        te_string_append(&e, "category: %s\nreason: %s\nlocation: %.17g "
                         "%.17g\nissues: %u\n", v.category, v.reason, v.x, v.y,
                         v.issues);
        rc = failed(e.ptr, "%s is not a valid geometry: %s",
                    nm(what, "geometry"), v.category);
    }
    tapi_geo_validity_free(&v);
    te_string_free(&e);
    return rc;
}

te_errno
tapi_geo_assert_type(rcf_rpc_server *rpcs, const tapi_geo_geometry *g,
                     const char *types, const char *what)
{
    tapi_geo_info i;
    te_errno rc = tapi_geo_info_get(rpcs, g, &i);
    te_string e = TE_STRING_INIT;
    char list[256];
    char *tok;
    char *save = NULL;
    bool ok = false;

    if (rc != 0)
        return rc;
    snprintf(list, sizeof(list), "%s", types);
    for (tok = strtok_r(list, ",", &save); tok != NULL;
         tok = strtok_r(NULL, ",", &save))
        ok = ok || strcmp(tok, i.type) == 0;
    if (!ok)
    {
        te_string_append(&e, "type: %s\nexpected: %s\n", i.type, types);
        rc = failed(e.ptr, "%s is a %s, not %s", nm(what, "geometry"),
                    i.type, types);
    }
    te_string_free(&e);
    return rc;
}

static te_errno
pred(rcf_rpc_server *rpcs, const char *p, const tapi_geo_geometry *a,
     const tapi_geo_geometry *b, bool want, const char *wa, const char *wb,
     const char *verdict_rel)
{
    bool v = false;
    char de9im[10] = "";
    te_string e = TE_STRING_INIT;
    te_errno rc = tapi_geo_predicate(rpcs, p, a, b, NULL, &v, de9im);

    if (rc != 0)
        return rc;
    if (v != want)
    {
        geom_line(&e, nm(wa, "a"), rpcs, a);
        geom_line(&e, nm(wb, "b"), rpcs, b);
        te_string_append(&e, "predicate: %s\nexpected: %s\nDE-9IM: %s\n", p,
                         want ? "true" : "false", de9im);
        rc = failed(e.ptr, "%s is %s %s", nm(wa, "geometry"), verdict_rel,
                    nm(wb, "the other geometry"));
    }
    te_string_free(&e);
    return rc;
}

te_errno
tapi_geo_assert_within(rcf_rpc_server *rpcs, const tapi_geo_geometry *a,
                       const tapi_geo_geometry *b, const char *wa,
                       const char *wb)
{
    return pred(rpcs, "within", a, b, true, wa, wb, "not within");
}

te_errno
tapi_geo_assert_contains(rcf_rpc_server *rpcs, const tapi_geo_geometry *a,
                         const tapi_geo_geometry *b, const char *wa,
                         const char *wb)
{
    return pred(rpcs, "contains", a, b, true, wa, wb, "not containing");
}

te_errno
tapi_geo_assert_intersects(rcf_rpc_server *rpcs, const tapi_geo_geometry *a,
                           const tapi_geo_geometry *b, const char *wa,
                           const char *wb)
{
    return pred(rpcs, "intersects", a, b, true, wa, wb, "disjoint from");
}

te_errno
tapi_geo_assert_disjoint(rcf_rpc_server *rpcs, const tapi_geo_geometry *a,
                         const tapi_geo_geometry *b, const char *wa,
                         const char *wb)
{
    return pred(rpcs, "disjoint", a, b, true, wa, wb, "intersecting");
}

te_errno
tapi_geo_assert_distance_le(rcf_rpc_server *rpcs, const tapi_geo_geometry *a,
                            const tapi_geo_geometry *b, tapi_geo_mode mode,
                            tapi_geo_axis axis, double max, const char *what)
{
    tapi_geo_measure m;
    te_string e = TE_STRING_INIT;
    te_errno rc = tapi_geo_distance(rpcs, a, b, mode, axis, &m);

    if (rc != 0)
        return rc;
    if (!(m.value <= max))
    {
        geom_line(&e, "a", rpcs, a);
        geom_line(&e, "b", rpcs, b);
        te_string_append(&e, "distance: %.17g %s\nlimit: %.17g %s\n"
                         "method: %s\n", m.value, m.unit, max, m.unit,
                         m.method);
        rc = failed(e.ptr, "%s: distance exceeds the limit",
                    nm(what, "distance"));
    }
    te_string_free(&e);
    return rc;
}

te_errno
tapi_geo_assert_measure_near(rcf_rpc_server *rpcs, const char *measure,
                             const tapi_geo_geometry *g, tapi_geo_mode mode,
                             tapi_geo_axis axis, double expected, double tol,
                             const char *what)
{
    tapi_geo_measure m;
    te_string e = TE_STRING_INIT;
    te_errno rc = tapi_geo_measure_any(rpcs, measure, g, NULL, mode, axis,
                                       NULL, &m);

    if (rc != 0)
        return rc;
    if (!(fabs(m.value - expected) <= tol))
    {
        geom_line(&e, "geometry", rpcs, g);
        te_string_append(&e, "%s: %.17g %s\nexpected: %.17g +- %g %s\n"
                         "mode: %s\nmethod: %s\n", measure, m.value, m.unit,
                         expected, tol, m.unit,
                         mode == TAPI_GEO_GEODESIC ? "geodesic" : "planar",
                         m.method);
        rc = failed(e.ptr, "%s: %s outside the tolerance",
                    nm(what, "geometry"), measure);
    }
    te_string_free(&e);
    return rc;
}

te_errno
tapi_geo_assert_coord_near(double x, double y, double ex, double ey,
                           double tol, const char *what)
{
    te_string e = TE_STRING_INIT;
    te_errno rc = 0;
    double d = hypot(x - ex, y - ey);

    if (!(d <= tol))
    {
        te_string_append(&e, "got: %.17g %.17g\nexpected: %.17g %.17g\n"
                         "offset: %.17g (tolerance %g)\n", x, y, ex, ey, d,
                         tol);
        rc = failed(e.ptr, "%s: coordinate off the reference",
                    nm(what, "coordinate"));
    }
    te_string_free(&e);
    return rc;
}

te_errno
tapi_geo_assert_bbox_contains(const tapi_geo_bbox *o, const tapi_geo_bbox *i,
                              const char *what)
{
    te_string e = TE_STRING_INIT;
    te_errno rc = 0;

    if (!(i->minx >= o->minx && i->miny >= o->miny && i->maxx <= o->maxx &&
          i->maxy <= o->maxy))
    {
        te_string_append(&e, "outer: %.17g %.17g %.17g %.17g\n"
                         "inner: %.17g %.17g %.17g %.17g\n", o->minx, o->miny,
                         o->maxx, o->maxy, i->minx, i->miny, i->maxx,
                         i->maxy);
        rc = failed(e.ptr, "%s: bounding box not inside the expected one",
                    nm(what, "extent"));
    }
    te_string_free(&e);
    return rc;
}

te_errno
tapi_geo_assert_equals(rcf_rpc_server *rpcs, const tapi_geo_geometry *a,
                       const tapi_geo_geometry *b, double tol,
                       const char *what)
{
    te_string e = TE_STRING_INIT;
    te_errno rc;
    bool eq = false;

    if (tol > 0)
    {
        tapi_geo_measure m;

        rc = tapi_geo_measure_any(rpcs, "hausdorff", a, b, TAPI_GEO_PLANAR,
                                  TAPI_GEO_AXIS_GIS, NULL, &m);
        if (rc != 0)
            return rc;
        eq = m.value <= tol;
        te_string_append(&e, "hausdorff: %.17g %s (tolerance %g)\n", m.value,
                         m.unit, tol);
    }
    else
    {
        rc = tapi_geo_equals(rpcs, a, b, &eq);
        if (rc != 0)
            return rc;
    }
    if (!eq)
    {
        geom_line(&e, "a", rpcs, a);
        geom_line(&e, "b", rpcs, b);
        rc = failed(e.ptr, "%s: geometries differ", nm(what, "geometry"));
    }
    te_string_free(&e);
    return rc;
}

te_errno
tapi_geo_assert_crs_compatible(rcf_rpc_server *rpcs, const char *a,
                               const char *b, const char *what)
{
    tapi_geo_crs_cmp c;
    te_string e = TE_STRING_INIT;
    te_errno rc = tapi_geo_crs_compare(rpcs, a, b, &c);

    if (rc != 0)
        return rc;
    if (!c.equivalent)
    {
        te_string_append(&e, "a: %s\nb: %s\nequivalent except axis order: "
                         "%s\nsame datum: %s\n", a, b,
                         c.equivalent_except_axis ? "yes" : "no",
                         c.same_geodetic ? "yes" : "no");
        rc = failed(e.ptr, "%s: reference systems are not compatible%s",
                    nm(what, "CRS"), c.axis_swapped ?
                    " (axis order differs)" : "");
    }
    te_string_free(&e);
    return rc;
}

te_errno
tapi_geo_assert_accuracy(const tapi_geo_xform_info *i, double max_m,
                         const char *what)
{
    te_string e = TE_STRING_INIT;
    te_errno rc = 0;

    if (i->ballpark || i->accuracy < 0 || i->accuracy > max_m)
    {
        te_string_append(&e, "operation: %s %s\naccuracy: %.3f m (limit "
                         "%.3f)\nballpark: %s\nfallback: %s\ngrids: %s\n",
                         i->operation, i->operation_id, i->accuracy, max_m,
                         i->ballpark ? "yes" : "no",
                         i->fallback ? "yes" : "no", i->grids);
        rc = failed(e.ptr, "%s: transformation accuracy %s",
                    nm(what, "transformation"),
                    i->ballpark ? "is a ballpark" :
                    i->accuracy < 0 ? "is unknown" : "is not good enough");
    }
    te_string_free(&e);
    return rc;
}
