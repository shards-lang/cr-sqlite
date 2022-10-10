#include "cfsqlite-triggers.h"
#include "cfsqlite-tableinfo.h"
#include "cfsqlite-util.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

char *cfsql_conflictSetsStr(cfsql_ColumnInfo *cols, int len)
{
  // set statements...
  char *sets[len];
  int resultLen = 0;
  char *ret = 0;

  if (len == 0)
  {
    return ret;
  }

  for (int i = 0; i < len; ++i)
  {
    if (cols[i].versionOf != 0)
    {
      sets[i] = sqlite3_mprintf(
          "\"%s\" = CASE WHEN EXCLUDED.\"%s\" THEN \"%s\" + 1 ELSE \"%s\" END",
          cols[i].name,
          cols[i].versionOf,
          cols[i].name,
          cols[i].name);
    }
    else
    {
      sets[i] = sqlite3_mprintf("\"%s\" = EXCLUDED.\"%s\"", cols[i].name, cols[i].name);
    }

    resultLen += strlen(sets[i]);
  }
  resultLen += len - 1;
  ret = sqlite3_malloc(resultLen * sizeof(char) + 1);
  ret[resultLen] = '\0';

  cfsql_joinWith(ret, sets, len, ',');

  for (int i = 0; i < len; ++i)
  {
    sqlite3_free(sets[i]);
  }

  return ret;
}

char *cfsql_localInsertOnConflictStr(cfsql_TableInfo *tableInfo)
{
  if (tableInfo->pksLen == 0)
  {
    // dup given the caller would try to deallocate it and we
    // cannot deallocate a literal
    return strdup("");
  }

  char *pkList = cfsql_asIdentifierList(tableInfo->pks, tableInfo->pksLen, 0);
  char *conflictSets = cfsql_conflictSetsStr(tableInfo->nonPks, tableInfo->nonPksLen);

  char *ret = sqlite3_mprintf(
      "ON CONFLICT (%s) DO UPDATE SET\
      %s%s\
    \"__cfsql_cl\" = CASE WHEN \"__cfsql_cl\" %% 2 = 0 THEN \"__cfsql_cl\" + 1 ELSE \"__cfsql_cl\" END,\
    \"__cfsql_src\" = 0",
      pkList,
      conflictSets,
      tableInfo->nonPksLen == 0 ? "" : ",");

  sqlite3_free(pkList);
  sqlite3_free(conflictSets);

  return ret;
}

char *cfsql_updateClocksStr(cfsql_TableInfo *tableInfo)
{
  // TODO: if there are now pks we need to use a `row_id` col

  char *pkList = 0;
  char *pkNew = 0;

  if (tableInfo->pksLen == 0)
  {
    pkNew = "NEW.\"row_id\"";
    pkList = "\"row_id\"";
  }
  else
  {
    pkNew = cfsql_asIdentifierList(tableInfo->pks, tableInfo->pksLen, "NEW.");
    pkList = cfsql_asIdentifierList(tableInfo->pks, tableInfo->pksLen, 0);
  }

  char *ret = sqlite3_mprintf(
      "INSERT INTO \"%s__cfsql_clock\" (\"__cfsql_site_id\", \"__cfsql_version\", %s)\
      VALUES (\
        cfsql_siteid(),\
        cfsql_dbversion(),\
        %s\
      )\
      ON CONFLICT (\"__cfsql_site_id\", %s) DO UPDATE SET\
        \"__cfsql_version\" = EXCLUDED.\"__cfsql_version\";\
    ",
      tableInfo->tblName,
      pkList,
      pkNew,
      pkList);

  if (tableInfo->pksLen != 0)
  {
    sqlite3_free(pkNew);
    sqlite3_free(pkList);
  }

  return ret;
}

int cfsql_createInsertTrigger(
    sqlite3 *db,
    cfsql_TableInfo *tableInfo,
    char **err)
{
  char *zSql;
  char *baseColumnsList = 0;
  char *baseColumnsNewList = 0;
  char *conflictResolution = 0;
  char *updateClocks = 0;
  int rc = SQLITE_OK;

  baseColumnsList = cfsql_asIdentifierList(tableInfo->baseCols, tableInfo->baseColsLen, 0);
  baseColumnsNewList = cfsql_asIdentifierList(tableInfo->baseCols, tableInfo->baseColsLen, "NEW.");
  conflictResolution = cfsql_localInsertOnConflictStr(tableInfo);
  updateClocks = cfsql_updateClocksStr(tableInfo);

  zSql = sqlite3_mprintf(
      "CREATE TRIGGER \"%s__cfsql_itrig\"\
      INSTEAD OF INSERT ON \"%s\"\
    BEGIN\
      INSERT INTO \"%s__cfsql_crr\" (\
        %s\
      ) VALUES (\
        %s\
      ) %s;\
      %s\
    END;",
      tableInfo->tblName,
      tableInfo->tblName,
      tableInfo->tblName,
      baseColumnsList,
      baseColumnsNewList,
      conflictResolution,
      updateClocks);

  rc = sqlite3_exec(db, zSql, 0, 0, err);

  sqlite3_free(zSql);
  sqlite3_free(baseColumnsList);
  sqlite3_free(baseColumnsNewList);
  sqlite3_free(conflictResolution);
  sqlite3_free(updateClocks);

  return rc;
}

