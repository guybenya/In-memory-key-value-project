#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "database.h"

// TEST: A value that was saved can be retrieved.
static void test_set_and_get(void) {
    HashTable *table = create_table();
    assert(table != NULL);

    assert(db_set(table, "name", "guy") == 0);

    char *value = db_get(table, "name");
    // Check for NULL BEFORE strcmp - strcmp(NULL, ...) would crash instead of failing clearly.
    assert(value != NULL);
    assert(strcmp(value, "guy") == 0);

    db_destroy(table);
}

// TEST: db_get returns NULL for a key that was never saved.
static void test_get_missing_key_returns_null(void) {
    HashTable *table = create_table();
    assert(table != NULL);

    // CASE 1: Empty table - the bucket is NULL, so find_node never enters its loop.
    assert(db_get(table, "ob") == NULL);

    assert(db_set(table, "a", "1") == 0);

    // CASE 2: Non-empty table, missing key that (most likely) lands in a different, empty bucket.
    assert(db_get(table, "wrong_key") == NULL);

    // CASE 3: "ob" collides with "a" (same bucket), so find_node walks the list,
    // compares with strcmp, finds no match and reaches the end - covers the full search path.
    assert(db_get(table, "ob") == NULL);

    db_destroy(table);
}

// HELPER: Verifies the state of every key - a deleted key returns NULL, and every other key
// still returns its original value (its index as a string: "0".."8").
// NOTE: Arrays passed to a function decay into pointers, so sizeof() can't give their length here -
// that's why 'count' is passed as a separate parameter.
static void assert_keys_state(HashTable *table, const char *keys[], const bool deleted[], size_t count) {
    for (size_t i = 0; i < count; i++) {
        char *value = db_get(table, keys[i]);

        if (deleted[i]) {
            assert(value == NULL);
        }
        else {
            // Rebuild the expected value the same way it was inserted
            char expected[16];
            snprintf(expected, sizeof(expected), "%zu", i);

            assert(value != NULL);
            assert(strcmp(value, expected) == 0);
        }
    }
}

// TEST: Deleting from the head, middle and tail of a collision list removes only the target key,
// and leaves every other key - in the same bucket and in other buckets - intact.
static void test_delete_with_collisions(void) {
    HashTable *table = create_table();
    assert(table != NULL);

    // Three groups of three keys. Each group lands in the same bucket (670, 663, 125).
    const char *keys[9] = {"a", "ob", "awq", "dog", "awj", "btt", "cat", "bdj", "ezq"};
    size_t count = sizeof(keys) / sizeof(keys[0]);

    // TEST ASSUMPTION: If the hash function or TABLE_SIZE changes, these keys may stop colliding,
    // and the test would pass without testing collisions at all. Fail loudly instead.
    assert(hash_function("a") == hash_function("ob"));
    assert(hash_function("a") == hash_function("awq"));
    assert(hash_function("dog") == hash_function("awj"));
    assert(hash_function("dog") == hash_function("btt"));
    assert(hash_function("cat") == hash_function("bdj"));
    assert(hash_function("cat") == hash_function("ezq"));

    // Insert every key with its index as the value.
    // Reusing 'buf' is safe only because db_set makes a deep copy (strdup).
    for (size_t i = 0; i < count; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%zu", i);
        assert(db_set(table, keys[i], buf) == 0);
    }

    // Insertion is at the head, so each bucket's list is in reverse order:
    //   bucket 670: awq -> ob  -> a
    //   bucket 663: btt -> awj -> dog
    //   bucket 125: ezq -> bdj -> cat
    bool deleted[9] = {false};
    assert_keys_state(table, keys, deleted, count);

    // HEAD: "awq" is the first node - deleting it must update the bucket slot itself.
    assert(db_delete(table, "awq") == 1);
    deleted[2] = true;
    assert_keys_state(table, keys, deleted, count);

    // MIDDLE: "awj" sits between "btt" and "dog" - deleting it must update btt->next.
    assert(db_delete(table, "awj") == 1);
    deleted[4] = true;
    assert_keys_state(table, keys, deleted, count);

    // TAIL: "cat" is the last node - deleting it must set bdj->next to NULL.
    assert(db_delete(table, "cat") == 1);
    deleted[6] = true;
    assert_keys_state(table, keys, deleted, count);

    // EMPTY BUCKET: Delete the remaining keys of bucket 670, so its slot goes back to NULL.
    assert(db_delete(table, "ob") == 1);
    deleted[1] = true;
    assert(db_delete(table, "a") == 1);
    deleted[0] = true;
    assert_keys_state(table, keys, deleted, count);

    // REUSE: An emptied bucket must accept new insertions.
    assert(db_set(table, "a", "0") == 0);
    deleted[0] = false;
    assert_keys_state(table, keys, deleted, count);

    db_destroy(table);
}

