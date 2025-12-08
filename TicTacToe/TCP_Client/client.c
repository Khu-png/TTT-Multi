#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <ctype.h>

#define BUF_SIZE 4096

int sock;
int in_match = 0;
int awaiting_response = 0;
char current_challenger[128] = {0};
pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

void print_menu_locked() {
    printf("\n========= MENU =========\n");
    printf("1) Register\n2) Login\n3) Logout\n4) List players\n5) Challenge a player\n6) Exit\n");
    printf("Select: ");
    fflush(stdout);
}

void *recv_thread(void *arg) {
    char buf[BUF_SIZE];
    while (1) {
        ssize_t n = recv(sock, buf, sizeof(buf)-1, 0);
        if (n <= 0) break;
        buf[n] = '\0';

        // Handle challenge request
        if (strncmp(buf, "131 CHALLENGE_REQUEST", 21) == 0) {
            if (in_match) {
                // send user_busy automatically
                char resp[256];
                char from[128];
                if (sscanf(buf, "131 CHALLENGE_REQUEST from %127s", from) == 1) {
                    snprintf(resp, sizeof(resp), "CHALLENGE_FAIL user_busy\r\n");
                    send(sock, resp, strlen(resp), 0);
                }
                continue;
            }

            char from[128];
            if (sscanf(buf, "131 CHALLENGE_REQUEST from %127s", from) == 1) {
                awaiting_response = 1;
                strncpy(current_challenger, from, sizeof(current_challenger)-1);
                pthread_mutex_lock(&print_mutex);
                printf("\nYou were challenged by %s. Accept? (y/n): ", from);
                fflush(stdout);
                pthread_mutex_unlock(&print_mutex);
            }
            continue;
        }

        // Challenge accepted
        if (strncmp(buf, "132 CHALLENGE_ACCEPTED", 21) == 0) {
            in_match = 1;
            printf("\nMatch started with %s!\n", current_challenger);
            fflush(stdout);
            continue;
        }

        // Challenge declined
        if (strncmp(buf, "133 CHALLENGE_DECLINED", 21) == 0) {
            printf("\nChallenge from %s declined.\n", current_challenger);
            awaiting_response = 0;
            memset(current_challenger, 0, sizeof(current_challenger));
            fflush(stdout);
            continue;
        }

        // Normal messages
        pthread_mutex_lock(&print_mutex);
        printf("%s", buf);
        fflush(stdout);
        // After printing server message, if not in match and not prompting,
        // reprint the menu prompt so user sees Select again.
        if (!in_match && !awaiting_response) {
            print_menu_locked();
        }
        pthread_mutex_unlock(&print_mutex);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 3) { fprintf(stderr,"Usage: %s <ip> <port>\n", argv[0]); return 1; }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(argv[2]));
    inet_pton(AF_INET, argv[1], &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("connect"); return 1; }

    pthread_t th;
    pthread_create(&th, NULL, recv_thread, NULL);
    pthread_detach(th);

    char line[BUF_SIZE];
    while (1) {
        // If waiting for challenge response, read a line and validate y/n
        if (awaiting_response) {
            char respbuf[64];
            while (awaiting_response) {
                if (!fgets(respbuf, sizeof(respbuf), stdin)) { awaiting_response = 0; break; }
                // trim
                respbuf[strcspn(respbuf, "\r\n")] = 0;
                if (respbuf[0] == 'y' || respbuf[0] == 'Y') {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "CHALLENGE_ACCEPT from %s\r\n", current_challenger);
                    send(sock, msg, strlen(msg), 0);
                    awaiting_response = 0;
                    break;
                } else if (respbuf[0] == 'n' || respbuf[0] == 'N') {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "CHALLENGE_DECLINE from %s\r\n", current_challenger);
                    send(sock, msg, strlen(msg), 0);
                    awaiting_response = 0;
                    break;
                } else {
                    pthread_mutex_lock(&print_mutex);
                    printf("Invalid input. Accept? (y/n): "); fflush(stdout);
                    pthread_mutex_unlock(&print_mutex);
                    continue;
                }
            }
            memset(current_challenger, 0, sizeof(current_challenger));
            printf("\n");
            continue;
        }

        // Don't show menu if in match
        if (in_match) {
            sleep(1);
            continue;
        }

        pthread_mutex_lock(&print_mutex);
        print_menu_locked();
        pthread_mutex_unlock(&print_mutex);

        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\r\n")] = 0; // trim newline

        // Handle menu choices locally for interactive options
        if (strcmp(line, "1") == 0) {
            char user[128], pass[128];
            printf("Enter username: "); fflush(stdout);
            if (!fgets(user, sizeof(user), stdin)) break;
            user[strcspn(user, "\r\n")] = 0;
            printf("Enter password: "); fflush(stdout);
            if (!fgets(pass, sizeof(pass), stdin)) break;
            pass[strcspn(pass, "\r\n")] = 0;
            char msg[256]; snprintf(msg, sizeof(msg), "REGISTER %s %s\r\n", user, pass);
            send(sock, msg, strlen(msg), 0);
            continue;
        }
        if (strcmp(line, "2") == 0) {
            char user[128], pass[128];
            printf("Enter username: "); fflush(stdout);
            if (!fgets(user, sizeof(user), stdin)) break;
            user[strcspn(user, "\r\n")] = 0;
            printf("Enter password: "); fflush(stdout);
            if (!fgets(pass, sizeof(pass), stdin)) break;
            pass[strcspn(pass, "\r\n")] = 0;
            char msg[256]; snprintf(msg, sizeof(msg), "LOGIN %s %s\r\n", user, pass);
            send(sock, msg, strlen(msg), 0);
            continue;
        }
        if (strcmp(line, "3") == 0) { send(sock, "3\r\n", 3, 0); continue; }
        if (strcmp(line, "4") == 0) { send(sock, "4\r\n", 3, 0); continue; }
        if (strcmp(line, "5") == 0) {
            char target[128];
            printf("Enter username to challenge: "); fflush(stdout);
            if (!fgets(target, sizeof(target), stdin)) break;
            target[strcspn(target, "\r\n")] = 0;
            char msg[256]; snprintf(msg, sizeof(msg), "CHALLENGE %s\r\n", target);
            send(sock, msg, strlen(msg), 0);
            continue;
        }
        if (strcmp(line, "6") == 0) { break; }

        // Fallback: send raw line
        send(sock, line, strlen(line), 0);
    }

    close(sock);
    return 0;
}
