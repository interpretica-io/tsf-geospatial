/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: RPC client layer
 *
 * One call per geo_* RPC (see rpcs_geospatial/geospatial.x.m4). Every
 * RPC has the same text-in, text-out shape, so they share the argument
 * and result structures below and one entry point that names the RPC.
 * Tests use tapi_geo.h and its siblings; this is the layer beneath.
 */

#ifndef __TAPI_GEO_RPC_H__
#define __TAPI_GEO_RPC_H__

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "rcf_rpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The geo_* RPCs, in the order of ta_geo_op on the agent. */
typedef enum tapi_geo_rpc_op {
#include "tapi_geo_rpc_ops.h"
    TAPI_GEO_RPC__COUNT
} tapi_geo_rpc_op;

/** Arguments of a geo_* RPC; NULL strings travel as "". */
typedef struct tapi_geo_rpc_args {
    int         ha;         /**< First object handle, or 0. */
    int         hb;         /**< Second object handle, or 0. */
    const char *kind;       /**< Sub-operation. */
    const char *text;       /**< Input text (WKT, path, CRS, URL...). */
    const char *params;     /**< Options, "key=value" lines. */
} tapi_geo_rpc_args;

/** Results of a geo_* RPC. Initialise with TAPI_GEO_RPC_RES_INIT. */
typedef struct tapi_geo_rpc_res {
    int             handle;     /**< New object, or 0. */
    unsigned int    features;   /**< Feature bits (available). */
    te_string       result;     /**< "key=value" lines. */
    te_string       report;     /**< Records, tab-separated fields. */
    te_string       errmsg;     /**< Why it failed. */
} tapi_geo_rpc_res;

#define TAPI_GEO_RPC_RES_INIT \
    { 0, 0, TE_STRING_INIT, TE_STRING_INIT, TE_STRING_INIT }

/** Free the strings of a result and reset it. */
extern void tapi_geo_rpc_res_free(tapi_geo_rpc_res *res);

/** Name of an RPC ("geo_geom_parse"). */
extern const char *tapi_geo_rpc_name(tapi_geo_rpc_op op);

/**
 * Call a geo_* RPC. The RPC's own failure is returned, not jumped on:
 * an input that does not parse is a finding for the test to judge.
 * A transport failure is TE_ECORRUPTED.
 *
 * @param rpcs        RPC server with the geospatial engine.
 * @param op          The RPC.
 * @param args        Its arguments.
 * @param timeout_ms  Timeout of this call, 0 for the default (10 min).
 * @param res         Results (appended to; free with
 *                    tapi_geo_rpc_res_free()).
 */
extern te_errno rpc_geo_call(rcf_rpc_server *rpcs, tapi_geo_rpc_op op,
                             const tapi_geo_rpc_args *args,
                             unsigned int timeout_ms,
                             tapi_geo_rpc_res *res);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_GEO_RPC_H__ */
