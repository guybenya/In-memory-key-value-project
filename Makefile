CC = clang
CFLAGS = -Wall -Wextra -g

server: main.c server.c database.c
	$(CC) $(CFLAGS) main.c server.c database.c -o server

clean:
	rm -f server