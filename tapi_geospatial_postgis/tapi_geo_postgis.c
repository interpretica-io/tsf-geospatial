/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: PostGIS, through tsf-sql
 */

#define TE_LGR_USER     "TAPI GEO PG"

#include "te_config.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "logger_api.h"

#include "tapi_geo_postgis.h"

/* [schema.]name, identifiers only. */
static bool
ident_ok(const char *s)
{
    const char *p = s;
    int parts = 0;

    if (s == NULL || *s == '\0' || strlen(s) > 127)
        return false;
    while (*p != '\0')
    {
        if (!(isalpha((unsigned char)*p) || *p == '_'))
            return false;
        while (isalnum((unsigned char)*p) || *p == '_')
            p++;
        parts++;
        if (*p == '.')
        {
            p++;
            if (parts > 1)
                return false;
        }
        else if (*p != '\0')
            return false;
    }
    return true;
}

#define IDENT(_s) \
    do {                                                            \
        if (!ident_ok(_s))                                          \
        {                                                           \
            ERROR("'%s' is not an SQL identifier", (_s) != NULL ?   \
                  (_s) : "(null)");                                 \
            return TE_RC(TE_TAPI, TE_EINVAL);                       \
        }                                                           \
    } while (0)

static const char *
cell(const tapi_sql_result *r, unsigned int row, unsigned int col)
{
    const tapi_sql_value *v = tapi_sql_result_cell(r, row, col);

    return v == NULL || v->type == TAPI_SQL_NULL || v->str == NULL ? "" :
                                                                     v->str;
}

/* See description in tapi_geo_postgis.h */
te_errno
tapi_geo_postgis_version(tapi_sql_conn *conn, te_string *version)
{
    tapi_sql_result r = TAPI_SQL_RESULT_INIT;
    te_errno rc = tapi_sql_query(conn, "SELECT postgis_full_version()", &r);

    if (rc == 0 && r.nrows == 1 && version != NULL)
        te_string_append(version, "%s", cell(&r, 0, 0));
    else if (rc == 0)
        rc = TE_RC(TE_TAPI, TE_ENODATA);
    tapi_sql_result_free(&r);
    return rc;
}

/* See description in tapi_geo_postgis.h */
te_errno
tapi_geo_postgis_columns(tapi_sql_conn *conn, tapi_geo_pg_column **cols,
                         unsigned int *n)
{
    tapi_sql_result r = TAPI_SQL_RESULT_INIT;
    te_errno rc;
    unsigned int i;

    *cols = NULL;
    *n = 0;
    rc = tapi_sql_query(conn, "SELECT f_table_schema, f_table_name, "
                        "f_geometry_column, coord_dimension, srid, type "
                        "FROM geometry_columns ORDER BY 1, 2, 3", &r);
    if (rc == 0 && r.nrows > 0)
    {
        *cols = calloc(r.nrows, sizeof(**cols));
        if (*cols == NULL)
            rc = TE_RC(TE_TAPI, TE_ENOMEM);
        for (i = 0; rc == 0 && i < r.nrows; i++)
        {
            tapi_geo_pg_column *c = &(*cols)[i];

            snprintf(c->schema, sizeof(c->schema), "%s", cell(&r, i, 0));
            snprintf(c->table, sizeof(c->table), "%s", cell(&r, i, 1));
            snprintf(c->column, sizeof(c->column), "%s", cell(&r, i, 2));
            c->coord_dimension = atoi(cell(&r, i, 3));
            c->srid = atoi(cell(&r, i, 4));
            snprintf(c->type, sizeof(c->type), "%s", cell(&r, i, 5));
        }
        if (rc == 0)
            *n = r.nrows;
    }
    tapi_sql_result_free(&r);
    return rc;
}

static te_errno
count(tapi_sql_conn *conn, long long *out, const char *fmt, ...)
    TE_LIKE_PRINTF(3, 4);

