"""Replication: writes to a primary SS propagate to its <name>_backup."""

import time

import pytest


@pytest.mark.xfail(
    reason="BUG: replication pairing is order-dependent. The NM assigns a replica only at "
    "the *primary's* registration (main.c), so a backup that registers after its primary "
    "is never paired and writes are not replicated. Fix in Phase 6, then drop this xfail.",
    strict=True,
)
def test_write_is_replicated_to_backup(cluster):
    cluster.add_ss("ss1_backup")  # joins after ss1 -> currently never paired
    cluster.write("rep.txt", "replicated content.", username="alice")

    deadline = time.time() + 8
    content = None
    while time.time() < deadline:
        content = cluster.storage_file("ss1_backup", "rep.txt")
        if content and "replicated content." in content:
            break
        time.sleep(0.2)
    assert content and "replicated content." in content
