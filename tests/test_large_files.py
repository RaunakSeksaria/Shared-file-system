"""Large files: READ returns content beyond the old fixed 64 KB buffer."""


def test_read_returns_file_larger_than_64kb(cluster):
    cluster.client(["CREATE big.txt"], username="alice")  # creates file + metadata
    blob = "A" * 100_000  # 100 KB, well past the old 65535-byte cap
    (cluster.ss["ss1"]["storage"] / "files" / "big.txt").write_text(blob)

    out = cluster.client(["READ big.txt"], username="alice")
    assert blob in out  # would have been truncated to ~64 KB before the fix
