"""EXEC: the file's contents are run as a (sandboxed) shell script on the Name Server."""

import time


def test_exec_runs_file_contents(cluster):
    cluster.write("script.txt", "echo exec_marker.")
    out = cluster.client(["EXEC script.txt"])
    assert "exec_marker" in out


def test_exec_sandbox_kills_runaway_script(cluster):
    # Write a script that would sleep far longer than the sandbox's wall-clock limit.
    # (Written straight to disk so the shell text isn't mangled by the sentence parser.)
    cluster.client(["CREATE loop.txt"], username="alice")
    (cluster.ss["ss1"]["storage"] / "files" / "loop.txt").write_text("sleep 60\n")

    start = time.time()
    out = cluster.client(["EXEC loop.txt"], username="alice", timeout=25)
    elapsed = time.time() - start

    assert elapsed < 20, "sandbox did not enforce the wall-clock timeout"
    assert "terminated" in out.lower() or "timeout" in out.lower()
