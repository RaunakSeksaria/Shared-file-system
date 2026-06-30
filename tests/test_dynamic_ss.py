"""A storage server can register with the Name Server after startup."""


def test_storage_server_can_join_after_startup(cluster):
    cluster.add_ss("ss2")
    assert "ss2" in cluster.log("nm")
    out = cluster.client(["CREATE joined.txt", "VIEW -a"])
    assert "joined.txt" in out
