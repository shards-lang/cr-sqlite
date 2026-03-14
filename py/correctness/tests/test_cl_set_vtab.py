from crsql_correctness import connect, close, get_site_id


def sync_left_to_right(l, r, since):
    changes = l.execute(
        "SELECT * FROM crsql_changes WHERE db_version > ?", (since,))
    for change in changes:
        r.execute(
            "INSERT INTO crsql_changes VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", change)
    r.commit()


def make_cl_set_db():
    c = connect(":memory:")
    c.execute(
        "CREATE VIRTUAL TABLE todo_schema USING CLSet "
        "(id PRIMARY KEY NOT NULL, title TEXT, done INTEGER)"
    )
    c.commit()
    return c


def test_create_cl_set():
    """Create a CL-set table, insert rows, verify clock entries exist."""
    c = make_cl_set_db()

    # The base table 'todo' should exist and be usable
    c.execute("INSERT INTO todo VALUES (1, 'Buy milk', 0)")
    c.execute("INSERT INTO todo VALUES (2, 'Walk dog', 1)")
    c.commit()

    rows = c.execute("SELECT * FROM todo ORDER BY id").fetchall()
    assert len(rows) == 2
    assert rows[0] == (1, 'Buy milk', 0)
    assert rows[1] == (2, 'Walk dog', 1)

    # Clock table should have entries
    clock_count = c.execute(
        "SELECT count(*) FROM todo__crsql_clock"
    ).fetchone()[0]
    assert clock_count > 0

    # Should appear in crsql_changes
    changes = c.execute(
        "SELECT * FROM crsql_changes WHERE [table] = 'todo'"
    ).fetchall()
    assert len(changes) > 0

    close(c)


def test_cl_set_sync():
    """Two DBs with CL-set tables, sync between them, verify convergence."""
    a = make_cl_set_db()
    b = make_cl_set_db()

    a.execute("INSERT INTO todo VALUES (1, 'Task A', 0)")
    a.commit()

    b.execute("INSERT INTO todo VALUES (2, 'Task B', 1)")
    b.commit()

    # Sync both directions
    sync_left_to_right(a, b, 0)
    sync_left_to_right(b, a, 0)

    # Both should have both rows
    a_rows = a.execute("SELECT * FROM todo ORDER BY id").fetchall()
    b_rows = b.execute("SELECT * FROM todo ORDER BY id").fetchall()
    assert a_rows == b_rows
    assert len(a_rows) == 2

    close(a)
    close(b)


def test_cl_set_delete():
    """Delete from CL-set, verify CL semantics match regular CRR."""
    a = make_cl_set_db()
    b = make_cl_set_db()

    a.execute("INSERT INTO todo VALUES (1, 'Task', 0)")
    a.commit()

    sync_left_to_right(a, b, 0)
    assert b.execute("SELECT count(*) FROM todo").fetchone()[0] == 1

    # Delete on A
    a.execute("DELETE FROM todo WHERE id = 1")
    a.commit()

    # Sync delete to B
    sync_left_to_right(a, b, 0)

    # Row should be gone on B
    assert b.execute("SELECT count(*) FROM todo").fetchone()[0] == 0

    close(a)
    close(b)
