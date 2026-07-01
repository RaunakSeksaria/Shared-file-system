"""Integration-test harness for the distributed file system.

Spins up a real Name Server + Storage Server(s) on ephemeral ports with
throwaway storage directories, and drives the interactive client via pexpect.
Each `cluster` fixture is function-scoped, so every test gets an isolated system.
"""

import os
import pathlib
import signal
import socket
import subprocess
import time

import pexpect
import pytest

REPO = pathlib.Path(__file__).resolve().parent.parent
BIN_NM = REPO / "bin_nm"
BIN_SS = REPO / "bin_ss"
BIN_CLIENT = REPO / "bin_client"


def _free_port() -> int:
    """Ask the OS for an unused localhost TCP port."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _wait_port(port: int, timeout: float = 5.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.02)
    return False


@pytest.fixture(scope="session", autouse=True)
def _binaries():
    """Build the project once per test session.

    DFS_SANITIZE=asan|tsan builds that sanitizer variant (used by CI); otherwise a
    normal build.
    """
    target = {"asan": ["make", "asan"], "tsan": ["make", "tsan"]}.get(
        os.environ.get("DFS_SANITIZE"), ["make"]
    )
    subprocess.run(
        target, cwd=REPO, check=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    for b in (BIN_NM, BIN_SS, BIN_CLIENT):
        assert b.exists(), f"missing binary: {b}"


class Cluster:
    """A running NM plus zero or more storage servers, all on ephemeral ports."""

    def __init__(self, tmp_path: pathlib.Path):
        self.tmp = tmp_path
        self.nm_port = _free_port()
        self.procs: dict[str, subprocess.Popen] = {}
        self.logs: dict[str, pathlib.Path] = {}
        self.ss: dict[str, dict] = {}
        self._start_nm()

    def _spawn(self, name: str, argv: list[str]) -> subprocess.Popen:
        log = self.tmp / f"{name}.log"
        self.logs[name] = log
        proc = subprocess.Popen(
            argv, cwd=self.tmp,
            stdout=open(log, "w"), stderr=subprocess.STDOUT,
        )
        self.procs[name] = proc
        return proc

    def _start_nm(self):
        self._spawn("nm", [str(BIN_NM), "--host", "127.0.0.1", "--port", str(self.nm_port)])
        assert _wait_port(self.nm_port), "NM failed to listen"

    def add_ss(self, name: str = "ss1") -> str:
        port = _free_port()
        storage = self.tmp / f"storage_{name}"
        storage.mkdir(exist_ok=True)
        self._spawn(name, [
            str(BIN_SS),
            "--nm-host", "127.0.0.1", "--nm-port", str(self.nm_port),
            "--host", "127.0.0.1", "--client-port", str(port),
            "--storage", str(storage), "--username", name,
        ])
        self.ss[name] = {"port": port, "storage": storage}
        assert _wait_port(port), f"SS {name} failed to listen"
        self._wait_known(name)
        return name

    def _wait_known(self, ss_name: str, timeout: float = 5.0) -> bool:
        """Wait until the NM log shows it has interacted with this SS."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if ss_name in self.log("nm"):
                return True
            time.sleep(0.03)
        return False

    def log(self, name: str) -> str:
        return self.logs[name].read_text(errors="ignore")

    def kill(self, name: str):
        proc = self.procs.get(name)
        if proc and proc.poll() is None:
            proc.send_signal(signal.SIGKILL)
            proc.wait(timeout=3)

    def client(self, commands, username: str = "alice", timeout: int = 15) -> str:
        """Run a fresh client, send `commands` then EXIT, return cleaned stdout.

        Strips the JSON log lines and command echoes the client emits, so tests
        can assert on the human-facing response text.
        """
        child = pexpect.spawn(
            str(BIN_CLIENT),
            ["--nm-host", "127.0.0.1", "--nm-port", str(self.nm_port), "--username", username],
            encoding="utf-8", timeout=timeout,
        )
        try:
            child.setecho(False)
            child.waitnoecho(timeout=1)
        except (pexpect.TIMEOUT, OSError):
            pass
        sent = list(commands)
        for cmd in sent:
            child.sendline(cmd)
        child.sendline("EXIT")
        child.expect(pexpect.EOF)
        raw = child.before or ""
        child.close()

        cleaned = []
        for line in raw.splitlines():
            stripped = line.strip()
            if stripped.startswith('{"ts"'):
                continue          # JSON log line
            if stripped in sent or stripped == "EXIT":
                continue          # residual command echo
            cleaned.append(line)
        return "\n".join(cleaned)

    def write(self, filename, content, sentence=0, index=0, username="alice", create=True):
        """Convenience: CREATE (optional) then a one-edit WRITE session (0-based index)."""
        cmds = []
        if create:
            cmds.append(f"CREATE {filename}")
        cmds += [f"WRITE {filename} {sentence}", f"{index} {content}", "ETIRW"]
        return self.client(cmds, username=username)

    def spawn(self, username="alice", timeout=20):
        """Spawn a client and return the raw pexpect child for step-by-step control
        (e.g. holding a WRITE lock open while another client tries the same sentence)."""
        child = pexpect.spawn(
            str(BIN_CLIENT),
            ["--nm-host", "127.0.0.1", "--nm-port", str(self.nm_port), "--username", username],
            encoding="utf-8", timeout=timeout,
        )
        try:
            child.setecho(False)
            child.waitnoecho(timeout=1)
        except (pexpect.TIMEOUT, OSError):
            pass
        return child

    def restart_ss(self, name):
        """Kill an SS and start a fresh one on the same storage dir + username."""
        self.kill(name)
        storage = self.ss[name]["storage"]
        port = _free_port()
        self._spawn(name, [
            str(BIN_SS),
            "--nm-host", "127.0.0.1", "--nm-port", str(self.nm_port),
            "--host", "127.0.0.1", "--client-port", str(port),
            "--storage", str(storage), "--username", name,
        ])
        self.ss[name]["port"] = port
        assert _wait_port(port), f"SS {name} failed to restart"
        self._wait_known(name)
        return name

    def wait_log(self, name, needle, timeout=30.0):
        """Poll a component's log until `needle` appears (for async failover/heartbeat)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.log(name):
                return True
            time.sleep(0.1)
        return False

    def storage_file(self, ss_name, filename):
        """Return a file's on-disk content under an SS storage dir, or None."""
        path = self.ss[ss_name]["storage"] / "files" / filename
        return path.read_text(errors="ignore") if path.exists() else None

    def teardown(self):
        for proc in self.procs.values():
            if proc.poll() is None:
                proc.send_signal(signal.SIGKILL)
        for proc in self.procs.values():
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                pass


@pytest.fixture
def cluster(tmp_path):
    """A ready-to-use system: NM + one storage server."""
    c = Cluster(tmp_path)
    c.add_ss("ss1")
    yield c
    c.teardown()
