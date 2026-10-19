CC      = clang
# EXTRA_CFLAGS is empty by default. CI passes EXTRA_CFLAGS=-Werror so any warning fails the build there.
CFLAGS  = -Wall -Wextra -g -Isrc $(EXTRA_CFLAGS)
BUILD   = build

SERVER_OBJS = $(BUILD)/main.o $(BUILD)/server.o $(BUILD)/database.o $(BUILD)/resp.o $(BUILD)/queue.o
TEST_OBJS   = $(BUILD)/test_database.o $(BUILD)/database.o
TEST_RESP_OBJS = $(BUILD)/test_resp.o $(BUILD)/resp.o
TEST_QUEUE_OBJS = $(BUILD)/test_queue.o $(BUILD)/queue.o

all: $(BUILD)/server

$(BUILD)/server: $(SERVER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_database: $(TEST_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_resp: $(TEST_RESP_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_queue: $(TEST_QUEUE_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: tests/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

test: $(BUILD)/test_database $(BUILD)/test_resp $(BUILD)/test_queue
	./$(BUILD)/test_database
	./$(BUILD)/test_resp
	./$(BUILD)/test_queue

# SANITIZERS: Rebuild the tests with runtime memory checks.
# AddressSanitizer catches use-after-free, double free and out-of-bounds access.
# UndefinedBehaviorSanitizer catches undefined behavior (e.g. signed integer overflow).
# -fno-omit-frame-pointer keeps the full call chain in error reports.
# -fno-sanitize-recover=all stops the program at the first error with a non-zero exit code.
# Without it, UBSan only prints a warning and continues - the tests would still "pass".
SAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

asan: tests/test_database.c src/database.c src/database.h tests/test_resp.c src/resp.c src/resp.h tests/test_queue.c src/queue.c src/queue.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN_FLAGS) tests/test_database.c src/database.c -o $(BUILD)/test_asan
	./$(BUILD)/test_asan
	$(CC) $(CFLAGS) $(SAN_FLAGS) tests/test_resp.c src/resp.c -o $(BUILD)/test_resp_asan
	./$(BUILD)/test_resp_asan
	$(CC) $(CFLAGS) $(SAN_FLAGS) tests/test_queue.c src/queue.c -o $(BUILD)/test_queue_asan
	./$(BUILD)/test_queue_asan

# LEAKS: AddressSanitizer can't detect leaks on macOS, so use the built-in 'leaks' tool.
# It runs the tests and reports heap memory still allocated at exit (exit code 1 = leaks found).
leaks: $(BUILD)/test_database $(BUILD)/test_resp $(BUILD)/test_queue
	leaks --atExit -- ./$(BUILD)/test_database
	leaks --atExit -- ./$(BUILD)/test_resp
	leaks --atExit -- ./$(BUILD)/test_queue

# OS DETECTION: 'uname -s' prints "Darwin" on macOS and "Linux" on Linux.
# On Linux, AddressSanitizer includes LeakSanitizer, so 'make asan' already fails on leaks
# and the macOS-only 'leaks' tool is not needed (it doesn't exist there).
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
LEAK_CHECK = leaks
else
LEAK_CHECK =
endif

# CHECK: Everything at once - regular tests, sanitizers and leak detection. Run before every commit.
check: test asan $(LEAK_CHECK)

# MUTATION TEST: Breaks the deep copy on purpose and checks that the tests notice.
# A broken copy of database.c is generated in build/ (the real source is never touched),
# where db_set stores the caller's value pointer instead of a strdup() copy.
# Expected result: the deep_copy test FAILS -> "MUTANT KILLED".
mutation: tests/test_database.c src/database.c | $(BUILD)
	sed 's/newNode->value = strdup(value);/newNode->value = (char *)value;/' src/database.c > $(BUILD)/database_mutant.c
	@# Stop if the line was not found (e.g. the code changed) - otherwise the "mutant" would be identical to the original
	@grep -q 'newNode->value = (char \*)value;' $(BUILD)/database_mutant.c || (echo "Mutation was not applied" && exit 1)
	$(CC) $(CFLAGS) tests/test_database.c $(BUILD)/database_mutant.c -o $(BUILD)/test_mutant
	@if ./$(BUILD)/test_mutant deep_copy; then \
		echo "MUTANT SURVIVED - the test did not catch the bug!"; exit 1; \
	else \
		echo "MUTANT KILLED - the test caught the bug, as expected."; \
	fi

# INTEGRATION TEST: starts the real server on a free port and talks RESP to it over TCP (Python standard library only).
integration: $(BUILD)/server
	python3 tests/integration_test.py ./$(BUILD)/server

# Same integration test, against a server built with AddressSanitizer + UBSan. Catches memory errors in the
# event loop and the command code: on the first error the server aborts, and the remaining tests fail.
integration-asan: $(BUILD)/server_asan
	python3 tests/integration_test.py ./$(BUILD)/server_asan

# The server built with AddressSanitizer + UBSan, used by integration-asan and python-test-asan.
$(BUILD)/server_asan: src/*.c src/*.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN_FLAGS) src/main.c src/server.c src/database.c src/resp.c src/queue.c -o $@

# PYTHON: the project's virtualenv if it exists (it has Biopython for the demo), else the system python3.
# Override with: make python-test PYTHON=/path/to/python
PYTHON ?= $(if $(wildcard .venv/bin/python),.venv/bin/python,python3)

# PYTHON TESTS: the client library and the worker framework, against the real server (see tests/test_*.py).
python-test: $(BUILD)/server
	$(PYTHON) -m unittest discover -s tests -p 'test_*.py' -v

# Same Python tests against the AddressSanitizer + UBSan server build.
python-test-asan: $(BUILD)/server_asan
	JOBQUEUE_SERVER=./$(BUILD)/server_asan $(PYTHON) -m unittest discover -s tests -p 'test_*.py'

# DEMO: the protein batch-analysis pipeline end to end (server, producer, 4 workers, SQLite, anomaly report).
# Needs Biopython: python3 -m venv .venv && .venv/bin/pip install -r demo/requirements.txt
demo: $(BUILD)/server
	$(PYTHON) demo/protein/run_demo.py --workers 4

# CRASH DEMO: the same, but one worker is killed with SIGKILL in the middle of a job. Every job must still finish.
crash-demo: $(BUILD)/server
	$(PYTHON) demo/protein/run_demo.py --workers 4 --kill-one

# Small crash demo for CI: 110 sequences, 2 workers, short timeout. Fails (exit 1) if anything is lost.
demo-ci: $(BUILD)/server
	$(PYTHON) demo/protein/run_demo.py --fasta demo/protein/data/ci_sample_100.fasta demo/protein/data/bad_examples.fasta \
		--workers 2 --batch-size 20 --kill-one --timeout 3

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)

.PHONY: all test asan leaks check mutation integration integration-asan python-test python-test-asan demo crash-demo demo-ci clean
