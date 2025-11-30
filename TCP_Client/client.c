// client.c
// Compile: gcc client.c -o client
// Usage: ./client <server_ip> <port>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define BUF_SIZE 4096

ssize_t send_all(int sock, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t s = send(sock, buf + total, len - total, 0);
        if (s <= 0) return s;
        total += s;
    }
    return total;
}

void trim_newline(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1]=='\n' || s[n-1]=='\r')) { s[n-1]='\0'; n--; }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }
    char *server_ip = argv[1];
    int port = atoi(argv[2]);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(server_ip);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); return 1;
    }

    char username[128], password[128];
    int choice;
    int logged_in = 0;

    while (1) {
        printf("\n=== MENU ===\n");
        if (!logged_in) {
            printf("1. REGISTER\n2. LOGIN\n3. QUIT\nChoice: ");
        } else {
            printf("1. LOGOUT\n2. QUIT\nChoice: ");
        }
        if (scanf("%d", &choice) != 1) { getchar(); continue; }
        getchar(); // consume newline

        if (!logged_in) {
            if (choice == 3) break;

            printf("Username: "); fgets(username, sizeof(username), stdin); trim_newline(username);
            printf("Password: "); fgets(password, sizeof(password), stdin); trim_newline(password);

            if (strlen(username)==0 || strlen(password)==0) {
                printf("[CLIENT] Username/password cannot be empty!\n");
                continue;
            }

            char line[BUF_SIZE];
            if (choice==1) snprintf(line, sizeof(line), "REGISTER %s %s\r\n", username, password);
            else if (choice==2) snprintf(line, sizeof(line), "LOGIN %s %s\r\n", username, password);
            else { printf("Invalid choice\n"); continue; }

            send_all(sock, line, strlen(line));

            char buf[BUF_SIZE];
            ssize_t n = recv(sock, buf, sizeof(buf)-1, 0);
            if (n <= 0) { printf("Disconnected from server\n"); break; }
            buf[n] = '\0';
            printf("Server response: %s", buf);

            if (choice==2 && strstr(buf, "LOGIN_OK")) logged_in = 1;

        } else { // logged_in menu
            if (choice==1) {
                send_all(sock, "LOGOUT\r\n", 8);
                char buf[BUF_SIZE];
                ssize_t n = recv(sock, buf, sizeof(buf)-1, 0);
                if (n>0) { buf[n]='\0'; printf("Server response: %s", buf); }
                logged_in = 0;
            } else if (choice==2) break;
            else printf("Invalid choice\n");
        }
    }

    close(sock);
    printf("Client exited.\n");
    return 0;
}

