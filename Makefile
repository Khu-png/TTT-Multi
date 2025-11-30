# Makefile TCP Server/Client nâng cấp

CC = gcc
CFLAGS = -Wall -Wextra -pthread

# Thư mục
SERVER_DIR = TCP_Server
CLIENT_DIR = TCP_Client

# Targets
all: server client

# Build server
server: $(SERVER_DIR)/server.c
	$(CC) $(CFLAGS) $(SERVER_DIR)/server.c -o $(SERVER_DIR)/server

# Build client
client: $(CLIENT_DIR)/client.c
	$(CC) $(CFLAGS) $(CLIENT_DIR)/client.c -o $(CLIENT_DIR)/client

# Run server (mặc định port 8080)
run_server: server
	@echo "Starting server on port 8080..."
	$(SERVER_DIR)/server 8080

# Run client (mặc định localhost:8080)
run_client: client
	@echo "Starting client connecting to 127.0.0.1:8080..."
	$(CLIENT_DIR)/client 127.0.0.1 8080

# Xóa file thực thi
clean:
	rm -f $(SERVER_DIR)/server
	rm -f $(CLIENT_DIR)/client
