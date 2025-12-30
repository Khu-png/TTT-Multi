#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <ctype.h>

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
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) {
        s[n-1] = '\0';
        n--;
    }
}

void read_line(char *buf, size_t size) {
    if (fgets(buf, size, stdin)) {
        trim_newline(buf);
    } else {
        buf[0] = '\0';
    }
}

/* ===== Global states ===== */
volatile int logged_in = 0;
volatile int current_match_id = -1;
volatile int in_game = 0;
volatile int game_over = 0;

struct move {
    int row, col, player; // 0: self, 1: opponent
};

struct move game_log[9];
int move_count = 0;

/* ===== Receiver thread ===== */
void *recv_thread(void *arg) {
    int sock = *(int*)arg;
    free(arg);

    char buf[BUF_SIZE];
    while (1) {
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            printf("\n[CLIENT] Disconnected from server\n");
            exit(0);
        }
        buf[n] = '\0';

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
                    game_log[move_count++] = (struct move){r, c, 1};
                }
            }
        }

        printf("\n[SERVER] %s", buf);
        fflush(stdout);
    }
    return NULL;
}

/* ===== Main ===== */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(argv[2]));
    addr.sin_addr.s_addr = inet_addr(argv[1]);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    int *psock = malloc(sizeof(int));
    *psock = sock;
    pthread_t tid;
    pthread_create(&tid, NULL, recv_thread, psock);
    pthread_detach(tid);

    char input[128];

    while (1) {
        printf("\n=== MENU ===\n");

        if (!logged_in) {
            printf("1. REGISTER\n2. LOGIN\n3. QUIT\nChoice: ");
            read_line(input, sizeof(input));

            if (strcmp(input, "3") == 0) break;

            if (strcmp(input, "1") != 0 && strcmp(input, "2") != 0) {
                printf("Invalid choice\n");
                continue;
            }

            char user[64], pass[64];
            printf("Username: "); read_line(user, sizeof(user));
            printf("Password: "); read_line(pass, sizeof(pass));

            char line[BUF_SIZE];
            snprintf(line, sizeof(line),
                    "%s %s %s\r\n",
                    strcmp(input, "1") == 0 ? "REGISTER" : "LOGIN",
                    user, pass);
            send_all(sock, line, strlen(line));

            /* ===== WAIT LOGIN RESULT ===== */
            if (strcmp(input, "2") == 0) { // LOGIN
                int waited = 0;
                while (!logged_in && waited < 2000) {
                    usleep(100000); // 100ms
                    waited += 100;
                }
            }       
        }
        else if (!in_game && !game_over) {
            printf("1. LOGOUT\n2. MAKE MOVE\n3. STOP\n4. QUIT\nChoice: ");
            read_line(input, sizeof(input));

            if (strcmp(input, "1") == 0) {
                send_all(sock, "LOGOUT\r\n", 8);
            }
            else if (strcmp(input, "2") == 0) {
                printf("Match id: ");
                read_line(input, sizeof(input));
                current_match_id = atoi(input);
                in_game = 1;
            }
            else if (strcmp(input, "3") == 0) {
                printf("Match id: ");
                read_line(input, sizeof(input));
                current_match_id = atoi(input);

                char line[BUF_SIZE];
                snprintf(line, sizeof(line),
                         "STOP match %d\r\n", current_match_id);
                send_all(sock, line, strlen(line));

                current_match_id = -1;
            }
            else if (strcmp(input, "4") == 0) break;
            else printf("Invalid choice\n");
        }
        else if (in_game) {
            printf("Make move (col row), S to stop: ");
            read_line(input, sizeof(input));
            if (game_over) {
                continue;
            }
            if (strcasecmp(input, "S") == 0) {
                char line[BUF_SIZE];
                snprintf(line, sizeof(line),
                         "STOP match %d\r\n", current_match_id);
                send_all(sock, line, strlen(line));
                current_match_id = -1;
                in_game = 0;
            } else {
                int c, r;
                if (sscanf(input, "%d %d", &c, &r) == 2) {
                    char line[BUF_SIZE];
                    snprintf(line, sizeof(line),
                             "MOVE match %d row %d col %d\r\n",
                             current_match_id, r, c);
                    send_all(sock, line, strlen(line));

                    if (move_count < 9)
                        game_log[move_count++] = (struct move){r, c, 0};
                } else {
                    printf("Invalid input\n");
                }
            }
        }
        else if (game_over) {
            printf("Game over! L to view log, Q to quit to menu: ");
            read_line(input, sizeof(input));

            if (strcasecmp(input, "L") == 0) {
                printf("Game Log:\n");
                for (int i = 0; i < move_count; i++) {
                    printf("Player %d: row %d col %d\n",
                           game_log[i].player,
                           game_log[i].row,
                           game_log[i].col);
                }
            }
            else if (strcasecmp(input, "Q") == 0) {
                game_over = 0;
                move_count = 0;
                current_match_id = -1;
            }
            else {
                printf("Invalid input\n");
            }
        }
    }

    close(sock);
    printf("Client exited.\n");
    return 0;
}
