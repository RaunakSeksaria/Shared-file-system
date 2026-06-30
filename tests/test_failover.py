"""Failover: after the primary SS dies, reads are served from the replica."""

import pytest


@pytest.mark.xfail(
    reason="BUG: failover has nothing to promote because replication pairing is "
    "order-dependent (same root cause as test_replication) — a backup joining after its "
    "primary is never paired, so no replica exists. Fix in Phase 6, then drop this xfail.",
    strict=True,
)
def test_read_survives_primary_failure(cluster):
    cluster.add_ss("ss1_backup")
    cluster.write("fo.txt", "failover content.", username="alice")

    cluster.kill("ss1")  # primary dies
    assert cluster.wait_log("nm", "failover_complete", timeout=40)

    out = cluster.client(["READ fo.txt"])
    assert "failover content." in out
