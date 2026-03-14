from crsql_correctness import connect, close, get_site_id


def sync_left_to_right(l, r, since):
    changes = l.execute(
        "SELECT * FROM crsql_changes WHERE db_version > ?", (since,))
    for change in changes:
        r.execute(
            "INSERT INTO crsql_changes VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", change)
    r.commit()


def make_db():
    c = connect(":memory:")
    c.execute("CREATE TABLE foo (a primary key not null, b, c)")
    c.execute("SELECT crsql_as_crr('foo')")
    c.commit()
    return c


def test_update_single_column():
    """Update a column and verify the changes vtab shows the update with correct col_version."""
    c = make_db()
    c.execute("INSERT INTO foo VALUES (1, 'hello', 'world')")
    c.commit()

    # Update column b
    c.execute("UPDATE foo SET b = 'goodbye' WHERE a = 1")
    c.commit()

    # Check that changes include the update for column b
    rows = c.execute(
        "SELECT cid, val, col_version FROM crsql_changes WHERE [table] = 'foo'"
    ).fetchall()

    # Find the row for column 'b'
    b_rows = [r for r in rows if r[0] == 'b']
    assert len(b_rows) == 1
    assert b_rows[0][1] == 'goodbye'
    # col_version should be 2 (1 from insert, 2 from update)
    assert b_rows[0][2] == 2

    close(c)


def test_merge_noop():
    """Merging a change with the same value and equal version should be a no-op."""
    a = make_db()
    b = make_db()

    # Both peers insert the same row with the same data
    a.execute("INSERT INTO foo VALUES (1, 'hello', 'world')")
    a.commit()
    b.execute("INSERT INTO foo VALUES (1, 'hello', 'world')")
    b.commit()

    va = a.execute("SELECT crsql_db_version()").fetchone()[0]
    vb = b.execute("SELECT crsql_db_version()").fetchone()[0]
    assert va == vb  # both did the same number of operations

    # Sync A -> B: should be a no-op since B already has the same data
    sync_left_to_right(a, b, 0)

    vb_post = b.execute("SELECT crsql_db_version()").fetchone()[0]
    # db_version should not advance for a no-op merge
    assert vb == vb_post

    close(a)
    close(b)


def test_update_pk_column():
    """Update a PK value — old row should be tombstoned, new row created."""
    c = make_db()
    c.execute("INSERT INTO foo VALUES (1, 'hello', 'world')")
    c.commit()

    c.execute("UPDATE foo SET a = 2 WHERE a = 1")
    c.commit()

    # Old row should not exist
    assert c.execute("SELECT count(*) FROM foo WHERE a = 1").fetchone()[0] == 0
    # New row should exist with the same data
    row = c.execute("SELECT b, c FROM foo WHERE a = 2").fetchone()
    assert row == ('hello', 'world')

    # Changes should reflect both the delete (old PK) and create (new PK)
    changes = c.execute("SELECT * FROM crsql_changes").fetchall()
    assert len(changes) > 0

    close(c)


def test_update_after_sync():
    """Update on peer A, sync to B, update same col on B, sync back — LWW semantics."""
    a = make_db()
    b = make_db()

    a.execute("INSERT INTO foo VALUES (1, 'v1', 'x')")
    a.commit()

    sync_left_to_right(a, b, 0)

    # Both should have same data
    assert a.execute("SELECT b FROM foo WHERE a = 1").fetchone()[0] == 'v1'
    assert b.execute("SELECT b FROM foo WHERE a = 1").fetchone()[0] == 'v1'

    va = a.execute("SELECT crsql_db_version()").fetchone()[0]

    # Update on A
    a.execute("UPDATE foo SET b = 'v2' WHERE a = 1")
    a.commit()

    # Update on B (later, so higher col_version after sync)
    b.execute("UPDATE foo SET b = 'v3' WHERE a = 1")
    b.commit()

    # Sync A -> B: B should keep 'v3' because B's col_version for b is higher
    sync_left_to_right(a, b, 0)
    assert b.execute("SELECT b FROM foo WHERE a = 1").fetchone()[0] == 'v3'

    # Sync B -> A: A should get 'v3' because B's col_version is higher
    sync_left_to_right(b, a, 0)
    assert a.execute("SELECT b FROM foo WHERE a = 1").fetchone()[0] == 'v3'

    close(a)
    close(b)
