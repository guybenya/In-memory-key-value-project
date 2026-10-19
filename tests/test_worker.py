# AUTHOR: Claude
# TESTS for the worker framework (client/jobqueue/worker.py), against the real server.
# Covers: normal processing, a failing handler (job retried after its timeout), the empty-queue
# back-off, a worker killed with SIGKILL in the middle of a job (the job is not lost), and a clean
# stop on SIGINT (the current job is finished and acked before exiting).

import logging
import os
import signal
import subprocess
import sys
import threading
import time
import unittest

from server_helper import REPO_ROOT, start_server, stop_server

from jobqueue import Client, Worker
from jobqueue.worker import next_idle_sleep

# A worker in a separate process, for the tests that send it signals.
# argv: port, queue, seconds the handler sleeps. It prints "started <id>" when a job begins.
CHILD_WORKER = """
import sys, time
from jobqueue import Worker
def handle(job):
    print("started", job.id, flush=True)
    time.sleep(float(sys.argv[3]))
Worker(sys.argv[2], handle, port=int(sys.argv[1]), visibility_timeout=1, idle_sleep=0.05).run()
"""


# AUTHOR: Claude
def wait_until(condition, timeout=10.0):
    """Polls condition() every 50 ms until it returns true; fails the test if it never does."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if condition():
            return
        time.sleep(0.05)
    raise AssertionError("condition not met within %.1f s" % timeout)


# AUTHOR: Claude
def run_worker(worker, timeout=15.0):
    """Runs worker.run() in a thread with a time limit, so a bug fails the test instead of hanging it.

    Returns the worker's counts. If the worker is still running after timeout, stops it and fails.
    """
    result = {}
    thread = threading.Thread(target=lambda: result.update(worker.run()))
    thread.start()
    thread.join(timeout)
    if thread.is_alive():
        worker.stop()
        thread.join()
        raise AssertionError("worker did not finish within %.1f s" % timeout)
    return result


# AUTHOR: Claude
class BackoffTest(unittest.TestCase):

    # AUTHOR: Claude
    def test_idle_sleep_doubles_up_to_the_cap(self):
        """Checks the empty-queue sleep doubles each time and stops growing at the maximum."""
        sleeps = [0.1]
        for _ in range(6):
            sleeps.append(next_idle_sleep(sleeps[-1], 2.0))
        self.assertEqual(sleeps, [0.1, 0.2, 0.4, 0.8, 1.6, 2.0, 2.0])


# AUTHOR: Claude
class WorkerTest(unittest.TestCase):

    # AUTHOR: Claude
    @classmethod
    def setUpClass(cls):
        """Starts one server for all tests in this class, and silences the worker's log lines."""
        cls.proc, cls.port = start_server()
        logging.getLogger("jobqueue.worker").setLevel(logging.CRITICAL)

    # AUTHOR: Claude
    @classmethod
    def tearDownClass(cls):
        """Stops the server."""
        stop_server(cls.proc)

    # AUTHOR: Claude
    def setUp(self):
        """Opens a connection for checking STATS and picks a queue name unique to this test."""
        self.c = Client(port=self.port)
        self.q = self.id().split(".")[-1]

    # AUTHOR: Claude
    def tearDown(self):
        """Closes the connection."""
        self.c.close()

    # AUTHOR: Claude
    def spawn_child(self, handler_seconds):
        """Starts CHILD_WORKER as a separate Python process on this test's queue and returns it.

        A watchdog kills the child after 20 s, so a stuck child makes readline() return "" and the
        test fail, instead of hanging forever.
        """
        env = dict(os.environ, PYTHONPATH=os.path.join(REPO_ROOT, "client"))
        child = subprocess.Popen([sys.executable, "-c", CHILD_WORKER, str(self.port), self.q, str(handler_seconds)],
                                 env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        watchdog = threading.Timer(20, child.kill)
        watchdog.daemon = True
        watchdog.start()
        self.addCleanup(watchdog.cancel)
        return child

    # AUTHOR: Claude
    def test_processes_and_acks_every_job(self):
        """Checks the handler sees every payload once and every job ends up acked."""
        for i in range(5):
            self.c.push(self.q, "job %d" % i)
        seen = []
        counts = run_worker(Worker(self.q, lambda job: seen.append(job.payload), port=self.port, max_jobs=5))
        self.assertEqual(sorted(seen), [b"job %d" % i for i in range(5)])
        self.assertEqual(counts["acked"], 5)
        self.assertEqual(self.c.stats(self.q), {"ready": 0, "reserved": 0, "acked": 5, "dead": 0})

    # AUTHOR: Claude
    def test_failed_job_is_retried_after_timeout(self):
        """Checks a handler exception means no ack: the job returns after its timeout as attempt 2."""
        self.c.push(self.q, "flaky")
        attempts = []

        def flaky(job):
            attempts.append(job.attempts)
            if job.attempts == 1:
                raise RuntimeError("first try fails")

        counts = run_worker(Worker(self.q, flaky, port=self.port, visibility_timeout=1,
                                   idle_sleep=0.05, max_idle_sleep=0.2, max_jobs=2))
        self.assertEqual(attempts, [1, 2])
        self.assertEqual((counts["failed"], counts["retries"], counts["acked"]), (1, 1, 1))
        self.assertEqual(self.c.stats(self.q)["acked"], 1)

    # AUTHOR: Claude
    def test_job_that_always_fails_ends_up_dead(self):
        """Checks a job whose handler always raises is given up after 5 attempts (dead, not lost)."""
        self.c.push(self.q, "poison")

        def always_fails(job):
            raise ValueError("bad input")

        run_worker(Worker(self.q, always_fails, port=self.port, visibility_timeout=1,
                          idle_sleep=0.05, max_idle_sleep=0.2, max_jobs=5))
        wait_until(lambda: self.c.stats(self.q)["dead"] == 1)
        self.assertEqual(self.c.stats(self.q), {"ready": 0, "reserved": 0, "acked": 0, "dead": 1})

    # AUTHOR: Claude
    def test_stop_wakes_an_idle_worker(self):
        """Checks stop() ends a worker that is sleeping on an empty queue, without waiting for the sleep."""
        worker = Worker(self.q, lambda job: None, port=self.port, idle_sleep=30, max_idle_sleep=30)
        thread = threading.Thread(target=worker.run)
        thread.start()
        time.sleep(0.3)                           # let it find the queue empty and start sleeping
        started = time.time()
        worker.stop()
        thread.join(timeout=5)
        self.assertFalse(thread.is_alive())
        self.assertLess(time.time() - started, 2)

    # AUTHOR: Claude
    def test_job_survives_worker_killed_mid_job(self):
        """Kills a worker with SIGKILL while it holds a job; checks the job comes back and finishes."""
        job_id = self.c.push(self.q, "survivor")
        child = self.spawn_child(handler_seconds=30)
        try:
            self.assertEqual(child.stdout.readline().split(), ["started", str(job_id)])
            child.kill()                          # SIGKILL: no cleanup, no ack - like a crash
            child.wait()
        finally:
            if child.poll() is None:
                child.kill()
            child.stdout.close()
        self.assertEqual(self.c.stats(self.q)["reserved"], 1)
        seen = []
        run_worker(Worker(self.q, seen.append, port=self.port, idle_sleep=0.05, max_idle_sleep=0.2, max_jobs=1))
        self.assertEqual([(j.id, j.attempts) for j in seen], [(job_id, 2)])
        self.assertEqual(self.c.stats(self.q), {"ready": 0, "reserved": 0, "acked": 1, "dead": 0})

    # AUTHOR: Claude
    def test_sigint_finishes_current_job_then_exits(self):
        """Sends Ctrl+C (SIGINT) mid-job; checks the worker finishes and acks the job, then exits cleanly."""
        self.c.push(self.q, "finish me")
        child = self.spawn_child(handler_seconds=0.5)
        try:
            self.assertEqual(child.stdout.readline().split()[:1], ["started"])
            child.send_signal(signal.SIGINT)
            self.assertEqual(child.wait(timeout=10), 0)
        finally:
            if child.poll() is None:
                child.kill()
            child.stdout.close()
        self.assertEqual(self.c.stats(self.q), {"ready": 0, "reserved": 0, "acked": 1, "dead": 0})


if __name__ == "__main__":
    unittest.main()
