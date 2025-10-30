#include "changes_vtab_read.h"
#include "str_util.h"
#include "consts.h"
#include <string.h>

// Build change query for a single table
static char *crsql_changes_query_for_table(crsql_TableInfo *table_info) {
  if (table_info->pks_len == 0) {
    // No primary keys - can't track changes
    return NULL;
  }

  char *pk_list = crsql_as_identifier_list(table_info->pks, table_info->pks_len,
                                           "pk_tbl.");
  if (!pk_list) return NULL;

  char *table_name_val = crsql_escape_ident_as_value(table_info->tbl_name);
  char *table_name_ident = crsql_escape_ident(table_info->tbl_name);

  if (!table_name_val || !table_name_ident) {
    sqlite3_free(pk_list);
    if (table_name_val) sqlite3_free(table_name_val);
    if (table_name_ident) sqlite3_free(table_name_ident);
    return NULL;
  }

  // Build the query
  // LEFT JOIN for causal length (sentinel) since we don't store it until needed
  char *query = sqlite3_mprintf(
    "SELECT "
    "  '%s' as tbl, "
    "  crsql_pack_columns(%s) as pks, "
    "  t1.col_name as cid, "
    "  t1.col_version as col_vrsn, "
    "  t1.db_version as db_vrsn, "
    "  site_tbl.site_id as site_id, "
    "  t1.key, "
    "  t1.seq as seq, "
    "  COALESCE(t2.col_version, 1) as cl "
    "FROM \"%s__crsql_clock\" AS t1 "
    "JOIN \"%s__crsql_pks\" AS pk_tbl ON t1.key = pk_tbl.__crsql_key "
    "LEFT JOIN crsql_site_id AS site_tbl ON t1.site_id = site_tbl.ordinal "
    "LEFT JOIN \"%s__crsql_clock\" AS t2 ON "
    "  t1.key = t2.key AND t2.col_name = '%s'",
    table_name_val,
    pk_list,
    table_name_ident,
    table_name_ident,
    table_name_ident,
    CL_SENTINEL
  );

  sqlite3_free(pk_list);
  sqlite3_free(table_name_val);
  sqlite3_free(table_name_ident);

  return query;
}

// Build union query of all table changes
char *crsql_changes_union_query(crsql_TableInfo **table_infos, int num_tables,
                                const char *idx_str) {
  if (num_tables == 0) {
    return sqlite3_mprintf("SELECT NULL WHERE 0 %s", idx_str ? idx_str : "");
  }

  // Build individual queries
  char **sub_queries = sqlite3_malloc(sizeof(char*) * num_tables);
  if (!sub_queries) return NULL;

  int valid_queries = 0;
  for (int i = 0; i < num_tables; i++) {
    sub_queries[i] = crsql_changes_query_for_table(table_infos[i]);
    if (sub_queries[i]) {
      valid_queries++;
    }
  }

  if (valid_queries == 0) {
    sqlite3_free(sub_queries);
    return sqlite3_mprintf("SELECT NULL WHERE 0 %s", idx_str ? idx_str : "");
  }

  // Join with UNION ALL
  char *unions = sqlite3_malloc(1);
  if (!unions) {
    for (int i = 0; i < num_tables; i++) {
      if (sub_queries[i]) sqlite3_free(sub_queries[i]);
    }
    sqlite3_free(sub_queries);
    return NULL;
  }
  unions[0] = '\0';

  int first = 1;
  for (int i = 0; i < num_tables; i++) {
    if (!sub_queries[i]) continue;

    char *new_unions;
    if (first) {
      new_unions = sqlite3_mprintf("%s", sub_queries[i]);
      first = 0;
    } else {
      new_unions = sqlite3_mprintf("%s UNION ALL %s", unions, sub_queries[i]);
    }

    sqlite3_free(unions);
    unions = new_unions;

    if (!unions) {
      for (int j = i; j < num_tables; j++) {
        if (sub_queries[j]) sqlite3_free(sub_queries[j]);
      }
      sqlite3_free(sub_queries);
      return NULL;
    }
  }

  // Free sub queries
  for (int i = 0; i < num_tables; i++) {
    if (sub_queries[i]) sqlite3_free(sub_queries[i]);
  }
  sqlite3_free(sub_queries);

  // Build final query
  char *final_query = sqlite3_mprintf(
    "SELECT tbl, pks, cid, col_vrsn, db_vrsn, site_id, key, seq, cl "
    "FROM (%s) %s",
    unions,
    idx_str ? idx_str : ""
  );

  sqlite3_free(unions);
  return final_query;
}
