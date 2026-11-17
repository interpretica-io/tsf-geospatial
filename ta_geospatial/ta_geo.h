/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Agent-side geospatial operations for the RPC server
 *
 * The TE-typed face of the geospatial engine. The engine itself is the
 * plain-C core in ta_geo_core.h over GEOS, PROJ and GDAL; this layer
 * maps its return codes to te_errno and logs failures. The objects the
 * operations create (geometries, datasets, tracks) live in the RPC
 * server process and die with it: a handle is scoped to the RPC server
 * that issued it.
 */

#ifndef __TA_GEO_H__
#define __TA_GEO_H__

#include "te_defs.h"
#include "te_errno.h"

#include "ta_geo_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Run one operation (see ta_geo_call() in ta_geo_core.h).
 *
 * @return Status code; the core's codes map onto TE_EINVAL, TE_ENOENT,
 *         TE_EIO, TE_ECORRUPTED, TE_E2BIG, TE_ENOMEM, TE_EOPNOTSUPP,
 *         TE_ETIMEDOUT, TE_EDOM, TE_EACCES, TE_EFAIL.
 */
extern te_errno ta_geo_rpc(ta_geo_op op, int ha, int hb, const char *kind,
                           const char *text, const char *params, int *handle,
                           unsigned int *features, char **result,
                           char **report, char **errmsg);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TA_GEO_H__ */
