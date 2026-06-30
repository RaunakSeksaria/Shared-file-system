"""WRITE: sentence editing, 0-based word indices, delimiters, error paths."""


def test_write_and_read_roundtrip(cluster):
    out = cluster.write("w.txt", "The quick brown fox.")
    assert "Write Successful!" in out
    assert "The quick brown fox." in cluster.client(["READ w.txt"])


def test_word_index_out_of_range_errors(cluster):
    out = cluster.client(["CREATE r.txt", "WRITE r.txt 0", "5 nope.", "ETIRW"])
    assert "out of range" in out.lower()


def test_sentence_index_out_of_range_errors(cluster):
    # Empty file has no sentence 9.
    out = cluster.client(["CREATE s.txt", "WRITE s.txt 9", "ETIRW"])
    assert "ERROR" in out


def test_multiple_edits_in_one_session(cluster):
    # Two edits in one session; the second operates on the already-modified sentence.
    cluster.client(["CREATE m.txt", "WRITE m.txt 0", "0 world.", "0 hello", "ETIRW"])
    out = cluster.client(["READ m.txt"])
    assert "hello" in out and "world." in out


def test_delimiter_splits_into_sentences(cluster):
    cluster.write("d.txt", "first one. second one.")
    out = cluster.client(["READ d.txt"])
    assert "first one." in out and "second one." in out


def test_mid_word_delimiter_splits_sentences(cluster):
    # Spec: every '.'/'!'/'?' delimits a sentence, even mid-token like "e.g.".
    # So "e.g." becomes two sentences ("e." and "g."), and READ space-joins sentences.
    cluster.write("e.txt", "e.g. done.")
    out = cluster.client(["READ e.txt"])
    assert "e. g. done." in out


def test_write_requires_write_access(cluster):
    cluster.write("p.txt", "owner only.", username="alice")
    out = cluster.client(["WRITE p.txt 0", "0 hacked", "ETIRW"], username="mallory")
    assert "ERROR" in out
