"""EXEC: the file's contents are run as a shell script on the Name Server."""


def test_exec_runs_file_contents(cluster):
    cluster.write("script.txt", "echo exec_marker.")
    out = cluster.client(["EXEC script.txt"])
    assert "exec_marker" in out
