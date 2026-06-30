"""Persistence: file content and metadata survive a storage-server restart."""


def test_file_survives_ss_restart(cluster):
    cluster.write("persist.txt", "durable content.", username="alice")
    cluster.restart_ss("ss1")
    out = cluster.client(["READ persist.txt"])
    assert "durable content." in out
