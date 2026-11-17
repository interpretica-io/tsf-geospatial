/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Agent-side geospatial operations: TE adapter
 */

#define TE_LGR_USER     "TA GEO"

#include "te_config.h"

#include <string.h>

#include "te_defs.h"
#include "te_errno.h"
#include "logger_api.h"

#include "ta_geo.h"

static te_errno
map_rc(int rc)
{
    switch (rc)
    {
        case TA_GEO_OK: return 0;
        case TA_GEO_EINVAL: return TE_RC(TE_TA_UNIX, TE_EINVAL);
        case TA_GEO_ENOENT: return TE_RC(TE_TA_UNIX, TE_ENOENT);
        case TA_GEO_EIO: return TE_RC(TE_TA_UNIX, TE_EIO);
        case TA_GEO_ECORRUPT: return TE_RC(TE_TA_UNIX, TE_ECORRUPTED);
        case TA_GEO_E2BIG: return TE_RC(TE_TA_UNIX, TE_E2BIG);
        case TA_GEO_ENOMEM: return TE_RC(TE_TA_UNIX, TE_ENOMEM);
        case TA_GEO_ENOTSUP: return TE_RC(TE_TA_UNIX, TE_EOPNOTSUPP);
        case TA_GEO_ETIMEDOUT: return TE_RC(TE_TA_UNIX, TE_ETIMEDOUT);
        case TA_GEO_EDOM: return TE_RC(TE_TA_UNIX, TE_EDOM);
        case TA_GEO_EACCES: return TE_RC(TE_TA_UNIX, TE_EACCES);
        default: return TE_RC(TE_TA_UNIX, TE_EFAIL);
    }
}

/* See description in ta_geo.h */
te_errno
ta_geo_rpc(ta_geo_op op, int ha, int hb, const char *kind, const char *text,
           const char *params, int *handle, unsigned int *features,
           char **result, char **report, char **errmsg)
{
    int rc = ta_geo_call(op, ha, hb, kind, text, params, handle, features,
                         result, report, errmsg);

    /* A refused input is often the point of a test: say it at WARN,
     * not ERROR, so the log does not read like a broken agent. */
    if (rc != 0)
        WARN("geo_%s(%s) failed: %s: %s", ta_geo_op_name(op),
             kind != NULL ? kind : "", ta_geo_strerror(rc), *errmsg);
    return map_rc(rc);
}
