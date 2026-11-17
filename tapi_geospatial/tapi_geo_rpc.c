/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: RPC client layer
 *
 * The rcf_rpc_call() boilerplate behind tapi_geo. The RPCs share one
 * shape, so one macro stamps out the call for each (the list is
 * generated, tapi_geo_rpc_gen.h) and rpc_geo_call() picks it.
 */

#define TE_LGR_USER     "TAPI GEO RPC"

#include "te_config.h"

#include <string.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "logger_api.h"
#include "tapi_rpc_internal.h"
#include "tarpc.h"

#include "tapi_geo_rpc.h"

#define CHECK_RPC_ERRNO_UNCHANGED(_func, _var) \
    CHECK_RETVAL_VAR_ERR_COND(_func, _var, false,                    \
                              TE_RC(TE_TAPI, TE_ECORRUPTED), false)

/* See description in tapi_geo_rpc.h */
void
tapi_geo_rpc_res_free(tapi_geo_rpc_res *res)
{
    te_string_free(&res->result);
    te_string_free(&res->report);
    te_string_free(&res->errmsg);
    res->handle = 0;
    res->features = 0;
}

static char *
s(const char *v)
{
    return (char *)(v != NULL ? v : "");
}

static void
take(te_string *dst, const char *src)
{
    if (src != NULL && *src != '\0')
        te_string_append(dst, "%s", src);
}

/* The first line of a text, for the log (no %.*s: TE's logger refuses
 * it). */
static const char *
first_line(const char *t, char *buf, size_t len)
{
    size_t n = strcspn(t, "\n");

    if (n >= len)
        n = len - 4, memcpy(buf + n, "...", 4);
    else
        buf[n] = '\0';
    memcpy(buf, t, n);
    return buf;
}

#define GEO_RPC(_name) \
static te_errno                                                         \
call_##_name(rcf_rpc_server *rpcs, const tapi_geo_rpc_args *a,          \
             tapi_geo_rpc_res *r)                                       \
{                                                                       \
    tarpc_##_name##_in in;                                              \
    tarpc_##_name##_out out;                                            \
    char line[96];                                                      \
                                                                        \
    memset(&in, 0, sizeof(in));                                         \
    memset(&out, 0, sizeof(out));                                       \
    in.ha = a->ha;                                                      \
    in.hb = a->hb;                                                      \
    in.kind = s(a->kind);                                               \
    in.text = s(a->text);                                               \
    in.params = s(a->params);                                           \
                                                                        \
    rcf_rpc_call(rpcs, #_name, &in, &out);                              \
    CHECK_RPC_ERRNO_UNCHANGED(_name, out.retval);                       \
    TAPI_RPC_LOG(rpcs, _name, "ha=%d hb=%d %s '%s'", "%r handle=%d",    \
                 a->ha, a->hb, s(a->kind),                              \
                 first_line(s(a->text), line, sizeof(line)),            \
                 out.retval, out.handle);                               \
    r->handle = out.handle;                                             \
    r->features = out.features;                                         \
    take(&r->result, out.result);                                       \
    take(&r->report, out.report);                                       \
    take(&r->errmsg, out.errmsg);                                       \
                                                                        \
    RETVAL_TE_ERRNO(_name, out.retval);                                 \
}

#include "tapi_geo_rpc_gen.h"

/* See description in tapi_geo_rpc.h */
const char *
tapi_geo_rpc_name(tapi_geo_rpc_op op)
{
    return (unsigned int)op < TE_ARRAY_LEN(geo_rpcs) ? geo_rpcs[op].name :
                                                       "geo_?";
}

/** Default timeout of a geospatial RPC: datasets stream whole. */
#define TAPI_GEO_RPC_TIMEOUT_MS (10 * 60 * 1000)

/* See description in tapi_geo_rpc.h */
te_errno
rpc_geo_call(rcf_rpc_server *rpcs, tapi_geo_rpc_op op,
             const tapi_geo_rpc_args *args, unsigned int timeout_ms,
             tapi_geo_rpc_res *res)
{
    if ((unsigned int)op >= TE_ARRAY_LEN(geo_rpcs) || rpcs == NULL)
        return TE_RC(TE_TAPI, TE_EINVAL);
    rpcs->timeout = timeout_ms != 0 ? timeout_ms : TAPI_GEO_RPC_TIMEOUT_MS;
    /* The engine's refusals are the test's to judge, not a reason to
     * jump to cleanup. */
    RPC_AWAIT_ERROR(rpcs);
    return geo_rpcs[op].call(rpcs, args, res);
}
