"""Replication: writes to a primary SS propagate to its <name>_backup."""

import time


def test_write_is_replicated_to_backup(cluster):
    cluster.add_ss("ss1_backup")  # paired with ss1 regardless of join order
    cluster.write("rep.txt", "replicated content.", username="alice")

    # Replication is asynchronous; poll the backup's on-disk copy.
    deadline = time.time() + 10
    content = None
    while time.time() < deadline:
        content = cluster.storage_file("ss1_backup", "rep.txt")
        if content and "replicated content." in content:
            break
        time.sleep(0.2)
    assert content and "replicated content." in content
