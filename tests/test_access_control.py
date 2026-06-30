"""Access control: ADDACCESS / REMACCESS and enforcement on READ."""


def _make_secret(cluster, f="s.txt", content="top secret.", owner="alice"):
    cluster.write(f, content, username=owner)


def test_unauthorized_read_blocked(cluster):
    _make_secret(cluster)
    out = cluster.client(["READ s.txt"], username="bob")
    assert "ERROR" in out
    assert "top secret." not in out


def test_grant_read_access_allows_read(cluster):
    _make_secret(cluster)
    cluster.client(["ADDACCESS -R s.txt bob"], username="alice")
    out = cluster.client(["READ s.txt"], username="bob")
    assert "top secret." in out


def test_remove_access_blocks_again(cluster):
    _make_secret(cluster)
    cluster.client(["ADDACCESS -R s.txt bob"], username="alice")
    cluster.client(["REMACCESS s.txt bob"], username="alice")
    out = cluster.client(["READ s.txt"], username="bob")
    assert "ERROR" in out


def test_non_owner_cannot_grant_access(cluster):
    _make_secret(cluster)
    out = cluster.client(["ADDACCESS -R s.txt mallory"], username="mallory")
    assert "ERROR" in out
