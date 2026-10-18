# AUTHOR: Claude
# INTEGRATION TEST: starts the real server binary and talks to it over a real TCP socket,
# exactly like redis-cli does. Unit tests check the parser alone; this checks the whole path:
# read() -> input buffer -> parser -> command -> reply buffer -> send.
#
# Python standard library only (no pip install), so it runs anywhere, including CI.
# Usage: python3 tests/integration_test.py ./build/server

import socket
import subprocess
import sys
import time
import unittest

SERVER_PATH = "./build/server"   # replaced by the command-line argument, see the bottom of the file
TIMEOUT = 5                      # seconds; a test fails instead of hanging forever if a reply never comes


# AUTHOR: Claude
class RespError(Exception):
    """An error reply from the server (-ERR ...)."""


# AUTHOR: Claude
def encode_command(*args):
    """Builds one RESP command: an array of bulk strings, e.g. *1\\r\\n$4\\r\\nPING\\r\\n."""
    out = b"*%d\r\n" % len(args)
    for arg in args:
        if isinstance(arg, str):
            arg = arg.encode()
        out += b"$%d\r\n%s\r\n" % (len(arg), arg)
    return out


# AUTHOR: Claude
def read_reply(f):
    """Reads ONE reply from a buffered socket file. Returns str / int / bytes / None, or RespError for -ERR."""
    line = f.readline()
    if not line:
        raise ConnectionError("server closed the connection")
    if not line.endswith(b"\r\n"):
        raise AssertionError("reply line does not end with \\r\\n: %r" % line)
    kind, body = line[:1], line[1:-2]
    if kind == b"+":
        return body.decode()
    if kind == b"-":
        return RespError(body.decode())
    if kind == b":":
        return int(body)
    if kind == b"$":
        length = int(body)
        if length == -1:
            return None
        data = f.read(length + 2)          # the data + its trailing \r\n
        if data[-2:] != b"\r\n":
            raise AssertionError("bulk reply does not end with \\r\\n: %r" % data)
        return data[:-2]
    raise AssertionError("unknown reply type: %r" % line)


# AUTHOR: Claude
def free_port():
    """Asks the OS for a free port: binding to port 0 makes the OS pick one."""
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# AUTHOR: Claude
class Connection:
    """One client connection. Use with 'with' so it is always closed -
    the server still serves ONE client at a time, so the next test can only connect after this one closes."""

    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=TIMEOUT)
        # Send small writes immediately instead of merging them - needed for the byte-by-byte test.
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.sock.makefile("rb")

    def send_raw(self, data):
        self.sock.sendall(data)

    def call(self, *args):
        self.send_raw(encode_command(*args))
        return read_reply(self.file)

    def read_reply(self):
        return read_reply(self.file)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.file.close()
        self.sock.close()


