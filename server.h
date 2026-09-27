#ifndef SERVER_H
#define SERVER_H
#include "database.h"

int start_server(int port);
void handle_client(int client_socket, HashTable *db);

#endif