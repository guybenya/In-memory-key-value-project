# AUTHOR: Claude
# WORKER FRAMEWORK: runs a user function on every job of one queue.
#
#     def handle(job):                 # job.id, job.payload (bytes), job.attempts
#         ...do the work...            # raise an exception to mark the job as failed
#
#     Worker("emails", handle, port=8080).run()
#
# The loop: RESERVE -> handler(job) -> ACK.
#   - Empty queue: RESERVE never blocks, so the worker sleeps a little before asking again,
#     and doubles the sleep each time the queue is still empty (up to max_idle_sleep).
#   - Handler raised: the job is NOT acked. The server returns it to the queue when its
#     visibility timeout ends, and after QUEUE_MAX_ATTEMPTS (5) tries it goes to "dead".
#   - attempts > 1: this job already failed or timed out somewhere - logged as a retry.
#   - Ctrl+C / SIGTERM: finish the current job, ack it, then exit. A second Ctrl+C stops at once
#     (the unfinished job comes back after its timeout, like after a crash).
#
# Delivery is AT-LEAST-ONCE: a job can run twice (e.g. a worker is killed after doing the work
# but before its ACK). Handlers must therefore be idempotent - running twice = same result.

import logging
import os
import signal
import threading
import time

from .client import Client

log = logging.getLogger("jobqueue.worker")


# AUTHOR: Claude
def next_idle_sleep(current, maximum):
    """Returns how long to sleep after the next empty RESERVE: double the last sleep, capped at maximum."""
    return min(current * 2, maximum)


# AUTHOR: Claude
class Worker:
    """Pulls jobs from one queue and runs handler(job) on each, until stopped."""

    # AUTHOR: Claude
    def __init__(self, queue, handler, host="127.0.0.1", port=8080, visibility_timeout=60,
                 idle_sleep=0.1, max_idle_sleep=2.0, max_jobs=None, reconnect_attempts=5, name=None):
        """Stores the settings; nothing connects until run().

        queue              - queue name to RESERVE from
        handler            - function(job); return normally = success (job is acked), raise = failure
        visibility_timeout - seconds a reserved job stays hidden from other workers; must be longer
                             than the slowest handler call, or the job is handed out twice
        idle_sleep         - first sleep (seconds) after finding the queue empty
        max_idle_sleep     - the sleep never grows beyond this
        max_jobs           - stop after this many handled jobs (None = run until stopped); for tests/demos
        reconnect_attempts - how many times to retry the connection before giving up
        name               - label for log lines (default: "worker-<pid>")
        """
        self.queue = queue
        self.handler = handler
        self.host = host
        self.port = port
        self.visibility_timeout = visibility_timeout
        self.idle_sleep = idle_sleep
        self.max_idle_sleep = max_idle_sleep
        self.max_jobs = max_jobs
        self.reconnect_attempts = reconnect_attempts
        self.name = name or "worker-%d" % os.getpid()
        self.client = None
        self._stop = threading.Event()
        # Counters, returned by run(): handled jobs, acked jobs, failed handler calls, retries seen,
        # and late acks (ACK returned 0: the job had already timed out and been handled elsewhere).
        self.counts = {"handled": 0, "acked": 0, "failed": 0, "retries": 0, "late_acks": 0}

    # AUTHOR: Claude
    def stop(self):
        """Asks the loop to exit after the current job. Safe to call from another thread or a signal handler."""
        self._stop.set()

    # AUTHOR: Claude
    def _on_signal(self, signum, frame):
        """Signal handler: the first Ctrl+C/SIGTERM asks for a clean stop, the second one stops at once."""
        if self._stop.is_set():
            raise KeyboardInterrupt
        log.info("%s: stop requested - finishing the current job (Ctrl+C again to stop now)", self.name)
        self.stop()

    # AUTHOR: Claude
    def _connect(self):
        """Opens the connection, retrying with a growing pause; raises the last error if every try fails."""
        for attempt in range(1, self.reconnect_attempts + 1):
            try:
                self.client = Client(self.host, self.port)
                return
            except OSError as e:
                if attempt == self.reconnect_attempts or self._stop.is_set():
                    raise
                log.warning("%s: cannot connect to %s:%d (%s), retry %d/%d",
                            self.name, self.host, self.port, e, attempt, self.reconnect_attempts)
                self._stop.wait(attempt)

    # AUTHOR: Claude
    def _call(self, method, *args):
        """Runs one client method (e.g. "reserve"); if the connection broke, reconnects once and retries it."""
        try:
            return getattr(self.client, method)(*args)
        except OSError as e:
            log.warning("%s: connection lost (%s), reconnecting", self.name, e)
            self.client.close()
            self._connect()
            return getattr(self.client, method)(*args)

    # AUTHOR: Claude
    def _handle(self, job):
        """Runs the handler on one job and acks it on success; on an exception, logs it and does NOT ack."""
        self.counts["handled"] += 1
        if job.attempts > 1:
            self.counts["retries"] += 1
            log.warning("%s: job %d is a retry (attempt %d)", self.name, job.id, job.attempts)
        try:
            self.handler(job)
        except Exception:
            # Not acked on purpose: the server re-queues the job after its visibility timeout.
            self.counts["failed"] += 1
            log.exception("%s: job %d failed (attempt %d), it will be retried after the timeout",
                          self.name, job.id, job.attempts)
            return
        if self._call("ack", job.id):
            self.counts["acked"] += 1
        else:
            # The job's timeout ran out while we worked, so another worker got it and acked it first.
            # Our work was still done - with idempotent writes this is harmless.
            self.counts["late_acks"] += 1
            log.warning("%s: late ACK for job %d - its timeout had expired and it was already handled",
                        self.name, job.id)

    # AUTHOR: Claude
    def run(self):
        """The main loop: reserve, handle, ack, until stop() / Ctrl+C / max_jobs. Returns the counts dict."""
        # Signal handlers can only be installed from the main thread (tests run workers in threads too).
        in_main_thread = threading.current_thread() is threading.main_thread()
        if in_main_thread:
            old_int = signal.signal(signal.SIGINT, self._on_signal)
            old_term = signal.signal(signal.SIGTERM, self._on_signal)
        sleep = self.idle_sleep
        try:
            self._connect()
            log.info("%s: working on queue %r at %s:%d", self.name, self.queue, self.host, self.port)
            while not self._stop.is_set():
                if self.max_jobs is not None and self.counts["handled"] >= self.max_jobs:
                    break
                job = self._call("reserve", self.queue, self.visibility_timeout)
                if job is None:
                    # Empty queue: wait, but wake up at once if stop() is called.
                    self._stop.wait(sleep)
                    sleep = next_idle_sleep(sleep, self.max_idle_sleep)
                    continue
                sleep = self.idle_sleep
                self._handle(job)
        finally:
            if in_main_thread:
                signal.signal(signal.SIGINT, old_int)
                signal.signal(signal.SIGTERM, old_term)
            if self.client is not None:
                self.client.close()
        log.info("%s: stopped %s", self.name, self.counts)
        return self.counts
