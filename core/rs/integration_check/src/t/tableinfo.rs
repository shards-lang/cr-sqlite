extern crate alloc;
use alloc::boxed::Box;
use alloc::ffi::CString;
use alloc::string::ToString;
use alloc::vec::Vec;
use core::{ffi::c_char, mem};
use crsql_bundle::crsql_core::{self, tableinfo::TableInfo};
use sqlite::Connection;
use sqlite_nostd as sqlite;

fn make_err_ptr() -> *mut *mut c_char {
    let mut inner_ptr: *mut c_char = core::ptr::null_mut();
    let outer_ptr: *mut *mut c_char = &mut inner_ptr;
    outer_ptr
}

fn make_site() -> *mut c_char {
    let inner_ptr: *mut c_char = CString::new("0000000000000000").unwrap().into_raw();
    inner_ptr
}

fn test_ensure_table_infos_are_up_to_date() {
    let db = crate::opendb().expect("Opened DB");
    let c = &db.db;
    let raw_db = db.db.db;
    let err = make_err_ptr();

    // manually create some clock tables w/o using the extension
    // pull table info and ensure it is what we expect
    c.exec_safe("CREATE TABLE foo (a PRIMARY KEY, b);")
        .expect("made foo");
    c.exec_safe(
        "CREATE TABLE foo__crsql_clock (
      id,
      __crsql_col_name,
      __crsql_col_version,
      __crsql_db_version,
      __crsql_site_id,
      __crsql_seq
    )",
    )
    .expect("made foo clock");

    let ext_data = unsafe { crsql_core::c::crsql_newExtData(raw_db, make_site()) };
    crsql_core::tableinfo::crsql_ensure_table_infos_are_up_to_date(raw_db, ext_data, err);

    let mut table_infos = unsafe {
        mem::ManuallyDrop::new(Box::from_raw((*ext_data).tableInfos as *mut Vec<TableInfo>))
    };

    assert_eq!(table_infos.len(), 1);
    assert_eq!(table_infos[0].tbl_name, "foo");

    // we're going to change table infos so we can check that it does not get filled again since no schema changes happened
    table_infos[0].tbl_name = "bar".to_string();

    crsql_core::tableinfo::crsql_ensure_table_infos_are_up_to_date(raw_db, ext_data, err);

    assert_eq!(table_infos.len(), 1);
    assert_eq!(table_infos[0].tbl_name, "bar");

    c.exec_safe("CREATE TABLE boo (a PRIMARY KEY, b);")
        .expect("made boo");
    c.exec_safe(
        "CREATE TABLE boo__crsql_clock (
      id,
      __crsql_col_name,
      __crsql_col_version,
      __crsql_db_version,
      __crsql_site_id,
      __crsql_seq
    )",
    )
    .expect("made boo clock");

    crsql_core::tableinfo::crsql_ensure_table_infos_are_up_to_date(raw_db, ext_data, err);

    assert_eq!(table_infos.len(), 2);
    assert_eq!(table_infos[0].tbl_name, "foo");
    assert_eq!(table_infos[1].tbl_name, "boo");

    c.exec_safe("DROP TABLE foo").expect("dropped foo");
    c.exec_safe("DROP TABLE boo").expect("dropped boo");
    c.exec_safe("DROP TABLE boo__crsql_clock")
        .expect("dropped boo");
    c.exec_safe("DROP TABLE foo__crsql_clock")
        .expect("dropped boo");

    crsql_core::tableinfo::crsql_ensure_table_infos_are_up_to_date(raw_db, ext_data, err);

    assert_eq!(table_infos.len(), 0);

    unsafe {
        crsql_core::c::crsql_freeExtData(ext_data);
    };
}