static te_errno
count(tapi_sql_conn *conn, long long *out, const char *fmt, ...)
{
    te_string q = TE_STRING_INIT;
    va_list ap;
    int64_t v = 0;
    te_errno rc;

    va_start(ap, fmt);
    te_string_append_va(&q, fmt, ap);
    va_end(ap);
    rc = tapi_sql_query_int(conn, q.ptr, &v);
    te_string_free(&q);
    *out = v;
    return rc;
}

/* See description in tapi_geo_postgis.h */
te_errno
tapi_geo_postgis_validate(tapi_sql_conn *conn, const char *table,
                          const char *column, const char *id_column,
                          tapi_geo_pg_validity *v)
{
    tapi_sql_result r = TAPI_SQL_RESULT_INIT;
    te_string q = TE_STRING_INIT;
    char sch[64] = "public", tab[64];
    const char *dot;
    te_errno rc;
    unsigned int i;

    IDENT(table);
    IDENT(column);
    if (id_column != NULL)
        IDENT(id_column);
    memset(v, 0, sizeof(*v));
    v->invalid_samples = (te_string)TE_STRING_INIT;
    dot = strchr(table, '.');
    if (dot != NULL)
    {
        snprintf(sch, sizeof(sch), "%.*s", (int)(dot - table), table);
        snprintf(tab, sizeof(tab), "%s", dot + 1);
    }
    else
        snprintf(tab, sizeof(tab), "%s", table);

    /* The declared SRID and type, from geometry_columns. */
    te_string_append(&q, "SELECT srid, type FROM geometry_columns WHERE "
                     "f_table_schema = ? AND f_table_name = ? AND "
                     "f_geometry_column = ?");
    {
        tapi_sql_value p[3] = { tapi_sql_text(sch), tapi_sql_text(tab),
                                tapi_sql_text(column) };

        rc = tapi_sql_query_params(conn, q.ptr, p, 3, &r, NULL);
    }
    te_string_free(&q);
    if (rc != 0)
        goto out;
    if (r.nrows != 1)
    {
        ERROR("%s.%s is not a registered geometry column", table, column);
        rc = TE_RC(TE_TAPI, TE_ENOENT);
        goto out;
    }
    v->declared_srid = atoi(cell(&r, 0, 0));
    snprintf(v->declared_type, sizeof(v->declared_type), "%s",
             cell(&r, 0, 1));
    tapi_sql_result_free(&r);

    if ((rc = count(conn, &v->rows, "SELECT count(*) FROM %s", table)) != 0 ||
        (rc = count(conn, &v->null, "SELECT count(*) FROM %s WHERE %s IS "
                    "NULL", table, column)) != 0 ||
        (rc = count(conn, &v->empty, "SELECT count(*) FROM %s WHERE "
                    "ST_IsEmpty(%s)", table, column)) != 0 ||
        (rc = count(conn, &v->invalid, "SELECT count(*) FROM %s WHERE NOT "
                    "ST_IsValid(%s)", table, column)) != 0 ||
        (rc = count(conn, &v->distinct_srids, "SELECT count(DISTINCT "
                    "ST_SRID(%s)) FROM %s WHERE %s IS NOT NULL", column,
                    table, column)) != 0)
        goto out;
    if (v->declared_srid != 0 &&
        (rc = count(conn, &v->srid_mismatch, "SELECT count(*) FROM %s WHERE "
                    "ST_SRID(%s) <> %d", table, column,
                    v->declared_srid)) != 0)
        goto out;
    if (strcasecmp(v->declared_type, "GEOMETRY") != 0 &&
        ident_ok(v->declared_type) &&
        (rc = count(conn, &v->type_mismatch, "SELECT count(*) FROM %s WHERE "
                    "%s IS NOT NULL AND upper(GeometryType(%s)) <> "
                    "upper('%s')", table, column, column,
                    v->declared_type)) != 0)
        goto out;
    if (v->invalid > 0)
    {
        te_string_append(&q, "SELECT %s, ST_IsValidReason(%s) FROM %s WHERE "
                         "NOT ST_IsValid(%s) LIMIT 20",
                         id_column != NULL ? id_column : "ctid::text",
                         column, table, column);
        rc = tapi_sql_query(conn, q.ptr, &r);
        te_string_free(&q);
        for (i = 0; rc == 0 && i < r.nrows; i++)
            te_string_append(&v->invalid_samples, "%s\t%s\n", cell(&r, i, 0),
                             cell(&r, i, 1));
    }
out:
    tapi_sql_result_free(&r);
    return rc;
}

