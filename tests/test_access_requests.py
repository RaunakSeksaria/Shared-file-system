"""Access-request workflow (bonus): RACC / VIEWACCR / APPROVEACCR / DISACCR."""


def test_request_then_approve_grants_access(cluster):
    cluster.write("ar.txt", "secret content.", username="alice")

    req = cluster.client(["RACC ar.txt R"], username="bob")
    assert "request created" in req.lower()

    view = cluster.client(["VIEWACCR"], username="alice")
    assert "bob" in view and "ar.txt" in view

    cluster.client(["APPROVEACCR 1"], username="alice")
    out = cluster.client(["READ ar.txt"], username="bob")
    assert "secret content." in out


def test_request_then_disapprove_denies_access(cluster):
    cluster.write("ar2.txt", "data here.", username="alice")
    cluster.client(["RACC ar2.txt R"], username="bob")
    cluster.client(["DISACCR 1"], username="alice")
    out = cluster.client(["READ ar2.txt"], username="bob")
    assert "ERROR" in out
