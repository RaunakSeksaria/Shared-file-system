"""READ: content round-trips, including multiple sentences."""


def test_read_preserves_multiple_sentences(cluster):
    cluster.write("r.txt", "Alpha beta. Gamma delta.")
    out = cluster.client(["READ r.txt"])
    assert "Alpha beta." in out
    assert "Gamma delta." in out


def test_read_nonexistent_errors(cluster):
    out = cluster.client(["READ nope.txt"])
    assert "ERROR" in out
