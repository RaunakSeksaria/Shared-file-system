"""Checkpoints (bonus): CHECKPOINT / REVERT / LISTCHECKPOINTS."""


def test_checkpoint_then_revert_restores_content(cluster):
    cluster.write("cp.txt", "version one.")
    cluster.client(["CHECKPOINT cp.txt v1"])
    cluster.client(["WRITE cp.txt 0", "0 two", "ETIRW"])
    out = cluster.client(["REVERT cp.txt v1", "READ cp.txt"])
    assert "version one." in out


def test_list_checkpoints_shows_tag(cluster):
    cluster.write("cl.txt", "hello.")
    cluster.client(["CHECKPOINT cl.txt tagA"])
    out = cluster.client(["LISTCHECKPOINTS cl.txt"])
    assert "tagA" in out
