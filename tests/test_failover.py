"""Failover: after the primary SS dies, reads are served from the replica."""


def test_read_survives_primary_failure(cluster):
    cluster.add_ss("ss1_backup")
    cluster.write("fo.txt", "failover content.", username="alice")

    cluster.kill("ss1")  # primary dies
    assert cluster.wait_log("nm", "failover_complete", timeout=40)

    out = cluster.client(["READ fo.txt"])
    assert "failover content." in out
