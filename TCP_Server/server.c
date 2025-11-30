// server.c
// Compile: gcc server.c -o server -lpthread
// Usage: ./server <port>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define BACKLOG 10
#define BUF_SIZE 4096
#define USERS_FILE "users.txt"

// Status codes
#define STR_LOGIN_OK "110 LOGIN_OK\r\n"
#define STR_REGISTER_OK "120 REGISTER_OK\r\n"
#define STR_LOGIN_FAIL_USERNAME "220 LOGIN_FAIL wrong_username\r\n"
#define STR_LOGIN_FAIL_PASSWORD "220 LOGIN_FAIL wrong_password\r\n"
#define STR_REGISTER_FAIL_EXISTS "221 REGISTER_FAIL user_exists\r\n"
#define STR_REGISTER_FAIL_EMPTY "222 REGISTER_FAIL empty_field\r\n"
#define STR_LOGOUT_OK "230 LOGOUT_OK\r\n"
#define STR_SERVER_ERROR "500 SERVER_ERROR\r\n"

pthread_mutex_t users_mutex = PTHREAD_MUTEX_INITIALIZER;

// Trim CRLF
static void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1]=='\n' || s[n-1]=='\r')) { s[n-1]='\0'; n--; }
}

// Check if username exists
int user_exists(const char *username) {
    FILE *f = fopen(USERS_FILE, "r");
    if (!f) return 0;
    char u[128], p[128];
    int found = 0;
    while (fscanf(f, "%s %s", u, p) != EOF) {
        if (strcmp(u, username) == 0) { found = 1; break; }
    }
    fclose(f);
    return found;
}

// Check user password
int check_user_pass(const char *username, const char *password) {
    FILE *f = fopen(USERS_FILE, "r");
    if (!f) return 0;
    char u[128], p[128];
    int ok = 0;
    while (fscanf(f, "%s %s", u, p) != EOF) {
        if (strcmp(u, username) == 0) {
            if (strcmp(p, password) == 0) ok = 1;
            else ok = -1;
            break;
        }
    }
    fclose(f);
    return ok;
}

// Register user
int register_user(const char *username, const char *password) {
    if (strlen(username)==0 || strlen(password)==0) return -2; // empty
    pthread_mutex_lock(&users_mutex);
    if (user_exists(username)) { pthread_mutex_unlock(&users_mutex); return 0; }
    FILE *f = fopen(USERS_FILE, "a");
    if (!f) { pthread_mutex_unlock(&users_mutex); return -1; }
    fprintf(f, "%s %s\n", username, password);
    fclose(f);
    pthread_mutex_unlock(&users_mutex);
    return 1;
}

// Send all data
ssize_t send_all(int sock, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t s = send(sock, buf + total, len - total, 0);
        if (s <= 0) return s;
        total += s;
    }
    return total;
}

// Send status to client
void send_status(int client_sock, const char *msg) {
    send_all(client_sock, msg, strlen(msg));
}

// Handle a single line from client
void handle_line(int client_sock, const char *line) {
    char cmd[16], u[128], p[128];
    if (sscanf(line, "%15s %127s %127s", cmd, u, p) < 1) {
        send_status(client_sock, STR_SERVER_ERROR);
        return;
    }

    if (strcmp(cmd, "REGISTER") == 0) {
        if (strlen(u)==0 || strlen(p)==0) { send_status(client_sock, STR_REGISTER_FAIL_EMPTY); return; }
        int r = register_user(u, p);
        if (r==1) send_status(client_sock, STR_REGISTER_OK);
        else if (r==0) send_status(client_sock, STR_REGISTER_FAIL_EXISTS);
        else if (r==-2) send_status(client_sock, STR_REGISTER_FAIL_EMPTY);
        else send_status(client_sock, STR_SERVER_ERROR);
        printf("[SERVER] REGISTER %s -> %d\n", u, r);
        return;
    }

    if (strcmp(cmd, "LOGIN") == 0) {
        int r = check_user_pass(u, p);
        if (r==1) send_status(client_sock, STR_LOGIN_OK);
        else if (r==-1) send_status(client_sock, STR_LOGIN_FAIL_PASSWORD);
        else send_status(client_sock, STR_LOGIN_FAIL_USERNAME);
        printf("[SERVER] LOGIN %s -> %d\n", u, r);
        return;
    }

    if (strcmp(cmd, "LOGOUT") == 0) {
        send_status(client_sock, STR_LOGOUT_OK);
        printf("[SERVER] LOGOUT client_sock=%d\n", client_sock);
        return;
    }

    send_status(client_sock, STR_SERVER_ERROR);
}

// Thread to handle client
void *client_thread(void *arg) {
    int client_sock = *(int*)arg;
    free(arg);

    char buf[BUF_SIZE];
    char linebuf[BUF_SIZE];
    size_t linepos = 0;

    while (1) {
        ssize_t n = recv(client_sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (linepos < BUF_SIZE-1) linebuf[linepos++] = buf[i];
            if (linepos >= 2 && linebuf[linepos-2]=='\r' && linebuf[linepos-1]=='\n') {
                linebuf[linepos] = '\0';
                trim_crlf(linebuf);
                if (strlen(linebuf)>0) handle_line(client_sock, linebuf);
                linepos = 0;
            }
        }
    }

    close(client_sock);
    printf("[SERVER] Client disconnected: sock=%d\n", client_sock);
    return NULL;
}

// Main function
int main(int argc, char *argv[]) {
    if (argc<2) { fprintf(stderr,"Usage: %s <port>\n", argv[0]); return 1; }
    int port = atoi(argv[1]);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr))<0) {
        perror("bind"); close(server_fd); return 1;
    }

    if (listen(server_fd, BACKLOG)<0) { perror("listen"); close(server_fd); return 1; }

    printf("[SERVER] Listening on port %d...\n", port);

    while(1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int *client_sock = malloc(sizeof(int));
        if (!client_sock) continue;
        *client_sock = accept(server_fd, (struct sockaddr*)&cli_addr, &cli_len);
        if (*client_sock < 0) { perror("accept"); free(client_sock); continue; }

        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli_addr.sin_addr, ipstr, sizeof(ipstr));
        printf("[SERVER] Client connected: %s:%d\n", ipstr, ntohs(cli_addr.sin_port));

        pthread_t th;
        pthread_create(&th, NULL, client_thread, client_sock);
        pthread_detach(th);
    }

    close(server_fd);
    return 0;
}

