// server.c
// Compile: gcc server.c -o server -lpthread
// Usage: ./server <port>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define BOARD_N 3 // size của bàn 

typedef struct match_t {
    int id;
    int players[2]; // socket fds, 0 = empty
    int board[BOARD_N][BOARD_N]; // 0 empty, 1 player0, 2 player1
    int turn; // 0 or 1 -> index of player whose turn it is
    struct match_t *next;
} match_t;

static match_t *matches = NULL;
static pthread_mutex_t matches_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t users_mutex = PTHREAD_MUTEX_INITIALIZER;

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

static match_t *find_match_locked(int id) {
    match_t *m = matches;
    while (m) {
        if (m->id == id) return m;
        m = m->next;
    }
    return NULL;
}

static match_t *create_match_locked(int id) {
    match_t *m = calloc(1, sizeof(match_t));
    if (!m) return NULL;
    m->id = id;
    m->players[0] = m->players[1] = 0;
    memset(m->board, 0, sizeof(m->board));
    m->turn = 0;
    m->is_finished = 0;
    m->winner = -1;
    m->next = matches;
    matches = m;
    return m;
}

// assign client socket to a match
static int assign_player_to_match(int id, int sock) {
    pthread_mutex_lock(&matches_mutex);
    match_t *m = find_match_locked(id);
    if (!m) m = create_match_locked(id);
    if (!m) { pthread_mutex_unlock(&matches_mutex); return -2; }

    if (m->players[0] == sock) { pthread_mutex_unlock(&matches_mutex); return 0; }
    if (m->players[1] == sock) { pthread_mutex_unlock(&matches_mutex); return 1; }

    if (m->players[0] == 0) { m->players[0] = sock; pthread_mutex_unlock(&matches_mutex); return 0; }
    if (m->players[1] == 0) { m->players[1] = sock; pthread_mutex_unlock(&matches_mutex); return 1; }

    pthread_mutex_unlock(&matches_mutex);
    return -1; // full
}

static void remove_player_from_matches(int sock) {
    pthread_mutex_lock(&matches_mutex);
    match_t **pp = &matches;
    while (*pp) {
        match_t *m = *pp;
        if (m->players[0] == sock) m->players[0] = 0;
        if (m->players[1] == sock) m->players[1] = 0;

        if (m->players[0] == 0 && m->players[1] == 0) {
            *pp = m->next;
            free(m);
            continue;
        }
        pp = &m->next;
    }
    pthread_mutex_unlock(&matches_mutex);
}

// Check if a player has won (3 in a row/col/diag)
static int check_win(match_t *m, int player_idx) {
    int player_mark = player_idx + 1; // 1 or 2
    // Check rows
    for (int i = 0; i < BOARD_N; i++) {
        int count = 0;
        for (int j = 0; j < BOARD_N; j++) {
            if (m->board[i][j] == player_mark) count++;
        }
        if (count == BOARD_N) return 1;
    }
    // Check columns
    for (int j = 0; j < BOARD_N; j++) {
        int count = 0;
        for (int i = 0; i < BOARD_N; i++) {
            if (m->board[i][j] == player_mark) count++;
        }
        if (count == BOARD_N) return 1;
    }
    // Check main diagonal (top-left to bottom-right)
    int count = 0;
    for (int i = 0; i < BOARD_N; i++) {
        if (m->board[i][i] == player_mark) count++;
    }
    if (count == BOARD_N) return 1;
    // Check anti-diagonal (top-right to bottom-left)
    count = 0;
    for (int i = 0; i < BOARD_N; i++) {
        if (m->board[i][BOARD_N-1-i] == player_mark) count++;
    }
    if (count == BOARD_N) return 1;
    return 0;
}

