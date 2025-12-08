CC = gcc
CFLAGS = -Wall -pthread

# --- TARGETS ---
all: server client

# --- SERVER BUILD ---
server: TCP_Server/server.c
	$(CC) $(CFLAGS) TCP_Server/server.c -o TCP_Server/server

# --- CLIENT BUILD ---
client: TCP_Client/client.c
	$(CC) $(CFLAGS) TCP_Client/client.c -o TCP_Client/client

# --- CLEAN ---
clean:
	rm -f TCP_Server/server
	rm -f TCP_Client/client
