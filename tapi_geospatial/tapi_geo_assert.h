/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: assertions with TE verdicts
 *
 * Each assertion checks one spatial property. When it does not hold,
 * it logs the evidence as a structured block ("key: value" lines at
 * ERROR: what was compared, the measured value, the expectation, the
 * unit, the engine's method), emits a verdict whose text is stable
 * (names, never measured numbers, so TRC can match it), and returns
 * TE_EFAIL. A failure to evaluate (an RPC or engine error) returns that
 * error with no verdict.
 *
 * TAPI_GEO_ASSERT() stops the test on a failed assertion:
 *
 * @code
 * TAPI_GEO_ASSERT(tapi_geo_assert_within(rpcs, point, region,
 *                                        "point", "region"));
 * @endcode
 */

#ifndef __TAPI_GEO_ASSERT_H__
#define __TAPI_GEO_ASSERT_H__

#include "tapi_geo.h"
#include "tapi_geo_crs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Stop the test when an assertion fails (verdict already emitted). */
#define TAPI_GEO_ASSERT(_expr) \
    do {                                                \
        te_errno _rc = (_expr);                         \
        if (_rc != 0)                                   \
        {                                               \
            if (TE_RC_GET_ERROR(_rc) != TE_EFAIL)       \
                TEST_FAIL("%s: %r", #_expr, _rc);       \
            TEST_STOP;                                  \
        }                                               \
    } while (0)

/** @p what names the geometry in the verdict ("parcel"). */
extern te_errno tapi_geo_assert_valid(rcf_rpc_server *rpcs,
                                      const tapi_geo_geometry *g,
                                      const char *what);

/** @p types: "Polygon,MultiPolygon". */
extern te_errno tapi_geo_assert_type(rcf_rpc_server *rpcs,
                                     const tapi_geo_geometry *g,
                                     const char *types, const char *what);

extern te_errno tapi_geo_assert_within(rcf_rpc_server *rpcs,
                                       const tapi_geo_geometry *a,
                                       const tapi_geo_geometry *b,
                                       const char *what_a,
                                       const char *what_b);

extern te_errno tapi_geo_assert_contains(rcf_rpc_server *rpcs,
                                         const tapi_geo_geometry *a,
                                         const tapi_geo_geometry *b,
                                         const char *what_a,
                                         const char *what_b);

extern te_errno tapi_geo_assert_intersects(rcf_rpc_server *rpcs,
                                           const tapi_geo_geometry *a,
                                           const tapi_geo_geometry *b,
                                           const char *what_a,
                                           const char *what_b);

extern te_errno tapi_geo_assert_disjoint(rcf_rpc_server *rpcs,
                                         const tapi_geo_geometry *a,
                                         const tapi_geo_geometry *b,
                                         const char *what_a,
                                         const char *what_b);

/** Distance at most @p max (metres for geodesic, CRS units planar). */
extern te_errno tapi_geo_assert_distance_le(rcf_rpc_server *rpcs,
                                            const tapi_geo_geometry *a,
                                            const tapi_geo_geometry *b,
                                            tapi_geo_mode mode,
                                            tapi_geo_axis axis, double max,
                                            const char *what);

/** @p what: "area", "length", "perimeter" of @p g within
 * |measured - expected| <= tol. */
extern te_errno tapi_geo_assert_measure_near(rcf_rpc_server *rpcs,
                                             const char *measure,
                                             const tapi_geo_geometry *g,
                                             tapi_geo_mode mode,
                                             tapi_geo_axis axis,
                                             double expected, double tol,
                                             const char *what);

/** Two positions within @p tol (planar, same units). No RPC. */
extern te_errno tapi_geo_assert_coord_near(double x, double y, double ex,
                                           double ey, double tol,
                                           const char *what);

/** @p inner inside @p outer (boxes). No RPC. */
extern te_errno tapi_geo_assert_bbox_contains(const tapi_geo_bbox *outer,
                                              const tapi_geo_bbox *inner,
                                              const char *what);

/** Same geometry: topologically, or within Hausdorff @p tol > 0. */
extern te_errno tapi_geo_assert_equals(rcf_rpc_server *rpcs,
                                       const tapi_geo_geometry *a,
                                       const tapi_geo_geometry *b,
                                       double tol, const char *what);

/** The two CRS give the same coordinates (equivalent). */
extern te_errno tapi_geo_assert_crs_compatible(rcf_rpc_server *rpcs,
                                               const char *a, const char *b,
                                               const char *what);

/** A transformation known to be at least as accurate as @p max_m and
 * not a ballpark. */
extern te_errno tapi_geo_assert_accuracy(const tapi_geo_xform_info *info,
                                         double max_m, const char *what);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_GEO_ASSERT_H__ */
