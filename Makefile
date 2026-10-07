CC      = clang
CFLAGS  = -Wall -Wextra -g -Isrc
BUILD   = build

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

.PHONY: all test mutation clean