// TEST: Calling db_set on an existing key replaces its value instead of creating a duplicate node.
static void test_set_existing_key_updates_value(void) {
    HashTable *table = create_table();
    assert(table != NULL);

    // Saving two different values under the same key
    assert(db_set(table, "key", "first_val") == 0);
    assert(db_set(table, "key", "second_val") == 0);

    // The new value is returned. NOTE: This alone does NOT prove there is no duplicate -
    // a duplicate node would be inserted at the head, so GET would still return the new value.
    char *value = db_get(table, "key");
    assert(value != NULL);
    assert(strcmp(value, "second_val") == 0);

    // THE REAL CHECK: Delete the key ONCE. If a duplicate node existed, only the new node would be
    // removed, and GET would "resurrect" the old value instead of returning NULL.
    assert(db_delete(table, "key") == 1);
    assert(db_get(table, "key") == NULL);

    db_destroy(table);
}

// TEST: db_delete returns 0 when there is nothing to delete, without crashing or damaging other keys.
static void test_delete_missing_key_returns_zero(void) {
    HashTable *table = create_table();
    assert(table != NULL);

    // CASE 1: Empty table - the bucket is NULL, so the loop in db_delete never starts.
    assert(db_delete(table, "key") == 0);

    // CASE 2: "ob" collides with "a" (same bucket) - db_delete walks the whole list,
    // finds no match and reaches the end. The existing key must stay intact.
    assert(db_set(table, "a", "first_val") == 0);
    assert(db_delete(table, "ob") == 0);

    char *value = db_get(table, "a");
    assert(value != NULL);
    assert(strcmp(value, "first_val") == 0);

    // CASE 3: Double delete - the first call removes the key, the second finds nothing.
    // IDEMPOTENCY: In the job queue, a worker may send the same ack twice (e.g. after a reconnect),
    // so a repeated delete must be safe and simply report "not found".
    assert(db_delete(table, "a") == 1);
    assert(db_delete(table, "a") == 0);

    db_destroy(table);
}

// TEST: db_set stores its own copies of the key and value (strdup), not pointers to the caller's memory.
// In the server, key and value point INTO the client's read buffer, which is overwritten by the next message.
static void test_set_stores_deep_copy(void) {
    HashTable *table = create_table();
    assert(table != NULL);

    // STACK ALLOCATION: Modifiable char arrays, initialized with a copy of the string literal.
    // NOTE: 'char *key_buf = "name"' would point to READ-ONLY memory, and writing to it would crash.
    char key_buf[] = "name";
    char value_buf[] = "hello";

    // Arrays decay into pointers to their first char when passed to a function.
    assert(db_set(table, key_buf, value_buf) == 0);

    // VALUE COPY: Overwrite the caller's value buffer ("hello" -> "Xello").
    // If the table stored a pointer to value_buf, GET would now return "Xello".
    value_buf[0] = 'X';
    char *value = db_get(table, "name");
    assert(value != NULL);
    assert(strcmp(value, "hello") == 0);

    // KEY COPY: Overwrite the caller's key buffer ("name" -> "Xame").
    // If the table stored a pointer to key_buf, the stored key would become "Xame":
    // looking up "name" would reach the right bucket, but strcmp would fail and the key would "disappear".
    key_buf[0] = 'X';
    assert(db_get(table, "name") != NULL);
    assert(db_get(table, "Xame") == NULL);

    db_destroy(table);
}

static void test_destroy_null_is_safe(void) {
    db_destroy(NULL);
}

// TEST REGISTRY: Pairs each test's name with a pointer to its function.
// FUNCTION POINTER: 'void (*fn)(void)' is a pointer to a function that takes no arguments and
// returns nothing - similar to a Runnable or a method reference (MyTests::someTest) in Java.
typedef struct {
    const char *name;
    void (*fn)(void);
} TestCase;

static const TestCase tests[] = {
    {"set_and_get",                test_set_and_get},
    {"get_missing_key",            test_get_missing_key_returns_null},
    {"delete_with_collisions",     test_delete_with_collisions},
    {"set_existing_key",           test_set_existing_key_updates_value},
    {"delete_missing_key",         test_delete_missing_key_returns_zero},
    {"deep_copy",                  test_set_stores_deep_copy},
    {"destroy_null",                 test_destroy_null_is_safe},
};

// USAGE: ./build/test_database             -> runs all tests
//        ./build/test_database deep_copy   -> runs only the test with that name
// 'argc' is the number of command-line arguments, 'argv' holds them as strings (argv[0] is the program name).
int main(int argc, char *argv[]) {
    size_t count = sizeof(tests) / sizeof(tests[0]);
    const char *only = (argc > 1) ? argv[1] : NULL;
    int ran = 0;

    for (size_t i = 0; i < count; i++) {
        // Skip tests that don't match the requested name (if one was given)
        if (only != NULL && strcmp(only, tests[i].name) != 0) {
            continue;
        }

        // Print the name BEFORE running, so a crash shows which test caused it
        printf("RUN  %s\n", tests[i].name);
        tests[i].fn();
        ran++;
    }

    if (ran == 0) {
        fprintf(stderr, "No test named '%s'\n", only);
        return 1;
    }

    printf("All %d tests passed\n", ran);
    return 0;
}
