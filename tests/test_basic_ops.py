"""Core single-client file operations: CREATE, VIEW, INFO, DELETE, LIST, READ."""


def test_create_then_view(cluster):
    out = cluster.client(["CREATE a.txt", "VIEW -a"])
    assert "File Created Successfully!" in out
    assert "a.txt" in out


def test_duplicate_create_is_rejected(cluster):
    out = cluster.client(["CREATE dup.txt", "CREATE dup.txt"])
    assert out.count("File Created Successfully!") == 1
    assert "ERROR" in out


def test_delete_removes_file(cluster):
    out = cluster.client(["CREATE d.txt", "DELETE d.txt", "VIEW -a"])
    assert "File deleted successfully!" in out
    assert "No files found." in out


def test_delete_missing_file_errors(cluster):
    out = cluster.client(["DELETE ghost.txt"])
    assert "ERROR" in out


def test_info_reports_owner(cluster):
    out = cluster.client(["CREATE info.txt", "INFO info.txt"])
    assert "info.txt" in out
    assert "Owner: alice" in out


def test_list_includes_registered_user(cluster):
    out = cluster.client(["LIST"], username="bob")
    assert "bob" in out


def test_read_empty_file_has_no_error(cluster):
    out = cluster.client(["CREATE e.txt", "READ e.txt"])
    assert "File Created Successfully!" in out
    assert "ERROR" not in out


def test_view_empty_system(cluster):
    out = cluster.client(["VIEW -a"])
    assert "No files found." in out
