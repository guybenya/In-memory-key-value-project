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
    if kind == b"*":
        # An array: a count, then that many replies of any type (read with this same function).
        return [read_reply(f) for _ in range(int(body))]
    raise AssertionError("unknown reply type: %r" % line)


# AUTHOR: Claude
def free_port():
    """Asks the OS for a free port: binding to port 0 makes the OS pick one."""
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# AUTHOR: Claude
class Connection:
    """One client connection. Use with 'with' so it is always closed, even when a test fails."""

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

    # --- Many clients at once (the poll() event loop) ---

    def test_two_clients_interleaved(self):
        with self.connect() as a, self.connect() as b:
            self.assertEqual(a.call("SET", "shared", "from-a"), "OK")
            self.assertEqual(b.call("GET", "shared"), b"from-a")      # b sees a's write
            self.assertEqual(b.call("SET", "shared", "from-b"), "OK")
            self.assertEqual(a.call("GET", "shared"), b"from-b")

    def test_stuck_client_does_not_block_others(self):
        # 'a' sends HALF a command and goes quiet. With the old blocking server, 'b' would hang here forever.
        cmd = encode_command("SET", "half", "done")
        with self.connect() as a, self.connect() as b:
            a.send_raw(cmd[:10])
            self.assertEqual(b.call("PING"), "PONG")
            self.assertEqual(b.call("SET", "other", "x"), "OK")
            a.send_raw(cmd[10:])                                     # now 'a' finishes its command
            self.assertEqual(a.read_reply(), "OK")
            self.assertEqual(b.call("GET", "half"), b"done")

    def test_many_clients(self):
        # 50 connections open at the same time, each with its own key.
        conns = [self.connect() for _ in range(50)]
        try:
            for i, c in enumerate(conns):
                self.assertEqual(c.call("SET", "worker:%d" % i, "job-%d" % i), "OK")
            for i, c in enumerate(conns):
                self.assertEqual(c.call("GET", "worker:%d" % i), b"job-%d" % i)
        finally:
            for c in conns:
                c.__exit__()

    def test_client_disconnects_mid_command(self):
        # A client that leaves in the middle of a command must not crash the server or leave it stuck.
        with self.connect() as a:
            a.send_raw(encode_command("SET", "k", "v")[:7])
        with self.connect() as b:
            self.assertEqual(b.call("PING"), "PONG")

    def test_slow_reader_does_not_block_others(self):
        # 'slow' asks for ~20 MB of replies and doesn't read them. The OS socket buffers fill up, so the
        # server must keep the rest and send it later (POLLOUT) - while still serving 'fast' in the meantime.
        value = b"x" * 1000000
        with self.connect() as slow, self.connect() as fast:
            self.assertEqual(slow.call("SET", "huge", value), "OK")
            slow.send_raw(encode_command("GET", "huge") * 20)        # 20 GETs, no reading
            time.sleep(0.2)
            self.assertEqual(fast.call("PING"), "PONG")              # the server is not stuck on 'slow'
            for _ in range(20):                                      # now 'slow' reads everything, intact
                self.assertEqual(slow.read_reply(), value)

    # --- Job queue ---
    # Every test uses its own queue name, because all tests share one server.

    def stats(self, conn, queue):
        """Returns STATS as a dict, e.g. {"ready": 2, "reserved": 0, "acked": 5, "dead": 0}."""
        reply = conn.call("STATS", queue)
        return {reply[i].decode(): reply[i + 1] for i in range(0, len(reply), 2)}

    def test_push_reserve_ack(self):
        with self.connect() as c:
            job_id = c.call("PUSH", "q-basic", "seq-batch-1")
            self.assertIsInstance(job_id, int)
            self.assertEqual(c.call("RESERVE", "q-basic", "30"), [job_id, b"seq-batch-1", 1])
            self.assertEqual(self.stats(c, "q-basic"), {"ready": 0, "reserved": 1, "acked": 0, "dead": 0})
            self.assertEqual(c.call("ACK", str(job_id)), 1)
            self.assertEqual(c.call("ACK", str(job_id)), 0)          # already finished
            self.assertEqual(self.stats(c, "q-basic"), {"ready": 0, "reserved": 0, "acked": 1, "dead": 0})

    def test_reserve_from_empty_queue(self):
        with self.connect() as c:
            self.assertIsNone(c.call("RESERVE", "q-empty", "30"))

    def test_producer_and_worker_are_different_clients(self):
        # Jobs pushed by one client come out, oldest first, for another client.
        with self.connect() as producer, self.connect() as worker:
            for n in range(3):
                producer.call("PUSH", "q-fifo", "job-%d" % n)
            for n in range(3):
                self.assertEqual(worker.call("RESERVE", "q-fifo", "30")[1], b"job-%d" % n)

    def test_binary_payload(self):
        # Queue payloads are stored with their length, so any byte is allowed - even a null byte.
        payload = b"\x00\x01 two\r\nlines \xff"
        with self.connect() as c:
            c.call("PUSH", "q-binary", payload)
            self.assertEqual(c.call("RESERVE", "q-binary", "30")[1], payload)

    def test_visibility_timeout(self):
        # A worker reserves a job for 1 second and never ACKs it (as if it hung or crashed).
        with self.connect() as stuck_worker, self.connect() as other_worker:
            job_id = stuck_worker.call("PUSH", "q-timeout", "work")
            self.assertEqual(stuck_worker.call("RESERVE", "q-timeout", "1")[0], job_id)
            self.assertIsNone(other_worker.call("RESERVE", "q-timeout", "1"))   # hidden while reserved
            time.sleep(1.3)
            # The deadline passed, so the server returned the job - with no client doing anything.
            self.assertEqual(other_worker.call("RESERVE", "q-timeout", "30"), [job_id, b"work", 2])

    def test_no_job_is_given_to_two_workers(self):
        # 5 workers take turns reserving 100 jobs: every job goes to exactly one worker.
        workers = [self.connect() for _ in range(5)]
        try:
            for n in range(100):
                workers[0].call("PUSH", "q-many", "job-%d" % n)
            seen = []
            while True:
                replies = [w.call("RESERVE", "q-many", "30") for w in workers]
                got = [r for r in replies if r is not None]
                if not got:
                    break
                seen.extend(r[0] for r in got)
            self.assertEqual(len(seen), 100)
            self.assertEqual(len(set(seen)), 100)                   # no id twice
            for job_id in seen:
                self.assertEqual(workers[1].call("ACK", str(job_id)), 1)
            self.assertEqual(self.stats(workers[2], "q-many")["acked"], 100)
        finally:
            for w in workers:
                w.__exit__()

    def test_queue_command_errors(self):
        with self.connect() as c:
            self.assertError(c.call("RESERVE", "q-err", "0"), "timeout must be")
            self.assertError(c.call("RESERVE", "q-err", "abc"), "timeout must be")
            self.assertError(c.call("RESERVE", "q-err", "86401"), "timeout must be")
            self.assertError(c.call("ACK", "-5"), "invalid job id")
            self.assertError(c.call("PUSH", "", "x"), "queue name must be")
            self.assertError(c.call("PUSH", "n" * 65, "x"), "queue name must be")
            self.assertError(c.call("PUSH", "q-err"), "wrong number of arguments for 'push'")
            self.assertError(c.call("STATS"), "wrong number of arguments for 'stats'")
            self.assertEqual(c.call("PING"), "PONG")                 # still connected


if __name__ == "__main__":
    if len(sys.argv) > 1:
        SERVER_PATH = sys.argv.pop(1)   # remove it so unittest doesn't treat it as a test name
    unittest.main(verbosity=2)
