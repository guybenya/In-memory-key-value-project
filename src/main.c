#include "server.h"
#include "database.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>


int main() {
    int port = 8080;

    // call our custom function to setup the server. This function handles the socket, bind and listen SYSTEM CALLS internally. It returns the main server file descriptor. 
    int server_fd = start_server(port);

    // Declare a structure to hold the IP address and port of the client that connects to us. The OS will fill this structure later.
    struct sockaddr_in client_address;

    // Store the size of the structure.
    // The OS needs to know exactly how much memory it is allowed to write into.
    socklen_t client_addr_len = sizeof(client_address);

    // HEAP ALLOCATION: Create the global database instance. 
    // POINTER: 'db' holds the memory address of the hash table. 
    // MEMORY OWNER: main() function. It owns this memory because the database must persist for the entire lifespan of the server. 
    HashTable *db = create_table();

    if (db == NULL) {
        perror("Failed to create databse");
        return 1;
    }

    printf("Waiting for connections... \n");

    // Infinite loop to keep the server running and continuously waiting for new clients. 

    while(1) {

        // SYSTEM CALL: accept()
        // This blocks (pauses) the program until 
        // a new client actually connects.
        // It returns a BRAND NEW file descriptor 
        // dedicated ONLY to this specific client.
        int client_fd = accept(
            server_fd,
            (struct sockaddr *)&client_address, &client_addr_len
        );

        // If the SYSTEM CALL fails, it return -1. 
        if(client_fd < 0) {
            // Print the error form the OS. 
            perror("Accept failed");

            // Do not crash the entire server. 
            // Skip to the next loop iteration to wait for the next client. 
            continue;
        }

        printf("Client connected successfully!\n");

        handle_client(client_fd, db);

        // SYSTEM CALL: close()
        // We must ask the OS to close the socket and free the resources for this connection. 
        // If we forget this SYSTEM CALL, the server will leak memory and eventually crash.
        
        close(client_fd);

    }


    return 0;
}