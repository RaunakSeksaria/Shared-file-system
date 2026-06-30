"""UNDO: reverts the most recent change; file-scoped, not user-scoped."""


def test_undo_reverts_last_write(cluster):
    cluster.write("u.txt", "original text.")
    cluster.client(["WRITE u.txt 0", "0 changed", "ETIRW"])
    out = cluster.client(["UNDO u.txt", "READ u.txt"])
    assert "Undo Successful!" in out
    assert "original text." in out


def test_undo_is_cross_user(cluster):
    # alice writes; bob (with write access) writes; alice's UNDO reverts bob's change.
    cluster.write("c.txt", "alice text.", username="alice")
    cluster.client(["ADDACCESS -W c.txt bob"], username="alice")
    cluster.client(["WRITE c.txt 0", "0 bob", "ETIRW"], username="bob")
    out = cluster.client(["UNDO c.txt", "READ c.txt"], username="alice")
    assert "Undo Successful!" in out
    assert "alice text." in out
