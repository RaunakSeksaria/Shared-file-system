"""STREAM: if the storage server dies mid-stream, the client reports an error."""

import time

import pexpect


def test_stream_reports_error_when_ss_dies(cluster):
    cluster.write(
        "stream.txt",
        "one two three four five six seven eight nine ten more words here now.",
        username="alice",
    )
    child = cluster.spawn("alice")
    child.sendline("STREAM stream.txt")
    time.sleep(0.3)          # let streaming start
    cluster.kill("ss1")      # storage server dies mid-stream

    # The client should surface an error once the SS connection drops.
    idx = child.expect(["[Ee]rror", pexpect.EOF, pexpect.TIMEOUT], timeout=10)
    child.close(force=True)
    assert idx == 0, "client did not report an error when the SS died mid-stream"
