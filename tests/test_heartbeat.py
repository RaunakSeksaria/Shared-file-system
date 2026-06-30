"""Heartbeat: the Name Server detects a storage server that stops responding."""


def test_storage_server_failure_is_detected(cluster):
    cluster.add_ss("ss2")
    cluster.kill("ss2")
    # Detection takes ~30s: a 15s overdue threshold plus 3 missed checks at 5s each.
    assert cluster.wait_log("nm", "heartbeat_monitor_failure", timeout=45)
