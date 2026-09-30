#ifndef DATABASE_H
#define DATABASE_H

#include <stdlib.h>
#include <string.h>

#define TABLE_SIZE 1000

// Struct representing a single key-value pair
typedef struct Node {
    // POINTER: 'key' points to a dynamically allocated string on the heap. 
    char *key;

    // POINTER: 'value' points to a dynamically allocated string on the heap.
    char *value;

    // POINTER: 'next' points to the next node in the linked list (for collisions). 
    struct Node *next;

} Node;

// Struct representing the hash table
typedef struct HashTable {
    // POINTER: 'buckets' is an array of pointers. 
    // Each pointer points to a Node on the heap. 
    Node **buckets;
} HashTable;

HashTable* create_table();
unsigned long hash_function(const char *str);

// Frees the entire table: every node, its key and value strings, the buckets array and the table itself.
// Safe to call with NULL.
void db_destroy(HashTable *table);

// Returns 0 on success, -1 on allocation failure.
int db_set(HashTable *table, const char *key, const char *value);

// Returns a pointer to the value, or NULL if the key was not found.
char* db_get(HashTable *table, const char *key);

// Removes a key and frees its memory. Returns 1 if the key was deleted, 0 if it was not found.
int db_delete(HashTable *table, const char *key);

#endif
