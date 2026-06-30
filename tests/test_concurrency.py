"""Concurrency: per-sentence write locking and concurrent clients."""

import threading

import pexpect


def test_sentence_lock_blocks_second_writer(cluster):
    cluster.write("lock.txt", "shared sentence.", username="alice")
    cluster.client(["ADDACCESS -W lock.txt bob"], username="alice")

    alice = cluster.spawn("alice")
    alice.sendline("WRITE lock.txt 0")
    alice.expect("WRITE> ")  # alice now holds the sentence-0 lock

    # bob has write access but the sentence is locked -> he must be refused.
    out = cluster.client(["WRITE lock.txt 0", "ETIRW"], username="bob")
    assert "locked" in out.lower()

    alice.sendline("ETIRW")
    alice.sendline("EXIT")
    alice.expect(pexpect.EOF)
    alice.close()


def test_concurrent_creates_do_not_corrupt_index(cluster):
    results = {}

    def make(i):
        results[i] = cluster.client([f"CREATE c{i}.txt"], username=f"u{i}")

    threads = [threading.Thread(target=make, args=(i,)) for i in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    out = cluster.client(["VIEW -a"])
    for i in range(4):
        assert f"c{i}.txt" in out
