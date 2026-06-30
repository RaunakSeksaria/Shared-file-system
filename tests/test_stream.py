"""STREAM: word-by-word delivery with a ~0.1s inter-word delay."""

import time


def test_stream_delivers_all_words(cluster):
    cluster.write("st.txt", "one two three.")
    out = cluster.client(["STREAM st.txt"])
    for word in ("one", "two", "three."):
        assert word in out


def test_stream_is_paced(cluster):
    cluster.write("st2.txt", "a b c d e f.")
    start = time.time()
    cluster.client(["STREAM st2.txt"])
    elapsed = time.time() - start
    # ~7 tokens * 0.1s of streaming delay; generous lower bound for CI noise.
    assert elapsed >= 0.4