void
tapi_geo_pg_validity_free(tapi_geo_pg_validity *v)
{
    te_string_free(&v->invalid_samples);
}

/* See description in tapi_geo_postgis.h */
te_errno
tapi_geo_postgis_index_check(tapi_sql_conn *conn, const char *table,
                             const char *column, bool *has_index,
                             bool *index_used, te_string *plan)
{
    tapi_sql_result r = TAPI_SQL_RESULT_INIT;
    te_string q = TE_STRING_INIT;
    te_errno rc;
    unsigned int i;
    long long n = 0;

    IDENT(table);
    IDENT(column);
    *has_index = false;
    *index_used = false;
    rc = count(conn, &n, "SELECT count(*) FROM pg_index x JOIN pg_class i ON "
               "i.oid = x.indexrelid JOIN pg_am am ON am.oid = i.relam JOIN "
               "pg_attribute a ON a.attrelid = x.indrelid AND a.attnum = "
               "ANY(x.indkey) WHERE x.indrelid = '%s'::regclass AND a.attname "
               "= '%s' AND am.amname IN ('gist', 'spgist', 'brin')", table,
               column);
    if (rc != 0)
        return rc;
    *has_index = n > 0;
    if ((rc = tapi_sql_begin(conn)) != 0)
        return rc;
    rc = tapi_sql_execute(conn, "SET LOCAL enable_seqscan = off", NULL);
    if (rc == 0)
    {
        te_string_append(&q, "EXPLAIN SELECT count(*) FROM %s WHERE %s && "
                         "(SELECT ST_Expand(ST_Centroid(ST_Extent(%s)), 1) "
                         "FROM %s)", table, column, column, table);
        rc = tapi_sql_query(conn, q.ptr, &r);
    }
    for (i = 0; rc == 0 && i < r.nrows; i++)
    {
        const char *l = cell(&r, i, 0);

        if (plan != NULL)
            te_string_append(plan, "%s\n", l);
        if (strstr(l, "Index Scan") != NULL ||
            strstr(l, "Bitmap Index Scan") != NULL ||
            strstr(l, "Index Only Scan") != NULL)
            *index_used = true;
    }
    tapi_sql_result_free(&r);
    te_string_free(&q);
    tapi_sql_rollback(conn);
    return rc;
}

static te_errno
ewkb(rcf_rpc_server *rpcs, const tapi_geo_geometry *g, te_string *out)
{
    return tapi_geo_serialize(rpcs, g, TAPI_GEO_FMT_EWKB, -1, out);
}

/* See description in tapi_geo_postgis.h */
te_errno
tapi_geo_postgis_predicate(tapi_sql_conn *conn, rcf_rpc_server *rpcs,
                           const char *predicate,
                           const tapi_geo_geometry *a,
                           const tapi_geo_geometry *b, bool *result)
{
    static const char *const allowed[] = {
        "ST_Intersects", "ST_Contains", "ST_Within", "ST_Touches",
        "ST_Crosses", "ST_Overlaps", "ST_Disjoint", "ST_Equals",
        "ST_Covers", "ST_CoveredBy", NULL
    };
    te_string ha = TE_STRING_INIT, hb = TE_STRING_INIT, q = TE_STRING_INIT;
    tapi_sql_result r = TAPI_SQL_RESULT_INIT;
    tapi_sql_value p[2];
    te_errno rc;
    size_t i;
    bool ok = false;

    for (i = 0; allowed[i] != NULL; i++)
        ok = ok || strcasecmp(allowed[i], predicate) == 0;
    if (!ok)
        return TE_RC(TE_TAPI, TE_EINVAL);
    if ((rc = ewkb(rpcs, a, &ha)) != 0 || (rc = ewkb(rpcs, b, &hb)) != 0)
        goto out;
    te_string_append(&q, "SELECT %s(ST_GeomFromEWKB(decode(?, 'hex')), "
                     "ST_GeomFromEWKB(decode(?, 'hex')))::int", predicate);
    p[0] = tapi_sql_text(ha.ptr);
    p[1] = tapi_sql_text(hb.ptr);
    rc = tapi_sql_query_params(conn, q.ptr, p, 2, &r, NULL);
    if (rc == 0)
    {
        if (r.nrows != 1)
            rc = TE_RC(TE_TAPI, TE_ENODATA);
        else
            *result = atoi(cell(&r, 0, 0)) != 0;
    }
out:
    tapi_sql_result_free(&r);
    te_string_free(&ha);
    te_string_free(&hb);
    te_string_free(&q);
    return rc;
}