fn test_pull_table_info() {
    let db = crate::opendb().expect("Opened DB");
    let c = &db.db;
    let raw_db = db.db.db;
    let err = make_err_ptr();
    // test that we receive the expected values in column info and such.
    // pks are ordered
    // pks and non pks split
    // cids filled

    c.exec_safe("CREATE TABLE foo (a INTEGER PRIMARY KEY, b TEXT NOT NULL, c NUMBER, d FLOAT, e);")
        .expect("made foo");

    let tbl_info = crsql_core::tableinfo::pull_table_info(raw_db, "foo", err)
        .expect("pulled table info for foo");
    assert_eq!(tbl_info.pks.len(), 1);
    assert_eq!(tbl_info.pks[0].name, "a");
    assert_eq!(tbl_info.pks[0].cid, 0);
    assert_eq!(tbl_info.pks[0].pk, 1);
    assert_eq!(tbl_info.non_pks.len(), 4);
    assert_eq!(tbl_info.non_pks[0].name, "b");
    assert_eq!(tbl_info.non_pks[0].cid, 1);
    assert_eq!(tbl_info.non_pks[1].name, "c");
    assert_eq!(tbl_info.non_pks[1].cid, 2);
    assert_eq!(tbl_info.non_pks[2].name, "d");
    assert_eq!(tbl_info.non_pks[2].cid, 3);
    assert_eq!(tbl_info.non_pks[3].name, "e");
    assert_eq!(tbl_info.non_pks[3].cid, 4);

    c.exec_safe("CREATE TABLE boo (a INTEGER, b TEXT NOT NULL, c NUMBER, d FLOAT, e, PRIMARY KEY(b, c, d, e));")
        .expect("made boo");
    let tbl_info = crsql_core::tableinfo::pull_table_info(raw_db, "boo", err)
        .expect("pulled table info for boo");
    assert_eq!(tbl_info.pks.len(), 4);
    assert_eq!(tbl_info.pks[0].name, "b");
    assert_eq!(tbl_info.pks[0].cid, 1);
    assert_eq!(tbl_info.pks[0].pk, 1);
    assert_eq!(tbl_info.pks[1].name, "c");
    assert_eq!(tbl_info.pks[1].cid, 2);
    assert_eq!(tbl_info.pks[1].pk, 2);
    assert_eq!(tbl_info.pks[2].name, "d");
    assert_eq!(tbl_info.pks[2].cid, 3);
    assert_eq!(tbl_info.pks[2].pk, 3);
    assert_eq!(tbl_info.pks[3].name, "e");
    assert_eq!(tbl_info.pks[3].cid, 4);
    assert_eq!(tbl_info.pks[3].pk, 4);
    assert_eq!(tbl_info.non_pks.len(), 1);
    assert_eq!(tbl_info.non_pks[0].name, "a");
    assert_eq!(tbl_info.non_pks[0].cid, 0);
    assert_eq!(tbl_info.non_pks[0].pk, 0);
}

