# AUTHOR: Claude
# Shared helper for the Python tests: starts the real server binary on a free port and stops it.
# The server binary comes from the JOBQUEUE_SERVER environment variable (default ./build/server),
# so the same tests can run against the sanitizer build too.

import os
import socket
import subprocess
import sys
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER_PATH = os.environ.get("JOBQUEUE_SERVER", os.path.join(REPO_ROOT, "build", "server"))

# Make "import jobqueue" work without installing anything.
sys.path.insert(0, os.path.join(REPO_ROOT, "client"))


# AUTHOR: Claude
def free_port():
    """Asks the OS for a free port: binding to port 0 makes the OS pick one."""
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# AUTHOR: Claude
def start_server(timeout=5.0):
    """Starts the server as a child process on a free port and waits until it accepts connections.

    Returns (process, port). Raises RuntimeError if the server exits or never starts listening.
    """
    port = free_port()
    proc = subprocess.Popen([SERVER_PATH, str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + timeout
    while True:
        if proc.poll() is not None:
            raise RuntimeError("server exited early with code %d" % proc.returncode)
        try:
            socket.create_connection(("127.0.0.1", port), timeout=timeout).close()
            return proc, port
        except ConnectionRefusedError:
            if time.time() > deadline:
                proc.kill()
                raise RuntimeError("server did not start listening on port %d" % port)
            time.sleep(0.05)


# AUTHOR: Claude
def stop_server(proc):
    """Stops the server child process and waits for it to exit."""
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