/* See description in tapi_geo_postgis.h */
te_errno
tapi_geo_postgis_fetch(tapi_sql_conn *conn, rcf_rpc_server *rpcs,
                       const char *table, const char *id_column,
                       const char *column, long long id,
                       tapi_geo_geometry **geom)
{
    tapi_sql_result r = TAPI_SQL_RESULT_INIT;
    te_string q = TE_STRING_INIT;
    tapi_sql_value p[1];
    te_errno rc;

    IDENT(table);
    IDENT(id_column);
    IDENT(column);
    *geom = NULL;
    te_string_append(&q, "SELECT encode(ST_AsEWKB(%s), 'hex') FROM %s WHERE "
                     "%s = ?", column, table, id_column);
    p[0] = tapi_sql_int(id);
    rc = tapi_sql_query_params(conn, q.ptr, p, 1, &r, NULL);
    te_string_free(&q);
    if (rc == 0)
    {
        if (r.nrows != 1 || *cell(&r, 0, 0) == '\0')
            rc = TE_RC(TE_TAPI, TE_ENOENT);
        else
            rc = tapi_geo_parse_wkb(rpcs, cell(&r, 0, 0), 0, geom);
    }
    tapi_sql_result_free(&r);
    return rc;
}

/* See description in tapi_geo_postgis.h */
te_errno
tapi_geo_postgis_compare(tapi_sql_conn *conn, rcf_rpc_server *rpcs,
                         const char *table, const char *id_column,
                         const char *column, const long long *ids,
                         tapi_geo_geometry *const *expected, unsigned int n,
                         double tol, tapi_geo_pg_diff *d)
{
    unsigned int i;
    te_errno rc = 0;

    memset(d, 0, sizeof(*d));
    d->records = (te_string)TE_STRING_INIT;
    for (i = 0; i < n && rc == 0; i++)
    {
        tapi_geo_geometry *got = NULL;
        bool same = false;
        double hd = 0;

        rc = tapi_geo_postgis_fetch(conn, rpcs, table, id_column, column,
                                    ids[i], &got);
        if (TE_RC_GET_ERROR(rc) == TE_ENOENT)
        {
            d->missing++;
            te_string_append(&d->records, "%lld\tmissing\t\n", ids[i]);
            rc = 0;
            continue;
        }
        if (rc != 0)
            break;
        d->compared++;
        if (tol > 0)
        {
            tapi_geo_measure m;

            rc = tapi_geo_measure_any(rpcs, "hausdorff", got, expected[i],
                                      TAPI_GEO_PLANAR, TAPI_GEO_AXIS_GIS,
                                      "any_crs=1", &m);
            hd = m.value;
            same = rc == 0 && hd <= tol;
        }
        else
            rc = tapi_geo_predicate(rpcs, "equals", got, expected[i],
                                    "any_crs=1", &same, NULL);
        if (rc == 0 && hd > d->max_hausdorff)
            d->max_hausdorff = hd;
        if (rc == 0 && !same)
        {
            d->different++;
            te_string_append(&d->records, "%lld\tdifferent\t%.17g\n", ids[i],
                             hd);
        }
        tapi_geo_geometry_free(rpcs, got);
    }
    return rc;
}

void
tapi_geo_pg_diff_free(tapi_geo_pg_diff *d)
{
    te_string_free(&d->records);
}
