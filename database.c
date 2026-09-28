#include "database.h"
#include <stdio.h>

// The djb2 hash function algorithm
unsigned long hash_function(const char *str) {
    unsigned long hash = 5381;
    int c;

    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    return hash % TABLE_SIZE;
}

HashTable* create_table() {
    // HEAP ALLOCATION: We allocate memory for the main table structure. 
    // MEMORY OWNER: The main server process. 
    // It must free this on server shutdown. 
    HashTable *table = (HashTable *)malloc(sizeof(HashTable));
    if (table == NULL) {
        return NULL;
    }

    // HEAP ALLOCATION: We allocate memory for the array of Node pointers. 

    table->buckets = (Node **)malloc(TABLE_SIZE * sizeof(Node *));
    if (table->buckets == NULL) {
        // If the second allocation (for 'buckets') fails, we must free the first
        // one (for 'table') to prevent memory leak. 
        free(table);
        return NULL;
    }

    // Loop to initialize all pointers to NULL. 
    // This prevents reading garbage memory when we later search for keys. 
    for (int i = 0; i < TABLE_SIZE; i++) {
        table->buckets[i] = NULL;
    }
    return table;
}

// FUNCTION: find_node(table,key)
// Searches the bucket's linked list for a node with matching key. 
// Returns a pointer to existing node, or NULL if the key is not found. 
// 'static' makes this function private to database.c (like 'private' in Java)
static Node* find_node(HashTable *table, const char *key) {
    unsigned long index = hash_function(key);

    // POINTER: 'current' is a traversal pointer on the stack. No new memory is allocated. 
    Node *current = table->buckets[index];

    while (current != NULL) {
        if (strcmp(current->key, key) == 0) {
            // POINTER: return the node itself (not just its value), so callers can modify it
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// FUNCTION: db_set(table,key,value)
// Stores a key-value pair in the hash table. 
// Resolves collisions by adding to the front of the linked list. 
void db_set(HashTable *table, const char *key, const char *value) {
    // UPDATE PATH: If the key already exists, replace its value instead of creating a duplicate node.
    // ORDER: Search BEFORE computing the index - find_node hashes internally, and the update path doesn't need 'index'.
    Node *existing = find_node(table,key);
    if (existing != NULL) {
        // HEAP ALLOCATION: Allocate and copy new value BEFORE freeing the old one.
        // If the allocation fails, the old value stays intact and the table remains valid.
        // ALLOCATE BEFORE FREE: If free() came first and strdup() failed, the node would point to
        // freed memory (dangling pointer) and the next GET would read garbage or crash.
        char *new_value = strdup(value);
        if (new_value == NULL) {
            perror("Failed to allocate value");
            return;
        }

        // HEAP DEALLOCATION: The old value string is no longer needed. Without free() it would leak forever.
        free(existing->value);

        // MEMORY OWNER: The node now owns the new string.
        existing->value = new_value;

        // GUARD CLAUSE: Early return so we don't fall through to the insert path and create a duplicate node.
        return;
    }

    // Determine the bucket index using our hash function
    unsigned long index = hash_function(key);

    // POINTER: 'newNode' points to dynamically allocated memory. 
    // HEAP ALLOCATION: We allocate space for the Node struct. 
    // MEMORY OWNER: The HashTable becomes the owner of this memory. 
    Node *newNode = (Node *)malloc(sizeof(Node));
    if ( newNode == NULL) {
        perror("Failed to allocate node");
        return;
    }

    // HEAP ALLOCATION: We must explicitly allocate new memory for the key string and copy its contents - DEEP COPY
    // If we just pointed to the existing key, it would be overwritten by the next client message in the buffer. 
    // MEMORY OWNER: The table owns this string memory. 
    newNode->key = (char *)malloc(strlen(key) + 1);
    strcpy(newNode->key, key);

    // HEAP ALLOCATION: same rationale for the value string. 
    // MEMORY OWNER: the table as well
    newNode->value = (char *)malloc(strlen(value) + 1); // (char *) - Type casting
    strcpy(newNode->value, value);

    // LINKED LIST INSERTION: Point the new node to the current head of the list at this bucket, then update the bucket to point to our new node (insert at head). 
    newNode->next = table->buckets[index];
    table->buckets[index] = newNode;

}

// FUNCTION: searches for a key and returns its associated value. 
char* db_get(HashTable *table, const char *key) {
    Node *node = find_node(table,key);
    
    // POINTER: return a pointer to the existing value string, or NULL if the key was not found. 
    return (node != NULL) ? node->value : NULL;
}
