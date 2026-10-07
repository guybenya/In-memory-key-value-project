#include "server.h"
#include "database.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>


// Establishing the structure
int start_server(int port) {
    // System call to create a new network endpoint (socket).
    // AF_INET specifies the IPv4 address family.
    // SOCK_STREAM specifies that we want a reliable, connection-oriented TCP stream.
    // 0 tells the operating system to choose the default protocol (TCP) for this setup.
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    // The socket system call returns -1 if the operating system could not allocate the resource.
    if (server_fd == -1) {
        // perror prints our custom message followed by the actual error reason from the OS (e.g., "Permission denied").
        perror("Socket creation failed");
        // exit immediately terminates the entire process and returns a failure code to the operating system.
        exit(EXIT_FAILURE);
    }

    // SYSTEM CALL: setsockopt(fd, level, option, value_ptr, value_len) - changes a setting of the socket.
    // SO_REUSEADDR: After the server stops, the OS keeps its old connections in TIME_WAIT for ~30-60 seconds,
    // and bind() would fail with "Address already in use". This option lets us bind to the port immediately.
    // MUST be set BEFORE bind(), because it changes what bind() is allowed to do.
    // 'opt' = 1 means "enable". The value is passed by pointer (void *) because different options use
    // different types - sizeof(opt) tells the OS how many bytes to read from that pointer.
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }

    // declares a specifically designed to hold IPv4 address and port information. 
    struct sockaddr_in address;

    // Sets the address family of the structure to IPv4, so the OS knows how to read the data inside. 
    address.sin_family = AF_INET;

    // "INADDR_ANY" configures the socket to listen on all available network interfaces on this machine (both local - host and external). 
    address.sin_addr.s_addr = INADDR_ANY;

    // htons (Host To Network Short) converts the port number from the local CPU's byte order to the standart network byteorder (Big Endian). 
    address.sin_port = htons(port);

    // System call that formally links (binds) the socket file descriptor to the specific IP and port we configured.
    // We must cast our specific 'sockaddr_in' pointer to the generic 'sockaddr' pointer that the bind function expects.
    // sizeof(address) tells the OS exactly how many bytes of memory to read from our structure.
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    // System call that transitions the socket from an active state to a passive listening state.
    // The number 3 defines the backlog queue length: the maximum number of pending connections the OS will queue before rejecting new ones.
    if (listen(server_fd, 3) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }
    // Prints a confirmation to the terminal so we know the server is successfully running.
    printf("Server listening on port %d... \n" , port);

    // Returns the file descriptor (the integer ID of the socket) so the rest of the program can use it to accept connections.
    return server_fd;
}

void handle_client(int client_fd, HashTable *db) {
    // POINTER: buffer pointsto dynamically allocated memory from the heap. 
    // OWNER: handle_client is the owner and must free this memory at the end. 
    char *buffer = (char *)malloc(1024);

    if (buffer == NULL) {
        perror("Allocation failed");
        return;
    }

    while (1) { // C version for while(true)

        // initializing a block of memory in one opetation - filling the array with zeros
        // in order not to print old leftovers text from the memory.  
        memset(buffer,0,1024);
        
        // SYSTEM CALL: read()
        // 1023 is the last index in the array - prevents reading from an undifined cell.
        ssize_t bytes_read = read(client_fd,buffer,1023);
        
        // If the client disconnects or an error occurs, we break out the loop. 
        if (bytes_read <= 0) {
            break;
        }

        // STACK - POINTER: tracks the current parsing position for the strtok_r.
        char *saveptr;

        // FUNCTION: strtok_r(string, delimiters, state)
        // Splits the string into words (tokens). 
        // Modifies the original buffer by inserting null-terminators ('\0') at the
        // end of each found word.
        // HEAP - POINTER: points directly to the first word INSIDE the existing heap buffer
        // There is no new memory allocation here.
        char *command = strtok_r(buffer, "\r\n ", &saveptr);

        if (command != NULL) {
            // FUNCTION: strcmp(string1, string2)
            // Compares two strings character by character.
            // It is the equivalent of 'equals()' in Java.
            // Returns 0 if they are exactly identical.
            if (strcmp(command, "SET") == 0) {
                // Passing NULL to strtok_r tells it to
                // continue parsing from where it left off.
                // POINTERS: key and value point to the
                // next words inside the same heap buffer.
                char *key = strtok_r (NULL, "\r\n ", &saveptr);
                char *value = strtok_r (NULL, "\r\n ", &saveptr);

                if (key != NULL && value != NULL) {
                    if (db_set(db, key,value) == 0) {
                        // POINTER: response points to a static string located in Read-Only memory. 
                        char *response = "OK - Saved to database\n";

                        // SYSTEM CALL: write(fd, buffer, count)
                        // Sends exactly strlen(response) bytes
                        // back to the client over the network.
                        write (client_fd, response, strlen(response));
                    }
                    else {
                        char *error = "ERROR - Failed to save \n";
                        write(client_fd,error,strlen(error));
                    }
                    
                }
                else {
                    char *error = "ERROR - Usage: SET <key> <value> \n";
                    write (client_fd, error, strlen(error));

                }
            }
            else if (strcmp(command, "GET") == 0) {

                char *key = strtok_r (NULL, "\r\n " , &saveptr);

                if (key != NULL) {
                    // RETRIVE FROM DATABASE
                    // POINTER: 'result' points to the existing string inside the database's heap memory. 
                    char *result = db_get(db, key);

                    if (result != NULL) {
                        // STACK ALLOCATION: response_buf is a local array. Memory is automatically reclaimded by the systme when this 'if' block ends. No need to use free().
                        char response_buf[1024];
                        
                        // FUNCTION: snprintf securely formats strings. It combines "VALUE: " with the retriving string. 
                        snprintf(response_buf, sizeof(response_buf), "VALUE: %s\n", result);

                        write(client_fd, response_buf, strlen(response_buf));
                    }
                    else {
                        char *error = "ERROR - Key not found\n";
                        write(client_fd, error, strlen(error));
                    }
                }
                else {
                    char *error = "ERROR - Usage: GET <key> \n";
                    write(client_fd, error, strlen(error));
                }
            }
            else if (strcmp(command, "DEL") == 0) {
                char *key = strtok_r (NULL, "\r\n " , &saveptr);
                if (key != NULL) {
                    if (db_delete(db,key) == 1) {
                        char *response = "OK - Deleted\n";
                        write(client_fd,response,strlen(response));
                    }
                    else {
                        char *error = "ERROR - Key not found\n";
                        write(client_fd, error, strlen(error));                        
                    }
                }
                else {
                        char *error = "ERROR - Usage: DEL <key>\n";
                        write(client_fd, error, strlen(error));
                }
            }
            else {
                char *error = "ERROR - Unknown command\n";
                write(client_fd,error,strlen(error));
            }
        }
    }
    // FUNCTION: free(pointer)
    // HEAP DEALLOCATION: Releases the memory 
    // block back to the Operating System.
    // Since Java has a Garbage Collector, you 
    // never do this there, but in C it is 
    // mandatory to prevent memory leaks.
    free(buffer);
}


