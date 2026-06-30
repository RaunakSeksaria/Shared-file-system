"""Folders (bonus): CREATEFOLDER / MOVE / VIEWFOLDER."""

import pytest


def test_create_folder_succeeds(cluster):
    out = cluster.client(["CREATEFOLDER /docs", "VIEWFOLDER /docs"])
    assert "ERROR" not in out


@pytest.mark.xfail(
    reason="BUG: MOVE bakes the folder into the filename (file becomes '/docs/f.txt') instead "
    "of setting a folder_path, so VIEWFOLDER queries by path and finds nothing. Fix in Phase 4.",
    strict=True,
)
def test_move_file_into_folder(cluster):
    cluster.client(["CREATEFOLDER /docs", "CREATE f.txt", "MOVE f.txt /docs"])
    out = cluster.client(["VIEWFOLDER /docs"])
    assert "f.txt" in out