fn test_is_table_compatible() {
    let db = crate::opendb().expect("Opened DB");
    let c = &db.db;
    let raw_db = db.db.db;
    let err = make_err_ptr();
    // convert the commented out test below into a format that resembles the tests above
    // and then run it

    // no pks
    c.exec_safe("CREATE TABLE foo (a);").expect("made foo");
    let is_compatible = crsql_core::tableinfo::is_table_compatible(raw_db, "foo", err)
        .expect("checked if foo is compatible");
    assert_eq!(is_compatible, false);

    // pks
    c.exec_safe("CREATE TABLE bar (a PRIMARY KEY);")
        .expect("made bar");
    let is_compatible = crsql_core::tableinfo::is_table_compatible(raw_db, "bar", err)
        .expect("checked if bar is compatible");
    assert_eq!(is_compatible, true);

    // pks + other non unique indices
    c.exec_safe("CREATE TABLE baz (a PRIMARY KEY, b);")
        .expect("made baz");
    c.exec_safe("CREATE INDEX bar_i ON baz (b);")
        .expect("made index");
    let is_compatible = crsql_core::tableinfo::is_table_compatible(raw_db, "baz", err)
        .expect("checked if baz is compatible");
    assert_eq!(is_compatible, true);

    // pks + other unique indices
    c.exec_safe("CREATE TABLE booz (a PRIMARY KEY, b);")
        .expect("made booz");
    c.exec_safe("CREATE UNIQUE INDEX booz_b ON booz (b);")
        .expect("made index");
    let is_compatible = crsql_core::tableinfo::is_table_compatible(raw_db, "booz", err)
        .expect("checked if booz is compatible");
    assert_eq!(is_compatible, false);

    // not null and no dflt
    c.exec_safe("CREATE TABLE buzz (a PRIMARY KEY, b NOT NULL);")
        .expect("made buzz");
    let is_compatible = crsql_core::tableinfo::is_table_compatible(raw_db, "buzz", err)
        .expect("checked if buzz is compatible");
    assert_eq!(is_compatible, false);

    // not null and dflt
    c.exec_safe("CREATE TABLE boom (a PRIMARY KEY, b NOT NULL DEFAULT 1);")
        .expect("made boom");
    let is_compatible = crsql_core::tableinfo::is_table_compatible(raw_db, "boom", err)
        .expect("checked if boom is compatible");
    assert_eq!(is_compatible, true);

    // fk constraint
    c.exec_safe("CREATE TABLE zoom (a PRIMARY KEY, b, FOREIGN KEY(b) REFERENCES foo(a));")
        .expect("made zoom");
    let is_compatible = crsql_core::tableinfo::is_table_compatible(raw_db, "zoom", err)
        .expect("checked if zoom is compatible");
    assert_eq!(is_compatible, false);

    // strict mode should be ok
    c.exec_safe("CREATE TABLE atable (\"id\" TEXT PRIMARY KEY) STRICT;")
        .expect("made atable");
    let is_compatible = crsql_core::tableinfo::is_table_compatible(raw_db, "atable", err)
        .expect("checked if atable is compatible");
    assert_eq!(is_compatible, true);

    // no autoincrement
    c.exec_safe("CREATE TABLE woom (a integer primary key autoincrement);")
        .expect("made woom");
    let is_compatible = crsql_core::tableinfo::is_table_compatible(raw_db, "woom", err)
        .expect("checked if woom is compatible");
    assert_eq!(is_compatible, false);

    // aliased rowid
    c.exec_safe("CREATE TABLE loom (a integer primary key);")
        .expect("made loom");
    let is_compatible = crsql_core::tableinfo::is_table_compatible(raw_db, "loom", err)
        .expect("checked if loom is compatible");
    assert_eq!(is_compatible, true);

    c.exec_safe("CREATE TABLE atable2 (\"id\" TEXT PRIMARY KEY, x TEXT) STRICT;")
        .expect("made atable2");
    let is_compatible = crsql_core::tableinfo::is_table_compatible(raw_db, "atable2", err)
        .expect("checked if atable2 is compatible");
    assert_eq!(is_compatible, true);

    c.exec_safe(
        "CREATE TABLE ydoc (\
        doc_id TEXT,\
        yhash BLOB,\
        yval BLOB,\
        primary key (doc_id, yhash)\
      ) STRICT;",
    )
    .expect("made ydoc");
    let is_compatible = crsql_core::tableinfo::is_table_compatible(raw_db, "atable2", err)
        .expect("checked if atable2 is compatible");
    assert_eq!(is_compatible, true);
}

fn test_create_clock_table_from_table_info() {
    let db = crate::opendb().expect("Opened DB");
    let c = &db.db;
    let raw_db = db.db.db;
    let err = make_err_ptr();

    c.exec_safe("CREATE TABLE foo (a, b, primary key (a, b));")
        .expect("made foo");
    c.exec_safe("CREATE TABLE bar (a primary key);")
        .expect("made bar");
    c.exec_safe("CREATE TABLE baz (a primary key, b);")
        .expect("made baz");
    c.exec_safe("CREATE TABLE boo (a primary key, b, c);")
        .expect("made boo");

    let foo_tbl_info = crsql_core::tableinfo::pull_table_info(raw_db, "foo", err)
        .expect("pulled table info for foo");
    let bar_tbl_info = crsql_core::tableinfo::pull_table_info(raw_db, "bar", err)
        .expect("pulled table info for bar");
    let baz_tbl_info = crsql_core::tableinfo::pull_table_info(raw_db, "baz", err)
        .expect("pulled table info for baz");
    let boo_tbl_info = crsql_core::tableinfo::pull_table_info(raw_db, "boo", err)
        .expect("pulled table info for boo");

    crsql_core::bootstrap::create_clock_table(raw_db, &foo_tbl_info, err)
        .expect("created clock table for foo");
    crsql_core::bootstrap::create_clock_table(raw_db, &bar_tbl_info, err)
        .expect("created clock table for bar");
    crsql_core::bootstrap::create_clock_table(raw_db, &baz_tbl_info, err)
        .expect("created clock table for baz");
    crsql_core::bootstrap::create_clock_table(raw_db, &boo_tbl_info, err)
        .expect("created clock table for boo");

    // todo: Check that clock tables have expected schema(s)
}

pub fn run_suite() {
    test_ensure_table_infos_are_up_to_date();
    test_pull_table_info();
    test_is_table_compatible();
    test_create_clock_table_from_table_info();
}
