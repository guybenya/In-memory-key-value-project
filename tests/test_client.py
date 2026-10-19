# AUTHOR: Claude
# TESTS for the Python client (client/jobqueue/client.py).
# Part 1 checks the RESP encoding/decoding alone (no server). Part 2 runs every command against
# the real server binary. Each test uses its own queue name, so tests never see each other's jobs.
#
# Run: make python-test   (or: python3 -m unittest discover -s tests -p 'test_*.py')

import io
import unittest

from server_helper import start_server, stop_server

from jobqueue import Client, Job, ServerError, MAX_PAYLOAD
from jobqueue.client import encode_command, read_reply


# AUTHOR: Claude
class RespEncodingTest(unittest.TestCase):
    """RESP encoding and decoding, without a server."""

    # AUTHOR: Claude
    def test_encode_mixes_str_bytes_and_int(self):
        """Checks that each argument becomes a length-prefixed bulk string."""
        self.assertEqual(encode_command("RESERVE", b"q", 30),
                         b"*3\r\n$7\r\nRESERVE\r\n$1\r\nq\r\n$2\r\n30\r\n")

    # AUTHOR: Claude
    def test_encode_counts_bytes_not_characters(self):
        """Checks that a non-ASCII str is sized by its UTF-8 bytes (2 bytes for 'é')."""
        self.assertEqual(encode_command("é"), b"*1\r\n$2\r\n\xc3\xa9\r\n")

    # AUTHOR: Claude
    def test_read_every_reply_type(self):
        """Checks that each RESP reply type is decoded to the right Python type."""
        f = io.BytesIO(b"+PONG\r\n:42\r\n$5\r\na\r\nbc\r\n$-1\r\n*2\r\n:1\r\n$0\r\n\r\n")
        self.assertEqual(read_reply(f), "PONG")
        self.assertEqual(read_reply(f), 42)
        self.assertEqual(read_reply(f), b"a\r\nbc")      # \r\n inside a bulk string is just data
        self.assertIsNone(read_reply(f))
        self.assertEqual(read_reply(f), [1, b""])

    # AUTHOR: Claude
    def test_error_reply_raises(self):
        """Checks that an -ERR reply becomes a ServerError with the server's message."""
        with self.assertRaisesRegex(ServerError, "bad thing"):
            read_reply(io.BytesIO(b"-ERR bad thing\r\n"))

    # AUTHOR: Claude
    def test_closed_connection_raises(self):
        """Checks that an empty or cut-off reply raises ConnectionError instead of returning garbage."""
        with self.assertRaises(ConnectionError):
            read_reply(io.BytesIO(b""))
        with self.assertRaises(ConnectionError):
            read_reply(io.BytesIO(b"$10\r\nabc"))


# AUTHOR: Claude
class ClientServerTest(unittest.TestCase):
    """Every client method against the real server."""

    # AUTHOR: Claude
    @classmethod
    def setUpClass(cls):
        """Starts one server for all tests in this class."""
        cls.proc, cls.port = start_server()

    # AUTHOR: Claude
    @classmethod
    def tearDownClass(cls):
        """Stops the server."""
        stop_server(cls.proc)

    # AUTHOR: Claude
    def setUp(self):
        """Opens a fresh connection and picks a queue name unique to this test."""
        self.c = Client(port=self.port)
        self.q = self.id().split(".")[-1]

    # AUTHOR: Claude
    def tearDown(self):
        """Closes the connection."""
        self.c.close()

    # AUTHOR: Claude
    def test_ping(self):
        """Checks the server answers PING."""
        self.assertTrue(self.c.ping())

    # AUTHOR: Claude
    def test_push_reserve_ack_cycle(self):
        """Checks the full life of one job: push -> reserve (attempt 1) -> ack -> gone."""
        job_id = self.c.push(self.q, "hello")
        job = self.c.reserve(self.q, 30)
        self.assertEqual(job, Job(job_id, b"hello", 1))
        self.assertTrue(self.c.ack(job.id))
        self.assertFalse(self.c.ack(job.id))             # second ack: id no longer exists
        self.assertIsNone(self.c.reserve(self.q, 30))

    # AUTHOR: Claude
    def test_fifo_order(self):
        """Checks jobs come out in the order they were pushed."""
        ids = [self.c.push(self.q, "job %d" % i) for i in range(3)]
        got = [self.c.reserve(self.q, 30).id for _ in range(3)]
        self.assertEqual(got, ids)

    # AUTHOR: Claude
    def test_binary_payload_round_trip(self):
        """Checks payload bytes come back unchanged, including \\0, \\r\\n and non-UTF-8 bytes."""
        payload = bytes(range(256)) + b"\r\n\x00end"
        self.c.push(self.q, payload)
        self.assertEqual(self.c.reserve(self.q, 30).payload, payload)

    # AUTHOR: Claude
    def test_stats(self):
        """Checks STATS is turned into a dict with all four counters."""
        self.c.push(self.q, "a")
        self.c.push(self.q, "b")
        job = self.c.reserve(self.q, 30)
        self.c.ack(job.id)
        self.c.reserve(self.q, 30)
        self.assertEqual(self.c.stats(self.q), {"ready": 0, "reserved": 1, "acked": 1, "dead": 0})

    # AUTHOR: Claude
    def test_server_error_is_raised(self):
        """Checks a rejected argument (timeout 0) raises ServerError and the connection still works."""
        with self.assertRaisesRegex(ServerError, "timeout"):
            self.c.reserve(self.q, 0)
        self.assertTrue(self.c.ping())

    # AUTHOR: Claude
    def test_payload_too_large_is_rejected_locally(self):
        """Checks an oversized payload raises ValueError before anything is sent."""
        with self.assertRaises(ValueError):
            self.c.push(self.q, b"x" * (MAX_PAYLOAD + 1))
        self.assertTrue(self.c.ping())

    # AUTHOR: Claude
    def test_max_payload_is_accepted(self):
        """Checks a payload of exactly MAX_PAYLOAD bytes is accepted by the server (the limits agree)."""
        self.c.push(self.q, b"x" * MAX_PAYLOAD)
        self.assertEqual(len(self.c.reserve(self.q, 30).payload), MAX_PAYLOAD)


if __name__ == "__main__":
    unittest.main()
