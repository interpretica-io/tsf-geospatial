/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief One engine operation from the command line, without TE
 *
 * Usage: geo_cli <op> <kind> <text> [params]
 * (params: "key=value" lines, '\n' written as ';;'). Prints the
 * result, then the report after a "--" line; exits with the code.
 * For operations that need no handle: available, gen, crs_info,
 * transform_coords, transform_bounds, ogc, perf (what=transform|parse).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ta_geo_core.h"

int
main(int argc, char **argv)
{
    int op;
    char *params;
    char *p;
    char *res = NULL, *rep = NULL, *err = NULL;
    int h = 0;
    unsigned int f = 0;
    int rc;

    if (argc < 4)
    {
        fprintf(stderr, "usage: %s <op> <kind> <text> [params]\n", argv[0]);
        return 2;
    }
    for (op = 0; op < TA_GEO_OP__COUNT; op++)
        if (strcmp(ta_geo_op_name((ta_geo_op)op), argv[1]) == 0)
            break;
    if (op == TA_GEO_OP__COUNT)
    {
        fprintf(stderr, "unknown op %s\n", argv[1]);
        return 2;
    }
    params = strdup(argc > 4 ? argv[4] : "");
    while ((p = strstr(params, ";;")) != NULL)
    {
        *p = '\n';
        memmove(p + 1, p + 2, strlen(p + 2) + 1);
    }
    rc = ta_geo_call((ta_geo_op)op, 0, 0, argv[2], argv[3], params, &h, &f,
                     &res, &rep, &err);
    printf("%s--\n%s%s", res, rep, rep[0] != '\0' &&
           rep[strlen(rep) - 1] != '\n' ? "\n" : "");
    if (rc != 0)
        fprintf(stderr, "%s: %s\n", ta_geo_strerror(rc), err);
    free(res);
    free(rep);
    free(err);
    free(params);
    ta_geo_shutdown();
    return rc;
}