# AUTHOR: Claude
class ServerTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        # Start the server as a child process on a free port, so it never clashes with a server you're running.
        cls.port = free_port()
        cls.proc = subprocess.Popen([SERVER_PATH, str(cls.port)],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # Wait until it accepts connections (it needs a moment to start).
        deadline = time.time() + TIMEOUT
        while True:
            if cls.proc.poll() is not None:
                raise RuntimeError("server exited early with code %d" % cls.proc.returncode)
            try:
                socket.create_connection(("127.0.0.1", cls.port), timeout=TIMEOUT).close()
                return
            except ConnectionRefusedError:
                if time.time() > deadline:
                    cls.proc.kill()
                    raise RuntimeError("server did not start listening on port %d" % cls.port)
                time.sleep(0.05)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(timeout=TIMEOUT)

    def connect(self):
        return Connection(self.port)

    def assertError(self, reply, text):
        self.assertIsInstance(reply, RespError)
        self.assertIn(text, str(reply))

    # --- Basic commands ---

    def test_ping(self):
        with self.connect() as c:
            self.assertEqual(c.call("PING"), "PONG")

    def test_set_get_del(self):
        with self.connect() as c:
            self.assertEqual(c.call("SET", "protein", "MKTAYIAK"), "OK")
            self.assertEqual(c.call("GET", "protein"), b"MKTAYIAK")
            self.assertEqual(c.call("SET", "protein", "GGG"), "OK")       # overwrite
            self.assertEqual(c.call("GET", "protein"), b"GGG")
            self.assertEqual(c.call("DEL", "protein", "missing"), 1)     # only one key existed
            self.assertIsNone(c.call("GET", "protein"))

    def test_data_survives_reconnect(self):
        with self.connect() as c:
            c.call("SET", "sample", "plate-7")
        with self.connect() as c:
            self.assertEqual(c.call("GET", "sample"), b"plate-7")

    def test_commands_are_case_insensitive(self):
        with self.connect() as c:
            self.assertEqual(c.call("set", "k", "v"), "OK")
            self.assertEqual(c.call("GeT", "k"), b"v")

    def test_value_with_spaces_and_newlines(self):
        # RESP sends a length before the data, so the value may contain any bytes, even \r\n.
        with self.connect() as c:
            c.call("SET", "note", "two words\r\nand a line")
            self.assertEqual(c.call("GET", "note"), b"two words\r\nand a line")

    def test_large_value(self):
        # 200 KB arrives in many read() calls (4 KB each) - the input buffer has to grow and collect them all.
        value = b"ACGT" * 50000
        with self.connect() as c:
            self.assertEqual(c.call("SET", "big", value), "OK")
            self.assertEqual(c.call("GET", "big"), value)

    # --- How the bytes arrive ---

    def test_pipelining(self):
        # Three commands in ONE send -> three replies, in order.
        with self.connect() as c:
            c.send_raw(encode_command("SET", "a", "1") +
                       encode_command("GET", "a") +
                       encode_command("PING"))
            self.assertEqual(c.read_reply(), "OK")
            self.assertEqual(c.read_reply(), b"1")
            self.assertEqual(c.read_reply(), "PONG")

    def test_command_sent_byte_by_byte(self):
        # The worst case of a partial read: every byte arrives separately.
        # The server must keep the unfinished command and answer only when it is complete.
        with self.connect() as c:
            cmd = encode_command("SET", "slow", "value")
            for i in range(len(cmd)):
                c.send_raw(cmd[i:i + 1])
                time.sleep(0.002)
            self.assertEqual(c.read_reply(), "OK")
            self.assertEqual(c.call("GET", "slow"), b"value")

    # --- Errors ---

    def test_unknown_command_keeps_connection_open(self):
        with self.connect() as c:
            self.assertError(c.call("FOO"), "unknown command 'FOO'")
            self.assertEqual(c.call("PING"), "PONG")

    def test_wrong_number_of_arguments(self):
        with self.connect() as c:
            self.assertError(c.call("GET"), "wrong number of arguments for 'get'")
            self.assertError(c.call("SET", "only-key"), "wrong number of arguments for 'set'")
            self.assertError(c.call("DEL"), "wrong number of arguments for 'del'")
            self.assertEqual(c.call("PING"), "PONG")

    def test_null_byte_rejected(self):
        with self.connect() as c:
            self.assertError(c.call("SET", "k", b"a\x00b"), "null bytes")

    def test_protocol_error_closes_connection(self):
        # Inline commands (plain text, no RESP) are not supported -> error reply, then the server hangs up.
        with self.connect() as c:
            c.send_raw(b"PING\r\n")
            self.assertError(c.read_reply(), "Protocol error")
            self.assertEqual(c.sock.recv(1), b"")    # b"" = the server closed the connection
        # The server itself is fine and accepts the next client.
        with self.connect() as c:
            self.assertEqual(c.call("PING"), "PONG")


if __name__ == "__main__":
    if len(sys.argv) > 1:
        SERVER_PATH = sys.argv.pop(1)   # remove it so unittest doesn't treat it as a test name
    unittest.main(verbosity=2)
