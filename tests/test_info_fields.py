"""INFO: metadata fields after content is written."""


def test_info_reports_owner_and_counts(cluster):
    cluster.write("i.txt", "one two three.")
    out = cluster.client(["INFO i.txt"])
    assert "Owner: alice" in out
    assert "Words:" in out
    assert "Size:" in out
