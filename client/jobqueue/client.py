# AUTHOR: Claude
# CLIENT: a small Python client for the job queue server.
# It opens one TCP connection, sends commands in RESP (the Redis wire protocol) and reads the replies.
#
# Usage:
#     with Client(port=8080) as c:
#         job_id = c.push("emails", b"hello")
#         job = c.reserve("emails", 30)        # Job(id, payload, attempts) or None
#         c.ack(job.id)
#
# Standard library only. Works with Python 3.9+.

import socket
from collections import namedtuple

# The server rejects bulk strings bigger than this (RESP_MAX_BULK_LEN in src/resp.h).
MAX_PAYLOAD = 1024 * 1024

# One reserved job, as returned by Client.reserve().
#   id       - the server's job id (pass it to ack())
#   payload  - the raw bytes that were pushed
#   attempts - how many times this job has been reserved, including this time (1 = first try)
Job = namedtuple("Job", ["id", "payload", "attempts"])


# AUTHOR: Claude
class ServerError(Exception):
    """An error reply from the server (a RESP '-ERR ...' line), e.g. a bad argument."""


# AUTHOR: Claude
def encode_command(*args):
    """Turns a command like ("PUSH", "q", b"data") into RESP bytes: an array of bulk strings.

    str arguments are encoded as UTF-8, int arguments as their decimal text, bytes are sent as they are.
    Bulk strings carry their length up front, so any bytes (even \\r\\n or \\0) are safe inside them.
    """
    parts = [b"*%d\r\n" % len(args)]
    for arg in args:
        if isinstance(arg, int):
            arg = str(arg)
        if isinstance(arg, str):
            arg = arg.encode("utf-8")
        parts.append(b"$%d\r\n%s\r\n" % (len(arg), arg))
    return b"".join(parts)


# AUTHOR: Claude
def read_reply(f):
    """Reads one complete RESP reply from a buffered binary file (a socket's makefile("rb")).

    Returns: str for +simple strings, int for :integers, bytes for $bulk strings,
    None for the nil bulk string ($-1), and a list for *arrays (each element read the same way).
    Raises ServerError for an -error reply, and ConnectionError if the server closed the connection.
    """
    line = f.readline()
    if not line or not line.endswith(b"\r\n"):
        raise ConnectionError("server closed the connection")
    kind, body = line[:1], line[1:-2]
    if kind == b"+":
        return body.decode("utf-8")
    if kind == b"-":
        raise ServerError(body.decode("utf-8", "replace"))
    if kind == b":":
        return int(body)
    if kind == b"$":
        length = int(body)
        if length == -1:
            return None
        data = f.read(length + 2)            # the data plus its trailing \r\n
        if len(data) != length + 2:
            raise ConnectionError("server closed the connection in the middle of a reply")
        if data[-2:] != b"\r\n":
            # The length did not match the data: the stream is out of sync, nothing after this can be trusted.
            raise ConnectionError("malformed bulk reply (missing \\r\\n after %d bytes)" % length)
        return data[:-2]
    if kind == b"*":
        count = int(body)
        if count == -1:
            return None
        return [read_reply(f) for _ in range(count)]
    raise ConnectionError("unexpected reply type %r - is this the job queue server?" % kind)


# AUTHOR: Claude
class Client:
    """One connection to the job queue server, with a method per command.

    Not thread-safe: use one Client per thread or process (each worker process makes its own).
    """

    # AUTHOR: Claude
    def __init__(self, host="127.0.0.1", port=8080, timeout=10.0):
        """Connects to the server. Raises OSError (e.g. ConnectionRefusedError) if it is not running.

        timeout - seconds to wait for a reply before raising socket.timeout, so a dead server
                  cannot hang the caller forever.
        """
        self.host = host
        self.port = port
        self.sock = socket.create_connection((host, port), timeout=timeout)
        # Send each command right away instead of waiting to merge small writes (Nagle's algorithm).
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.sock.makefile("rb")

    # AUTHOR: Claude
    def call(self, *args):
        """Sends one raw command and returns the decoded reply (see read_reply for the types)."""
        self.sock.sendall(encode_command(*args))
        return read_reply(self.file)

    # AUTHOR: Claude
    def ping(self):
        """Checks that the server answers. Returns True on PONG."""
        return self.call("PING") == "PONG"

    # AUTHOR: Claude
    def push(self, queue, payload):
        """Adds a job with this payload (bytes or str) to the end of the queue. Returns the new job id."""
        if isinstance(payload, str):
            payload = payload.encode("utf-8")
        if len(payload) > MAX_PAYLOAD:
            # Checked here because the server answers an oversized command with a protocol error
            # and closes the connection - a clear Python error is easier to debug.
            raise ValueError("payload is %d bytes; the server limit is %d" % (len(payload), MAX_PAYLOAD))
        return self.call("PUSH", queue, payload)

    # AUTHOR: Claude
    def reserve(self, queue, timeout_sec):
        """Takes the oldest ready job from the queue for timeout_sec seconds (the visibility timeout).

        Returns a Job, or None if the queue has no ready job (the call never blocks).
        If the job is not acked within timeout_sec, the server makes it ready again for another worker.
        """
        reply = self.call("RESERVE", queue, int(timeout_sec))
        if reply is None:
            return None
        job_id, payload, attempts = reply
        return Job(job_id, payload, attempts)

    # AUTHOR: Claude
    def ack(self, job_id):
        """Tells the server the job is done, so it is deleted. Returns True if it was deleted,
        False if the server no longer knows this id (e.g. another worker already acked it)."""
        return self.call("ACK", job_id) == 1

    # AUTHOR: Claude
    def stats(self, queue):
        """Returns the queue's counters as a dict: {"ready": n, "reserved": n, "acked": n, "dead": n}."""
        reply = self.call("STATS", queue)
        # The reply is a flat list [name, value, name, value, ...]: pair up even and odd positions.
        return {name.decode("utf-8"): value for name, value in zip(reply[0::2], reply[1::2])}

    # AUTHOR: Claude
    def close(self):
        """Closes the connection. Safe to call twice."""
        try:
            self.file.close()
        finally:
            self.sock.close()

    # AUTHOR: Claude
    def __enter__(self):
        """Lets the client be used in a 'with' block."""
        return self

    # AUTHOR: Claude
    def __exit__(self, *exc):
        """Closes the connection at the end of the 'with' block, even after an exception."""
        self.close()
