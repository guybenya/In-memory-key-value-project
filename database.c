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

// FUNCTION: db_set(table,key,value)
// Stores a key-value pair in the hash table. 
// Resolves collisions by adding to the front of the linked list. 
void db_set(HashTable *table, const char *key, const char *value) {
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

    // HEAP ALLOCATION: same rationale for the valye string. 
    // MEMORY OWNER: the table as well
    newNode->value = (char *)malloc(strlen(value) + 1); // (char *) - Type casting
    strcpy(newNode->value, value);

    // LINKED LIST INSERTION: Point the new node to the curren head of the list at this bucket, the update the bucket to point to our new node (insert at head). 
    newNode->next = table->buckets[index];
    table->buckets[index] = newNode;

}

// FUNCTION: Searches for a key and returns its associated value. 
char* db_get (HashTable *table, const char *key) {
    unsigned long index = hash_function(key);

    // POINTER: 'current' is a traversal pointer sitting on the stack. It points to the existing Nodes inside the heap and read them. It does NOT allocate any new memory. 
    Node *current = table->buckets[index];

    // Traverse the linked list at this bucket in case of collisions
    while (current != NULL) {
        // Use strcmp to find the exact matching key
        if (strcmp(current->key, key) == 0) {
            // POINTER: return a pointer to the exsiting value string. 
            return current->value;
        }
        current = current->next;
    }
    // Return NULL if the key was not found in the list
    return NULL;
}