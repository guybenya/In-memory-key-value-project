CC      = clang
CFLAGS  = -Wall -Wextra -g -Isrc
BUILD   = build

# resp.o is left out until src/resp.c exists (RESP stage) - listing it breaks the build.
SERVER_OBJS = $(BUILD)/main.o $(BUILD)/server.o $(BUILD)/database.o
TEST_OBJS   = $(BUILD)/test_database.o $(BUILD)/database.o

all: $(BUILD)/server

$(BUILD)/server: $(SERVER_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_database: $(TEST_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: tests/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

test: $(BUILD)/test_database
	./$(BUILD)/test_database

# SANITIZERS: Rebuild the tests with runtime memory checks.
# AddressSanitizer catches use-after-free, double free and out-of-bounds access.
# UndefinedBehaviorSanitizer catches undefined behavior (e.g. signed integer overflow).
# -fno-omit-frame-pointer keeps the full call chain in error reports.
# -fno-sanitize-recover=all stops the program at the first error with a non-zero exit code.
# Without it, UBSan only prints a warning and continues - the tests would still "pass".
SAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

asan: tests/test_database.c src/database.c src/database.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN_FLAGS) tests/test_database.c src/database.c -o $(BUILD)/test_asan
	./$(BUILD)/test_asan

# LEAKS: AddressSanitizer can't detect leaks on macOS, so use the built-in 'leaks' tool.
# It runs the tests and reports heap memory still allocated at exit (exit code 1 = leaks found).
leaks: $(BUILD)/test_database
	leaks --atExit -- ./$(BUILD)/test_database

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

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)

.PHONY: all test asan leaks check mutation clean
