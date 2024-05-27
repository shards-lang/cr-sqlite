
extern crate alloc;

use core::alloc::Layout;
use core::ffi::c_char;
use core::panic::PanicInfo;
use crsql_core;
use crsql_core::sqlite3_crsqlcore_init;
#[cfg(feature = "test")]
pub use crsql_core::test_exports;
use crsql_fractindex_core::sqlite3_crsqlfractionalindex_init;
use sqlite_nostd as sqlite;
use sqlite_nostd::SQLite3Allocator;

// This must be our allocator so we can transfer ownership of memory to SQLite and have SQLite free that memory for us.
// This drastically reduces copies when passing strings and blobs back and forth between Rust and C.
// TODO(guusw)
// #[global_allocator]
// static ALLOCATOR: SQLite3Allocator = SQLite3Allocator {};

#[no_mangle]
pub extern "C" fn sqlite3_crsqlrustbundle_init(
    db: *mut sqlite::sqlite3,
    err_msg: *mut *mut c_char,
    api: *mut sqlite::api_routines,
) -> *mut ::core::ffi::c_void {
    sqlite::EXTENSION_INIT2(api);

    let rc = sqlite3_crsqlfractionalindex_init(db, err_msg, api);
    if rc != 0 {
        return core::ptr::null_mut();
    }

    sqlite3_crsqlcore_init(db, err_msg, api)
}
