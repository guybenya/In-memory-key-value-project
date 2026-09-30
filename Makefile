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

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)

.PHONY: all test clean
