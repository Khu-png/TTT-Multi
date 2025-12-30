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
volatile int current_match_id = -1;
volatile int in_game = 0;
volatile int game_over = 0;

struct move {
    int row, col, player; // 0: self, 1: opponent
};

struct move game_log[9];
int move_count = 0;

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
        if (strstr(buf, "LOGOUT_OK")) {
            logged_in = 0;
            current_match_id = -1;
            move_count = 0;
            in_game = 0;
            game_over = 0;
        }
        if (strstr(buf, "MATCH_RESULT")) {
            game_over = 1;
            in_game = 0;
        }
        if (strstr(buf, "MATCH_STOPPED")) {
            current_match_id = -1;
            move_count = 0;
            in_game = 0;
            game_over = 0;
        }
        if (strstr(buf, "OPPONENT_MOVE")) {
            int r, c;
            if (sscanf(buf, "OPPONENT_MOVE row %d col %d", &r, &c) == 2) {
                if (move_count < 9) {
                    game_log[move_count].row = r;
                    game_log[move_count].col = c;
                    game_log[move_count].player = 1;
                    move_count++;
                }
            }
        }
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
            if (!in_game && !game_over) {
                printf("1. LOGOUT\n2. MAKE MOVE\n3. STOP\n4. QUIT\nChoice: ");
            } else if (in_game) {
                printf("Make move (column row), 3 to stop: ");
            } else if (game_over) {
                printf("Game over! L to view log, Q to quit to menu: ");
            }
        }
        if (!logged_in || (!in_game && !game_over)) {
            if (scanf("%d", &choice) != 1) { getchar(); continue; }
            getchar(); // consume newline
        }

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
            if (!in_game && !game_over) {
                if (choice==1) {
                    send_all(sock, "LOGOUT\r\n", 8);
                    // recv_thread sẽ in LOGOUT_OK và cập nhật logged_in
                    int waited = 0;
                    while (logged_in && waited < 2000) { usleep(100000); waited += 100; }
                } else if (choice==2) {
                    // MAKE MOVE
                    if (current_match_id == -1) {
                        printf("Match id: "); 
                        if (scanf("%d", &current_match_id) != 1) { 
                            getchar(); 
                            printf("Invalid match id\n"); 
                            current_match_id = -1;
                            continue; 
                        }
                        getchar(); // consume newline
                    }
                    in_game = 1;
                } else if (choice==3) {
                    // STOP
                    if (current_match_id == -1) {
                        printf("Match id: "); 
                        if (scanf("%d", &current_match_id) != 1) { 
                            getchar(); 
                            printf("Invalid match id\n"); 
                            current_match_id = -1;
                            continue; 
                        }
                        getchar();
                    }
                    char line[BUF_SIZE];
                    snprintf(line, sizeof(line), "STOP match %d\r\n", current_match_id);
                    send_all(sock, line, strlen(line));
                    current_match_id = -1;
                    move_count = 0;
                    in_game = 0;
                } else if (choice==4) break;
                else printf("Invalid choice\n");
            } else if (in_game) {
                char input[32];
                fgets(input, sizeof(input), stdin);
                trim_newline(input);
                if (strcmp(input, "3") == 0) {
                    char line[BUF_SIZE];
                    snprintf(line, sizeof(line), "STOP match %d\r\n", current_match_id);
                    send_all(sock, line, strlen(line));
                    current_match_id = -1;
                    move_count = 0;
                    in_game = 0;
                } else {
                    int c, r;
                    if (sscanf(input, "%d %d", &c, &r) == 2) {
                        char line[BUF_SIZE];
                        snprintf(line, sizeof(line), "MOVE match %d row %d col %d\r\n", current_match_id, r, c);
                        send_all(sock, line, strlen(line));
                        // lưu move của mình
                        if (move_count < 9) {
                            game_log[move_count].row = r;
                            game_log[move_count].col = c;
                            game_log[move_count].player = 0;
                            move_count++;
                        }
                    } else {
                        printf("Invalid input. Use 'column row' or '3' to stop.\n");
                    }
                }
            } else if (game_over) {
                char input[32];
                fgets(input, sizeof(input), stdin);
                trim_newline(input);
                if (strcmp(input, "L") == 0 || strcmp(input, "l") == 0) {
                    printf("Game Log:\n");
                    for (int i = 0; i < move_count; i++) {
                        printf("Player %d: row %d col %d\n", game_log[i].player, game_log[i].row, game_log[i].col);
                    }
                } else if (strcmp(input, "Q") == 0 || strcmp(input, "q") == 0) {
                    current_match_id = -1;
                    move_count = 0;
                    game_over = 0;
                } else {
                    printf("Invalid input. Press L to view log or Q to quit to menu.\n");
                }
            }
        }
    }

    close(sock);
    printf("Client exited.\n");
    return 0;
}
