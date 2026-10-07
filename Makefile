CC      = clang
CFLAGS  = -Wall -Wextra -g -Isrc
BUILD   = build

SERVER_OBJS = $(BUILD)/main.o $(BUILD)/server.o $(BUILD)/database.o $(BUILD)/resp.o
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
SAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer

asan: tests/test_database.c src/database.c src/database.h | $(BUILD)
	$(CC) $(CFLAGS) $(SAN_FLAGS) tests/test_database.c src/database.c -o $(BUILD)/test_asan
	./$(BUILD)/test_asan

# LEAKS: AddressSanitizer can't detect leaks on macOS, so use the built-in 'leaks' tool.
# It runs the tests and reports heap memory still allocated at exit (exit code 1 = leaks found).
leaks: $(BUILD)/test_database
	leaks --atExit -- ./$(BUILD)/test_database

# CHECK: Everything at once - regular tests, sanitizers and leak detection. Run before every commit.
check: test asan leaks

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