static int process_move(int client_sock, int match_id, int r, int c) {
    char buf[256];
    pthread_mutex_lock(&matches_mutex);

    match_t *m = find_match_locked(match_id);
    if (!m) {
        pthread_mutex_unlock(&matches_mutex);
        send_status(client_sock, "240 MOVE_FAIL not_in_match\r\n");
        return -1;
    }

    int idx = -1;
    if (m->players[0] == client_sock) idx = 0;
    else if (m->players[1] == client_sock) idx = 1;

    if (idx == -1) {
        pthread_mutex_unlock(&matches_mutex);
        send_status(client_sock, "240 MOVE_FAIL not_in_match\r\n");
        return -1;
    }

    if (m->turn != idx) {
        pthread_mutex_unlock(&matches_mutex);
        send_status(client_sock, "241 MOVE_FAIL not_your_turn\r\n");
        return 0;
    }

    if (r < 0 || r >= BOARD_N || c < 0 || c >= BOARD_N) {
        pthread_mutex_unlock(&matches_mutex);
        send_status(client_sock, "242 MOVE_FAIL out_of_range\r\n");
        return 0;
    }

    if (m->board[r][c] != 0) {
        pthread_mutex_unlock(&matches_mutex);
        send_status(client_sock, "243 MOVE_FAIL position_occupied\r\n");
        return 0;
    }

    // apply move
    m->board[r][c] = idx + 1;
    int opponent = m->players[1 - idx];
    
    // Check if current player wins
    int is_win = check_win(m, idx);
    
    m->turn = 1 - m->turn;
    
    if (is_win) {
        m->is_finished = 1;
        m->winner = idx;
    }

    pthread_mutex_unlock(&matches_mutex);

    send_status(client_sock, "150 MOVE_OK\r\n");

    if (opponent != 0) {
        snprintf(buf, sizeof(buf),
                 "OPPONENT_MOVE row %d col %d\r\n",
                 r, c);
        send_status(opponent, buf);
    }
    
    // If match finished, send result to both players and remove match
    if (is_win) {
        snprintf(buf, sizeof(buf), "160 MATCH_RESULT id %d result WIN\r\n", match_id);
        send_status(client_sock, buf);
        if (opponent != 0) {
            send_status(opponent, buf);
        }
        // Remove match from list
        pthread_mutex_lock(&matches_mutex);
        match_t **pp = &matches;
        while (*pp) {
            if ((*pp)->id == match_id) {
                match_t *tmp = *pp;
                *pp = tmp->next;
                free(tmp);
                break;
            }
            pp = &(*pp)->next;
        }
        pthread_mutex_unlock(&matches_mutex);
    }
    return 1;
}

// Handle a single line from client
void handle_line(int client_sock, const char *line) {
    // MOVE command
    if (strncmp(line, "MOVE", 4) == 0) {
        int match_id, r, c;
        if (sscanf(line, "MOVE match %d row %d col %d",
                   &match_id, &r, &c) == 3) {

            int assigned = assign_player_to_match(match_id, client_sock);
            if (assigned < -1) {
                send_status(client_sock, STR_SERVER_ERROR);
                return;
            }
            process_move(client_sock, match_id, r, c);
            return;
        } else {
            send_status(client_sock, "244 MOVE_FAIL format_error\r\n");
            return;
        }
    }

    // REGISTER / LOGIN / LOGOUT (giữ nguyên)
    char cmd[16], u[128], p[128];
    if (sscanf(line, "%15s %127s %127s", cmd, u, p) < 1) {
        send_status(client_sock, STR_SERVER_ERROR);
        return;
    }

    if (strcmp(cmd, "REGISTER") == 0) {
        if (strlen(u)==0 || strlen(p)==0) {
            send_status(client_sock, STR_REGISTER_FAIL_EMPTY);
            return;
        }
        int r = register_user(u, p);
        if (r==1) send_status(client_sock, STR_REGISTER_OK);
        else if (r==0) send_status(client_sock, STR_REGISTER_FAIL_EXISTS);
        else send_status(client_sock, STR_SERVER_ERROR);
        return;
    }

    if (strcmp(cmd, "LOGIN") == 0) {
        int r = check_user_pass(u, p);
        if (r==1) send_status(client_sock, STR_LOGIN_OK);
        else if (r==-1) send_status(client_sock, STR_LOGIN_FAIL_PASSWORD);
        else send_status(client_sock, STR_LOGIN_FAIL_USERNAME);
        return;
    }

    if (strcmp(cmd, "LOGOUT") == 0) {
        send_status(client_sock, STR_LOGOUT_OK);
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
    remove_player_from_matches(client_sock);
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

