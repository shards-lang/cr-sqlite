#include "triggers.h"
#include "str_util.h"
#include <stdio.h>
#include <string.h>

// Create all three triggers (insert, update, delete) for a CRR table
int crsql_create_triggers(sqlite3 *db, crsql_TableInfo *table_info, char **err) {
  int rc = crsql_create_insert_trigger(db, table_info, err);
  if (rc != SQLITE_OK) return rc;

  rc = crsql_create_update_trigger(db, table_info, err);
  if (rc != SQLITE_OK) return rc;

  return crsql_create_delete_trigger(db, table_info, err);
}

// Create the INSERT trigger for change tracking
int crsql_create_insert_trigger(sqlite3 *db, crsql_TableInfo *table_info, char **err) {
  char *escaped_tbl_value = crsql_escape_ident_as_value(table_info->tbl_name);
  char *escaped_tbl_ident = crsql_escape_ident(table_info->tbl_name);
  char *pk_new_list = crsql_as_identifier_list(table_info->pks, table_info->pks_len, "NEW.");

  if (!escaped_tbl_value || !escaped_tbl_ident || !pk_new_list) {
    if (err) *err = sqlite3_mprintf("Out of memory creating insert trigger");
    sqlite3_free(escaped_tbl_value);
    sqlite3_free(escaped_tbl_ident);
    sqlite3_free(pk_new_list);
    return SQLITE_NOMEM;
  }

  char *sql = sqlite3_mprintf(
    "CREATE TRIGGER IF NOT EXISTS \"%s__crsql_itrig\"\n"
    "  AFTER INSERT ON \"%s\" WHEN crsql_internal_sync_bit() = 0\n"
    "  BEGIN\n"
    "    VALUES (crsql_after_insert('%s', %s));\n"
    "  END;",
    escaped_tbl_ident,
    escaped_tbl_ident,
    escaped_tbl_value,
    pk_new_list
  );

  sqlite3_free(escaped_tbl_value);
  sqlite3_free(escaped_tbl_ident);
  sqlite3_free(pk_new_list);

  if (!sql) {
    if (err) *err = sqlite3_mprintf("Out of memory creating insert trigger SQL");
    return SQLITE_NOMEM;
  }

  int rc = sqlite3_exec(db, sql, NULL, NULL, err);
  sqlite3_free(sql);
  return rc;
}

// Create the UPDATE trigger for change tracking
int crsql_create_update_trigger(sqlite3 *db, crsql_TableInfo *table_info, char **err) {
  char *escaped_tbl_value = crsql_escape_ident_as_value(table_info->tbl_name);
  char *escaped_tbl_ident = crsql_escape_ident(table_info->tbl_name);
  char *pk_new_list = crsql_as_identifier_list(table_info->pks, table_info->pks_len, "NEW.");
  char *pk_old_list = crsql_as_identifier_list(table_info->pks, table_info->pks_len, "OLD.");

  if (!escaped_tbl_value || !escaped_tbl_ident || !pk_new_list || !pk_old_list) {
    if (err) *err = sqlite3_mprintf("Out of memory creating update trigger");
    sqlite3_free(escaped_tbl_value);
    sqlite3_free(escaped_tbl_ident);
    sqlite3_free(pk_new_list);
    sqlite3_free(pk_old_list);
    return SQLITE_NOMEM;
  }

  char *sql;

  // If there are no non-pk columns, we only pass the pk lists
  if (table_info->non_pks_len == 0) {
    sql = sqlite3_mprintf(
      "CREATE TRIGGER IF NOT EXISTS \"%s__crsql_utrig\"\n"
      "  AFTER UPDATE ON \"%s\" WHEN crsql_internal_sync_bit() = 0\n"
      "  BEGIN\n"
      "    VALUES (crsql_after_update('%s', %s, %s));\n"
      "  END;",
      escaped_tbl_ident,
      escaped_tbl_ident,
      escaped_tbl_value,
      pk_new_list,
      pk_old_list
    );
  } else {
    // Include non-pk column lists
    char *non_pk_new_list = crsql_as_identifier_list(table_info->non_pks, table_info->non_pks_len, "NEW.");
    char *non_pk_old_list = crsql_as_identifier_list(table_info->non_pks, table_info->non_pks_len, "OLD.");

    if (!non_pk_new_list || !non_pk_old_list) {
      if (err) *err = sqlite3_mprintf("Out of memory creating update trigger");
      sqlite3_free(escaped_tbl_value);
      sqlite3_free(escaped_tbl_ident);
      sqlite3_free(pk_new_list);
      sqlite3_free(pk_old_list);
      sqlite3_free(non_pk_new_list);
      sqlite3_free(non_pk_old_list);
      return SQLITE_NOMEM;
    }

    sql = sqlite3_mprintf(
      "CREATE TRIGGER IF NOT EXISTS \"%s__crsql_utrig\"\n"
      "  AFTER UPDATE ON \"%s\" WHEN crsql_internal_sync_bit() = 0\n"
      "  BEGIN\n"
      "    VALUES (crsql_after_update('%s', %s, %s, %s, %s));\n"
      "  END;",
      escaped_tbl_ident,
      escaped_tbl_ident,
      escaped_tbl_value,
      pk_new_list,
      pk_old_list,
      non_pk_new_list,
      non_pk_old_list
    );

    sqlite3_free(non_pk_new_list);
    sqlite3_free(non_pk_old_list);
  }

  sqlite3_free(escaped_tbl_value);
  sqlite3_free(escaped_tbl_ident);
  sqlite3_free(pk_new_list);
  sqlite3_free(pk_old_list);

  if (!sql) {
    if (err) *err = sqlite3_mprintf("Out of memory creating update trigger SQL");
    return SQLITE_NOMEM;
  }

  int rc = sqlite3_exec(db, sql, NULL, NULL, err);
  sqlite3_free(sql);
  return rc;
}

// Create the DELETE trigger for change tracking
int crsql_create_delete_trigger(sqlite3 *db, crsql_TableInfo *table_info, char **err) {
  char *escaped_tbl_value = crsql_escape_ident_as_value(table_info->tbl_name);
  char *escaped_tbl_ident = crsql_escape_ident(table_info->tbl_name);
  char *pk_old_list = crsql_as_identifier_list(table_info->pks, table_info->pks_len, "OLD.");

  if (!escaped_tbl_value || !escaped_tbl_ident || !pk_old_list) {
    if (err) *err = sqlite3_mprintf("Out of memory creating delete trigger");
    sqlite3_free(escaped_tbl_value);
    sqlite3_free(escaped_tbl_ident);
    sqlite3_free(pk_old_list);
    return SQLITE_NOMEM;
  }

  char *sql = sqlite3_mprintf(
    "CREATE TRIGGER IF NOT EXISTS \"%s__crsql_dtrig\"\n"
    "  AFTER DELETE ON \"%s\" WHEN crsql_internal_sync_bit() = 0\n"
    "  BEGIN\n"
    "    VALUES (crsql_after_delete('%s', %s));\n"
    "  END;",
    escaped_tbl_ident,
    escaped_tbl_ident,
    escaped_tbl_value,
    pk_old_list
  );

  sqlite3_free(escaped_tbl_value);
  sqlite3_free(escaped_tbl_ident);
  sqlite3_free(pk_old_list);

  if (!sql) {
    if (err) *err = sqlite3_mprintf("Out of memory creating delete trigger SQL");
    return SQLITE_NOMEM;
  }

  int rc = sqlite3_exec(db, sql, NULL, NULL, err);
  sqlite3_free(sql);
  return rc;
}