static char *mapPkWhere(const char *x) {
  return sqlite3_mprintf("\"%s\" = NEW.\"%s\"", x, x);
}

// TODO: we could generalize this and `conflictSetsStr` and and `updateTrigUpdateSet` and other places
// if we add a parameter which is a function that produces the strings
// to join.
char *cfsql_upTrigwhereConditions(cfsql_ColumnInfo *columnInfo, int len)
{
  char *columnNames[len];
  for (int i = 0; i < len; ++i) {
    columnNames[i] = columnInfo[i].name;
  }

  return cfsql_join2(&mapPkWhere, columnNames, len, " AND ");
}

char *cfsql_updateTrigUpdateSets(cfsql_ColumnInfo *columnInfo, int len)
{
  return 0;
}

int cfsql_createUpdateTrigger(sqlite3 *db,
                              cfsql_TableInfo *tableInfo,
                              char **err)
{
  char *zSql;
  char *sets = 0;
  char *pkWhereConditions = 0;
  char *pkList = 0;
  char *pkNewList = 0;
  int rc = SQLITE_OK;

  if (tableInfo->pksLen == 0)
  {
    pkList = "\"row_id\"";
    pkNewList = "NEW.\"row_id\"";
    pkWhereConditions = "\"row_id\" = NEW.\"row_id\"";
  }
  else
  {
    pkList = cfsql_asIdentifierList(tableInfo->pks, tableInfo->pksLen, 0);
    pkNewList = cfsql_asIdentifierList(tableInfo->pks, tableInfo->pksLen, "NEW.");
    pkWhereConditions = cfsql_upTrigwhereConditions(tableInfo->pks, tableInfo->pksLen);
  }

  sets = cfsql_updateTrigUpdateSets(tableInfo->withVersionCols, tableInfo->withVersionColsLen);
  zSql = sqlite3_mprintf(
      "CREATE TRIGGER \"%s__cfsql_utrig\"\
      INSTEAD OF UPDATE ON \"%s\"\
    BEGIN\
      UPDATE \"%s__cfsql_crr\" SET\
        %s,\
        \"__cfsql_src\" = 0\
      WHERE %s;\
    INSERT INTO \"%s__cfsql_clock\" (\"__cfsql_site_id\", \"__cfsql_version\", %s)\
      VALUES (\
        cfsql_siteid(),\
        cfsql_dbversion(),\
        %s\
      )\
      ON CONFLCIT (\"__cfsql_site_id\", %s) DO UPDATE SET\
        \"__cfsql_version\" = EXCLUDED.\"__cfsql_version\";\
    END;\
    ",
      tableInfo->tblName,
      tableInfo->tblName,
      tableInfo->tblName,
      sets,
      pkWhereConditions,
      tableInfo->tblName,
      pkList,
      pkNewList,
      pkList);
  rc = sqlite3_exec(db, zSql, 0, 0, err);

  sqlite3_free(zSql);
  sqlite3_free(sets);

  if (tableInfo->pksLen != 0)
  {
    sqlite3_free(pkWhereConditions);
    sqlite3_free(pkList);
    sqlite3_free(pkNewList);
  }

  return rc;
}

int cfsql_createDeleteTrigger(
    sqlite3 *db,
    cfsql_TableInfo *tableInfo,
    char **err)
{
  // char *zSql = sqlite3_mprintf(
  //   "CREATE TRIGGER"
  // );
  return SQLITE_OK;
}

int cfsql_createCrrViewTriggers(
    sqlite3 *db,
    cfsql_TableInfo *tableInfo,
    char **err)
{

  int rc = cfsql_createInsertTrigger(db, tableInfo, err);
  if (rc == SQLITE_OK)
  {
    rc = cfsql_createUpdateTrigger(db, tableInfo, err);
  }
  if (rc == SQLITE_OK)
  {
    rc = cfsql_createDeleteTrigger(db, tableInfo, err);
  }

  return rc;
}

int cfsql_createPatchTrigger(
    sqlite3 *db,
    cfsql_TableInfo *tableInfo,
    char **err)
{
  return 0;
}
