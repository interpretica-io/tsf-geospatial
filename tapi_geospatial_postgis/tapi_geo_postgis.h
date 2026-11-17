/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica, Unipessoal Lda. All rights reserved. */
/** @file
 * @brief Geospatial TAPI: PostGIS, through tsf-sql
 *
 * Spatial checks of a PostgreSQL/PostGIS database. Connections are
 * tsf-sql's (tapi_sql_connect()); nothing here opens one. Geometries
 * read from the database are compared with geometries in the agent's
 * geospatial engine, so the database's answer and GEOS's answer to the
 * same question can be held against each other.
 *
 * Table and column names are SQL identifiers ([A-Za-z_][A-Za-z0-9_]*,
 * optionally schema-qualified); anything else is refused, so a test
 * parameter cannot become SQL. Geometries travel as bound parameters.
 *
 * This library links tapi_sql (tsf-sql) and tapi_geospatial; declare
 * both repositories to use it.
 */

#ifndef __TAPI_GEO_POSTGIS_H__
#define __TAPI_GEO_POSTGIS_H__

#include "tapi_sql.h"
#include "tapi_geo.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Is PostGIS installed in the connected database (and which)? */
extern te_errno tapi_geo_postgis_version(tapi_sql_conn *conn,
                                         te_string *version);

/** A spatial column (geometry_columns). */
typedef struct tapi_geo_pg_column {
    char schema[64];
    char table[64];
    char column[64];
    int coord_dimension;
    int srid;
    char type[32];
} tapi_geo_pg_column;

extern te_errno tapi_geo_postgis_columns(tapi_sql_conn *conn,
                                         tapi_geo_pg_column **cols,
                                         unsigned int *n);

/** What is stored in a column. */
typedef struct tapi_geo_pg_validity {
    long long rows;
    long long null;
    long long empty;
    long long invalid;          /**< NOT ST_IsValid */
    long long srid_mismatch;    /**< ST_SRID <> the declared SRID */
    long long type_mismatch;    /**< against the declared type */
    long long distinct_srids;
    int declared_srid;
    char declared_type[32];
    te_string invalid_samples;  /**< "id TAB ST_IsValidReason" */
} tapi_geo_pg_validity;

/** @p id_column names rows in the samples (may be @c NULL). */
extern te_errno tapi_geo_postgis_validate(tapi_sql_conn *conn,
                                          const char *table,
                                          const char *column,
                                          const char *id_column,
                                          tapi_geo_pg_validity *v);

extern void tapi_geo_pg_validity_free(tapi_geo_pg_validity *v);

/**
 * Is there a spatial (GiST/SP-GiST/BRIN) index on the column, and does
 * the planner use it for a bounding-box query when it can (sequential
 * scans disabled)?
 */
extern te_errno tapi_geo_postgis_index_check(tapi_sql_conn *conn,
                                             const char *table,
                                             const char *column,
                                             bool *has_index,
                                             bool *index_used,
                                             te_string *plan);

/**
 * Evaluate a predicate in the database (ST_Intersects, ST_Contains,
 * ST_Within, ST_Touches, ST_Crosses, ST_Overlaps, ST_Disjoint,
 * ST_Equals) on two geometries of the engine, sent as EWKB.
 */
extern te_errno tapi_geo_postgis_predicate(tapi_sql_conn *conn,
                                           rcf_rpc_server *rpcs,
                                           const char *predicate,
                                           const tapi_geo_geometry *a,
                                           const tapi_geo_geometry *b,
                                           bool *result);

/** A stored geometry against an expected one. */
typedef struct tapi_geo_pg_diff {
    long long compared;
    long long missing;          /**< id not in the table */
    long long different;
    double max_hausdorff;
    te_string records;          /**< "id TAB missing|different TAB d" */
} tapi_geo_pg_diff;

/**
 * Compare the geometries of rows @p ids with @p expected (same order,
 * engine geometries) within Hausdorff distance @p tolerance (0 =
 * topological equality).
 */
extern te_errno tapi_geo_postgis_compare(tapi_sql_conn *conn,
                                         rcf_rpc_server *rpcs,
                                         const char *table,
                                         const char *id_column,
                                         const char *column,
                                         const long long *ids,
                                         tapi_geo_geometry *const *expected,
                                         unsigned int n, double tolerance,
                                         tapi_geo_pg_diff *diff);

extern void tapi_geo_pg_diff_free(tapi_geo_pg_diff *diff);

/** Fetch one stored geometry into the engine. */
extern te_errno tapi_geo_postgis_fetch(tapi_sql_conn *conn,
                                       rcf_rpc_server *rpcs,
                                       const char *table,
                                       const char *id_column,
                                       const char *column, long long id,
                                       tapi_geo_geometry **geom);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TAPI_GEO_POSTGIS_H__ */
