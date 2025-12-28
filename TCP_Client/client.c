#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

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

// Global login flag updated by receiver thread
volatile int logged_in = 0;

// Receiver thread: đọc mọi thông điệp từ server và in ra
void *recv_thread(void *arg) {
    int sock = *(int*)arg;
    free(arg);
    char buf[BUF_SIZE];
    while (1) {
        ssize_t n = recv(sock, buf, sizeof(buf)-1, 0);
        if (n <= 0) {
            printf("\n[CLIENT] Disconnected from server\n");
            exit(0);
        }
        buf[n] = '\0';
        // cập nhật trạng thái đăng nhập nếu server trả về LOGIN_OK / LOGOUT_OK
        if (strstr(buf, "LOGIN_OK")) logged_in = 1;
        if (strstr(buf, "LOGOUT_OK")) logged_in = 0;
        // In nguyên thông điệp nhận được
        printf("\n[SERVER] %s", buf);
        fflush(stdout);
    }
    return NULL;
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

    // start receiver thread
    int *psock = malloc(sizeof(int));
    if (!psock) { perror("malloc"); close(sock); return 1; }
    *psock = sock;
    pthread_t rth;
    pthread_create(&rth, NULL, recv_thread, psock);
    pthread_detach(rth);

    char username[128], password[128];
    int choice;

    while (1) {
        printf("\n=== MENU ===\n");
        if (!logged_in) {
            printf("1. REGISTER\n2. LOGIN\n3. QUIT\nChoice: ");
        } else {
            printf("1. LOGOUT\n2. MAKE MOVE\n3. STOP\n4. QUIT\nChoice: ");
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

            // Chờ ngắn dò kết quả (recv_thread sẽ in kết quả và cập nhật logged_in)
            int waited = 0;
            while (choice==2 && !logged_in && waited < 2000) { usleep(100000); waited += 100; }
            // Nếu đăng nhập không thành công, thông điệp lỗi đã được in bởi recv_thread
        } else { // logged_in menu
            if (choice==1) {
                send_all(sock, "LOGOUT\r\n", 8);
                // recv_thread sẽ in LOGOUT_OK và cập nhật logged_in
                int waited = 0;
                while (logged_in && waited < 2000) { usleep(100000); waited += 100; }
            } else if (choice==2) {
                int match_id, r, c;
                printf("Match id: "); if (scanf("%d", &match_id)!=1) { getchar(); printf("Invalid\n"); continue; } 
                printf("Row (0-based): "); if (scanf("%d", &r)!=1) { getchar(); printf("Invalid\n"); continue; }
                printf("Col (0-based): "); if (scanf("%d", &c)!=1) { getchar(); printf("Invalid\n"); continue; }
                getchar(); 
                char line[BUF_SIZE];
                snprintf(line, sizeof(line), "MOVE match %d row %d col %d\r\n", match_id, r, c);
                send_all(sock, line, strlen(line));
            } else if (choice==3) {
                // STOP - stop/cancel match
                int match_id;
                printf("Match id: "); if (scanf("%d", &match_id)!=1) { getchar(); printf("Invalid\n"); continue; }
                getchar();
                char line[BUF_SIZE];
                snprintf(line, sizeof(line), "STOP match %d\r\n", match_id);
                send_all(sock, line, strlen(line));
            } else if (choice==4) break;
            else printf("Invalid choice\n");
        }
    }

    close(sock);
    printf("Client exited.\n");
    return 0;
}
