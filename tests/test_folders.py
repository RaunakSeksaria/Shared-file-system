"""Folders (bonus): CREATEFOLDER / MOVE / VIEWFOLDER."""


def test_create_folder_succeeds(cluster):
    out = cluster.client(["CREATEFOLDER /docs", "VIEWFOLDER /docs"])
    assert "ERROR" not in out


def test_move_file_into_folder(cluster):
    cluster.client(["CREATEFOLDER /docs", "CREATE f.txt", "MOVE f.txt /docs"])
    out = cluster.client(["VIEWFOLDER /docs"])
    assert "f.txt" in out
