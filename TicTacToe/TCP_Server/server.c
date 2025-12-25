// server.c
// Compile: gcc -pthread -o server server.c
// Usage: ./server <port>
//
// Implements: REGISTER, LOGIN, LIST_PLAYERS, CHALLENGE, CHALLENGE_ACCEPT, CHALLENGE_DECLINE
// Protocol follows "Giao th?c ?ng d?ng(3).docx" codes strictly (no extra error codes).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>

#define BACKLOG 10
#define BUF_SIZE 4096
#define USERS_FILE "users.txt"

#define STR_LOGIN_OK "110 LOGIN_OK\r\n"
#define STR_REGISTER_OK "120 REGISTER_OK\r\n"

#define STR_LOGIN_FAIL_USERNAME "220 LOGIN_FAIL wrong_username\r\n"
#define STR_LOGIN_FAIL_PASSWORD "220 LOGIN_FAIL wrong_password\r\n"
#define STR_LOGIN_FAIL_EMPTY "221 LOGIN_FAIL empty_field\r\n"
#define STR_LOGIN_FAIL_ALREADY "222 LOGIN_FAIL already_logged_in\r\n"

#define STR_REGISTER_FAIL_EXISTS "221 REGISTER_FAIL user_exists\r\n"
#define STR_REGISTER_FAIL_EMPTY "222 REGISTER_FAIL empty_field\r\n"
#define STR_REGISTER_FAIL_WEAK "223 REGISTER_FAIL weak_password\r\n"
#define STR_REGISTER_FAIL_INVALID "224 REGISTER_FAIL invalid_username\r\n"

#define STR_LIST_FAIL_NOT_LOGGED "221 LIST_FAIL not_logged_in\r\n"
#define STR_PLAYER_LIST_FMT "100 PLAYER_LIST [%s]\r\n"
#define STR_PLAYER_LIST_EMPTY "101 PLAYER_LIST []\r\n"

#define STR_CHALLENGE_FAIL_NOT_LOGGED "221 CHALLENGE_FAIL not_logged_in\r\n"
#define STR_CHALLENGE_SENT "130 CHALLENGE_SENT\r\n"
#define STR_CHALLENGE_REQUEST_FMT "131 CHALLENGE_REQUEST from %s\r\n"

#define STR_CHALLENGE_DECLINED "133 CHALLENGE_DECLINED\r\n"
#define STR_CHALLENGE_ACCEPTED "132 CHALLENGE_ACCEPTED\r\n"

#define STR_CHALLENGE_FAIL_USER_NOT_FOUND "231 CHALLENGE_FAIL user_not_found\r\n"
#define STR_CHALLENGE_FAIL_USER_BUSY "232 CHALLENGE_FAIL user_busy\r\n"
#define STR_CHALLENGE_FAIL_SELF "234 CHALLENGE_FAIL self_challenge\r\n"
#define STR_CHALLENGE_FAIL_RATE "236 CHALLENGE_FAIL rate_limited\r\n"
#define STR_CHALLENGE_FAIL_OFFLINE "237 CHALLENGE_FAIL offline_target\r\n"
#define STR_CHALLENGE_FAIL_EXPIRED "238 CHALLENGE_FAIL expired_request\r\n"
#define STR_CHALLENGE_FAIL_LEVELGAP "233 CHALLENGE_FAIL level_gap_too_large\r\n"

#define STR_LOGOUT_OK "140 LOGOUT_OK\r\n"

#define STR_SESSION_NOT_FOUND "301 SESSION_NOT_FOUND\r\n"
#define STR_ALREADY_IN_MATCH "304 ALREADY_IN_MATCH\r\n"
#define STR_MATCH_FAIL_PLAYER_UNAVAILABLE "341 MATCH_FAIL player_unavailable\r\n"

#define STR_SERVER_ERROR "500 SERVER_ERROR\r\n"

pthread_mutex_t users_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t online_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t pending_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t recent_mutex = PTHREAD_MUTEX_INITIALIZER;

#define MAX_ONLINE 512
#define MAX_PENDING 1024
#define MAX_RECENT 256

const int CHALLENGE_TIMEOUT = 30;   // seconds
const int LEVEL_GAP_THRESHOLD = 10; // ELO difference threshold
const int RATE_LIMIT_WINDOW = 3;    // seconds
const int RATE_LIMIT_COUNT = 3;     // allowed in window

typedef struct {
    int sock;
    int logged_in;
    char username[128];
    int in_match;
} client_state_t;

typedef struct {
    char username[128];
    int sock;
    int in_match;
    int elo;
} online_user_t;

typedef struct {
    char from[128];
    char to[128];
    time_t ts;
} pending_t;

typedef struct {
    char username[128];
    time_t times[16];
    int idx;
} recent_t;

online_user_t online_users[MAX_ONLINE];
int online_count = 0;

pending_t pending[MAX_PENDING];
int pending_count = 0;

recent_t recent_list[MAX_RECENT];
int recent_count = 0;

/* utility */
static void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) { s[n-1] = '\0'; n--; }
}

ssize_t send_all(int sock, const char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t s = send(sock, buf + total, len - total, 0);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (s == 0) return (ssize_t)total;
        total += s;
    }
    return total;
}

void send_status(int client_sock, const char *msg) { send_all(client_sock, msg, strlen(msg)); }

/* users file: supports "username password" or "username password elo" */
int parse_users_file(const char *username, char *password_out, int *elo_out) {
    FILE *f = fopen(USERS_FILE, "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        trim_crlf(line);
        if (line[0] == '\0') continue;
        char u[128], p[128];
        int elo = -1;
        int n = sscanf(line, "%127s %127s %d", u, p, &elo);
        if (n >= 2 && strcmp(u, username) == 0) {
            if (password_out) strncpy(password_out, p, 127);
            if (elo_out) {
                if (n == 3) *elo_out = elo;
                else *elo_out = 1000;
            }
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

int user_exists_file(const char *username) {
    FILE *f = fopen(USERS_FILE, "r");
    if (!f) return 0;
    char u[128], p[128];
    int elo;
    int found = 0;
    while (fscanf(f, "%127s %127s %d", u, p, &elo) >= 2) {
        if (strcmp(u, username) == 0) { found = 1; break; }
        if (feof(f)) break;
    }
    fclose(f);
    return found;
}

int register_user_file(const char *username, const char *password, int elo) {
    pthread_mutex_lock(&users_mutex);
    if (user_exists_file(username)) { pthread_mutex_unlock(&users_mutex); return 0; }
    FILE *f = fopen(USERS_FILE, "a");
    if (!f) { pthread_mutex_unlock(&users_mutex); return -1; }
    if (fprintf(f, "%s %s %d\n", username, password, elo) < 0) {
        fclose(f); pthread_mutex_unlock(&users_mutex); return -2;
    }
    fclose(f);
    pthread_mutex_unlock(&users_mutex);
    return 1;
}

/* online management */
int find_online_index(const char *u) {
    for (int i = 0; i < online_count; ++i) if (strcmp(online_users[i].username, u) == 0) return i;
    return -1;
}

int is_online(const char *u) {
    pthread_mutex_lock(&online_mutex);
    int idx = find_online_index(u);
    pthread_mutex_unlock(&online_mutex);
    return idx >= 0;
}

int read_elo_from_file(const char *username) {
    char pw[128];
    int elo = 1000;
    if (parse_users_file(username, pw, &elo)) return elo;
    return 1000;
}

void add_online(const char *u, int sock, int elo) {
    pthread_mutex_lock(&online_mutex);
    if (online_count < MAX_ONLINE) {
        snprintf(online_users[online_count].username, sizeof(online_users[online_count].username), "%s", u);
        online_users[online_count].sock = sock;
        online_users[online_count].in_match = 0;
        online_users[online_count].elo = elo;
        online_count++;
        printf("[DEBUG] add_online: %s (sock=%d, elo=%d) online_count=%d\n", u, sock, elo, online_count);
    }
    pthread_mutex_unlock(&online_mutex);
}

void remove_online(const char *u) {
    pthread_mutex_lock(&online_mutex);
    int idx = find_online_index(u);
    if (idx != -1) {
        for (int j = idx; j < online_count - 1; ++j) online_users[j] = online_users[j+1];
        online_count--;
        printf("[DEBUG] remove_online: %s removed, online_count=%d\n", u, online_count);
    }
    pthread_mutex_unlock(&online_mutex);
}

/* pending challenges */
int find_pending_index(const char *from, const char *to) {
    for (int i = 0; i < pending_count; ++i) if (strcmp(pending[i].from, from) == 0 && strcmp(pending[i].to, to) == 0) return i;
    return -1;
}

void add_pending(const char *from, const char *to) {
    pthread_mutex_lock(&pending_mutex);
    // avoid duplicate pending requests
    if (find_pending_index(from, to) != -1) { pthread_mutex_unlock(&pending_mutex); return; }
    if (pending_count < MAX_PENDING) {
        snprintf(pending[pending_count].from, sizeof(pending[pending_count].from), "%s", from);
        snprintf(pending[pending_count].to, sizeof(pending[pending_count].to), "%s", to);
        pending[pending_count].ts = time(NULL);
        pending_count++;
    }
    pthread_mutex_unlock(&pending_mutex);
}

void remove_pending(const char *from, const char *to) {
    pthread_mutex_lock(&pending_mutex);
    int idx = find_pending_index(from, to);
    if (idx != -1) {
        for (int j = idx; j < pending_count - 1; ++j) pending[j] = pending[j+1];
        pending_count--;
    }
    pthread_mutex_unlock(&pending_mutex);
}

int has_pending(const char *from, const char *to) {
    pthread_mutex_lock(&pending_mutex);
    int found = find_pending_index(from, to) != -1;
    pthread_mutex_unlock(&pending_mutex);
    return found;
}

void remove_all_pending_for(const char *user) {
    pthread_mutex_lock(&pending_mutex);
    int i = 0;
    while (i < pending_count) {
        if (strcmp(pending[i].from, user) == 0 || strcmp(pending[i].to, user) == 0) {
            for (int j = i; j < pending_count - 1; ++j) pending[j] = pending[j+1];
            pending_count--;
        } else i++;
    }
    pthread_mutex_unlock(&pending_mutex);
}

void expire_pending() {
    pthread_mutex_lock(&pending_mutex);
    time_t now = time(NULL);
    int i = 0;
    while (i < pending_count) {
        if (now - pending[i].ts > CHALLENGE_TIMEOUT) {
            for (int j = i; j < pending_count - 1; ++j) pending[j] = pending[j+1];
            pending_count--;
        } else i++;
    }
    pthread_mutex_unlock(&pending_mutex);
}

/* rate limiting */
recent_t *find_recent(const char *username) {
    for (int i = 0; i < recent_count; ++i) if (strcmp(recent_list[i].username, username) == 0) return &recent_list[i];
    return NULL;
}

void record_challenge_time(const char *username) {
    time_t now = time(NULL);
    pthread_mutex_lock(&recent_mutex);
    recent_t *r = find_recent(username);
    if (!r) {
        if (recent_count < MAX_RECENT) {
            snprintf(recent_list[recent_count].username, sizeof(recent_list[recent_count].username), "%s", username);
            for (int k = 0; k < 16; ++k) recent_list[recent_count].times[k] = 0;
            recent_list[recent_count].idx = 0;
            r = &recent_list[recent_count++];
        } else { pthread_mutex_unlock(&recent_mutex); return; }
    }
    r->times[r->idx % 16] = now;
    r->idx++;
    pthread_mutex_unlock(&recent_mutex);
}

int is_rate_limited(const char *username) {
    pthread_mutex_lock(&recent_mutex);
    recent_t *r = find_recent(username);
    if (!r) { pthread_mutex_unlock(&recent_mutex); return 0; }
    time_t now = time(NULL);
    int cnt = 0;
    for (int k = 0; k < 16; ++k) {
        if (r->times[k] != 0 && now - r->times[k] <= RATE_LIMIT_WINDOW) cnt++;
    }
    pthread_mutex_unlock(&recent_mutex);
    return cnt >= RATE_LIMIT_COUNT;
}

/* handlers */

void handle_list_players(int client_sock, client_state_t *st) {
    if (!st->logged_in) { 
        send_status(client_sock, STR_LIST_FAIL_NOT_LOGGED); 
        return; 
    }

    pthread_mutex_lock(&online_mutex);
    if (online_count == 0) {
        pthread_mutex_unlock(&online_mutex);
        send_status(client_sock, STR_PLAYER_LIST_EMPTY);
        return;
    }

    printf("[DEBUG] Online users count = %d\n", online_count);
    for (int i = 0; i < online_count; i++)
        printf("  %s (in_match=%d, sock=%d, elo=%d)\n", online_users[i].username, online_users[i].in_match, online_users[i].sock, online_users[i].elo);

    char buf[4096] = "";
    for (int i = 0; i < online_count; ++i) {
        if (strlen(buf) > 0) strncat(buf, ",", sizeof(buf)-strlen(buf)-1);
        strncat(buf, online_users[i].username, sizeof(buf)-strlen(buf)-1);
    }
    pthread_mutex_unlock(&online_mutex);

    char out[4096];
    snprintf(out, sizeof(out), STR_PLAYER_LIST_FMT, buf);
    if (send_all(client_sock, out, strlen(out)) < 0) {
        printf("[ERROR] failed to send player list to sock=%d\n", client_sock);
    }
}

void handle_challenge(int client_sock, client_state_t *st, const char *target) {
    if (!st->logged_in) { send_status(client_sock, STR_CHALLENGE_FAIL_NOT_LOGGED); return; }
    if (!target || strlen(target) == 0) { send_status(client_sock, STR_CHALLENGE_FAIL_USER_NOT_FOUND); return; }

    printf("[DEBUG] handle_challenge: from='%s' target='%s'\n", st->username, target);
    fflush(stdout);

    expire_pending();

    if (strcmp(st->username, target) == 0) { send_status(client_sock, STR_CHALLENGE_FAIL_SELF); return; }

    pthread_mutex_lock(&online_mutex);
    int idx = find_online_index(target);
    if (idx == -1) { printf("[DEBUG] handle_challenge: target '%s' not found online\n", target); pthread_mutex_unlock(&online_mutex); send_status(client_sock, STR_CHALLENGE_FAIL_USER_NOT_FOUND); return; }
    if (online_users[idx].in_match) { pthread_mutex_unlock(&online_mutex); send_status(client_sock, STR_CHALLENGE_FAIL_USER_BUSY); return; }
    int target_sock = online_users[idx].sock;
    int target_elo = online_users[idx].elo;
    printf("[DEBUG] handle_challenge: target_idx=%d sock=%d elo=%d\n", idx, target_sock, target_elo);
    pthread_mutex_unlock(&online_mutex);

    int from_elo = read_elo_from_file(st->username);
    if (abs(from_elo - target_elo) > LEVEL_GAP_THRESHOLD) { send_status(client_sock, STR_CHALLENGE_FAIL_LEVELGAP); return; }

    if (is_rate_limited(st->username)) { send_status(client_sock, STR_CHALLENGE_FAIL_RATE); return; }
    record_challenge_time(st->username);

    add_pending(st->username, target);

    char req[256];
    snprintf(req, sizeof(req), STR_CHALLENGE_REQUEST_FMT, st->username);
    if (send_all(target_sock, req, strlen(req)) < 0) {
        // cannot deliver -> treat as offline
        remove_pending(st->username, target);
        send_status(client_sock, STR_CHALLENGE_FAIL_OFFLINE);
        return;
    }

    send_status(client_sock, STR_CHALLENGE_SENT);
}

void handle_challenge_accept(client_state_t *st, const char *from_user) {
    if (!st->logged_in) { send_status(st->sock, STR_SESSION_NOT_FOUND); return; }

    expire_pending();

    if (st->in_match) { send_status(st->sock, STR_ALREADY_IN_MATCH); return; }
    if (!has_pending(from_user, st->username)) { send_status(st->sock, STR_MATCH_FAIL_PLAYER_UNAVAILABLE); return; }

    pthread_mutex_lock(&online_mutex);
    int idx = find_online_index(from_user);
    if (idx == -1) { pthread_mutex_unlock(&online_mutex); remove_pending(from_user, st->username); send_status(st->sock, STR_MATCH_FAIL_PLAYER_UNAVAILABLE); return; }
    if (online_users[idx].in_match) { pthread_mutex_unlock(&online_mutex); remove_pending(from_user, st->username); send_status(st->sock, STR_MATCH_FAIL_PLAYER_UNAVAILABLE); return; }
    int from_sock = online_users[idx].sock;
    // mark both in_match
    for (int i = 0; i < online_count; ++i) {
        if (strcmp(online_users[i].username, st->username) == 0) online_users[i].in_match = 1;
        if (strcmp(online_users[i].username, from_user) == 0) online_users[i].in_match = 1;
    }
    // ensure acceptor's client_state reflects match state
    st->in_match = 1;
    pthread_mutex_unlock(&online_mutex);

    pthread_mutex_lock(&pending_mutex);
    int pidx = find_pending_index(from_user, st->username);
    if (pidx == -1) { pthread_mutex_unlock(&pending_mutex); send_status(st->sock, STR_MATCH_FAIL_PLAYER_UNAVAILABLE); return; }
    time_t now = time(NULL);
    if (now - pending[pidx].ts > CHALLENGE_TIMEOUT) {
        remove_pending(from_user, st->username);
        pthread_mutex_unlock(&pending_mutex);
        send_status(st->sock, STR_CHALLENGE_FAIL_EXPIRED);
        return;
    }
    remove_pending(from_user, st->username);
    pthread_mutex_unlock(&pending_mutex);

    // send accepted messages
    send_status(st->sock, STR_CHALLENGE_ACCEPTED);

    char to_sender[256];
    snprintf(to_sender, sizeof(to_sender), "132 CHALLENGE_ACCEPTED from %s\r\n", st->username);
    send_all(from_sock, to_sender, strlen(to_sender));

    // send plain TEXT notification (no status code) as you requested (B)
    char notif_a[256], notif_b[256];
    snprintf(notif_a, sizeof(notif_a), "MATCH between %s and %s has begun\r\n", st->username, from_user);
    snprintf(notif_b, sizeof(notif_b), "MATCH between %s and %s has begun\r\n", from_user, st->username);
    // send to acceptor (st->sock) and sender (from_sock)
    send_all(st->sock, notif_a, strlen(notif_a));
    send_all(from_sock, notif_b, strlen(notif_b));
}

void handle_challenge_decline(client_state_t *st, const char *from_user) {
    if (!st->logged_in) { send_status(st->sock, STR_SESSION_NOT_FOUND); return; }

    expire_pending();

    if (!has_pending(from_user, st->username)) { send_status(st->sock, STR_MATCH_FAIL_PLAYER_UNAVAILABLE); return; }

    pthread_mutex_lock(&online_mutex);
    int idx = find_online_index(from_user);
    int from_sock = -1;
    if (idx >= 0) from_sock = online_users[idx].sock;
    pthread_mutex_unlock(&online_mutex);

    remove_pending(from_user, st->username);

    send_status(st->sock, STR_CHALLENGE_DECLINED);
    if (from_sock >= 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "133 CHALLENGE_DECLINED from %s\r\n", st->username);
        send_all(from_sock, msg, strlen(msg));
    }
}

/* perform logout for a client state */
void do_logout(int client_sock, client_state_t *st) {
    if (!st->logged_in) { send_status(client_sock, STR_SESSION_NOT_FOUND); return; }
    remove_online(st->username);
    remove_all_pending_for(st->username);
    st->logged_in = 0;
    st->username[0] = '\0';
    st->in_match = 0;
    send_status(client_sock, STR_LOGOUT_OK);
    printf("[DEBUG] user logged out (do_logout), sock=%d\n", client_sock);
    fflush(stdout);
}

/* parse and dispatch */

void handle_line(int client_sock, const char *line, client_state_t *st) {
    char cmd[512];
    int j = 0;
    for (size_t i = 0; i < strlen(line) && j < sizeof(cmd)-1; i++) {
        if (line[i] != '\r' && line[i] != '\n') cmd[j++] = line[i];
    }
    cmd[j] = '\0';

        printf("[DEBUG] handle_line: raw cmd='%s' (len=%zu) from sock=%d logged_in=%d user='%s'\n",
            cmd, strlen(cmd), client_sock, st->logged_in, st->username);
        fflush(stdout);

    // ----------------------------
    // Map menu numbers to actions
    // ----------------------------
    if (strcmp(cmd, "1") == 0) { send_status(client_sock, "INFO: Please type REGISTER username <user> password <pass>\r\n"); return; }
    if (strcmp(cmd, "2") == 0) { send_status(client_sock, "INFO: Please type LOGIN username <user> password <pass>\r\n"); return; }
    if (strcmp(cmd, "3") == 0) { do_logout(client_sock, st); return; }
    if (strcmp(cmd, "4") == 0) { handle_list_players(client_sock, st); return; }
    if (strcmp(cmd, "5") == 0) { send_status(client_sock, "INFO: Use CHALLENGE to <player>\r\n"); return; }
    if (strcmp(cmd, "6") == 0) { send_status(client_sock, "INFO: Exiting...\r\n"); return; }

    // ----------------------------
    // Standard protocol commands
    // ----------------------------
    if (strncasecmp(cmd, "REGISTER", 8) == 0) {
        char t1[32], u[128], t2[32], p[128];
        int items = sscanf(cmd, "REGISTER %31s %127s %31s %127s", t1, u, t2, p);
        char username[128]={0}, password[128]={0};
        if (items == 4 && strcmp(t1,"username")==0 && strcmp(t2,"password")==0) {
            strncpy(username,u,sizeof(username)-1);
            strncpy(password,p,sizeof(password)-1);
        } else if (sscanf(cmd, "REGISTER %127s %127s", username, password) != 2) {
            send_status(client_sock, STR_REGISTER_FAIL_EMPTY);
            return;
        }
        trim_crlf(username); trim_crlf(password);
        if (strlen(username)==0 || strlen(password)==0) { send_status(client_sock, STR_REGISTER_FAIL_EMPTY); return; }
        int okname = 1;
        for (size_t i=0;i<strlen(username);++i) if (!isalnum((unsigned char)username[i]) && username[i]!='_' && username[i]!='-') { okname=0; break; }
        if (!okname) { send_status(client_sock, STR_REGISTER_FAIL_INVALID); return; }
        if (strlen(password)<1) { send_status(client_sock, STR_REGISTER_FAIL_WEAK); return; }
        int r = register_user_file(username,password,1000);
        if (r==1) send_status(client_sock, STR_REGISTER_OK);
        else if (r==0) send_status(client_sock, STR_REGISTER_FAIL_EXISTS);
        else send_status(client_sock, STR_SERVER_ERROR);
        return;
    }

    if (strncasecmp(cmd, "LOGIN", 5) == 0) {
        char t1[32], u[128], t2[32], p[128];
        int items = sscanf(cmd, "LOGIN %31s %127s %31s %127s", t1, u, t2, p);
        char username[128]={0}, password[128]={0};
        if (items==4 && strcmp(t1,"username")==0 && strcmp(t2,"password")==0) {
            strncpy(username,u,sizeof(username)-1);
            strncpy(password,p,sizeof(password)-1);
        } else if (sscanf(cmd, "LOGIN %127s %127s", username, password) != 2) {
            send_status(client_sock, STR_LOGIN_FAIL_USERNAME);
            return;
        }
        trim_crlf(username); trim_crlf(password);
        if (strlen(username)==0 || strlen(password)==0) { send_status(client_sock, STR_LOGIN_FAIL_EMPTY); return; }
        char realpw[128]; int elo=1000;
        if (!parse_users_file(username, realpw, &elo)) { send_status(client_sock, STR_LOGIN_FAIL_USERNAME); return; }
        if (strcmp(realpw,password)!=0) { send_status(client_sock, STR_LOGIN_FAIL_PASSWORD); return; }
        if (is_online(username)) { send_status(client_sock, STR_LOGIN_FAIL_ALREADY); return; }
        send_status(client_sock, STR_LOGIN_OK);
        st->logged_in = 1;
        strncpy(st->username, username, sizeof(st->username)-1);
        st->in_match = 0;
        add_online(username, client_sock, elo);
        return;
    }

    if (strncasecmp(cmd,"LIST_PLAYERS",12)==0) { handle_list_players(client_sock, st); return; }

    if (strncasecmp(cmd,"CHALLENGE",9)==0) {
        char target[128];
        if (sscanf(cmd,"CHALLENGE to %127s",target)==1 || sscanf(cmd,"CHALLENGE %127s",target)==1) handle_challenge(client_sock, st, target);
        else send_status(client_sock, STR_SERVER_ERROR);
        return;
    }

    if (strncasecmp(cmd,"CHALLENGE_ACCEPT",16)==0) {
        char from[128];
        if (sscanf(cmd,"CHALLENGE_ACCEPT from %127s",from)==1) handle_challenge_accept(st,from);
        else send_status(client_sock, STR_SERVER_ERROR);
        return;
    }

    if (strncasecmp(cmd, "LOGOUT", 6) == 0) {
        do_logout(client_sock, st);
        return;
    }

    if (strncasecmp(cmd,"CHALLENGE_DECLINE",17)==0) {
        char from[128];
        if (sscanf(cmd,"CHALLENGE_DECLINE from %127s",from)==1) handle_challenge_decline(st,from);
        else send_status(client_sock, STR_SERVER_ERROR);
        return;
    }

    send_status(client_sock, STR_SERVER_ERROR);
}


/* client thread */
void *client_thread(void *arg) {
    int client_sock = *(int*)arg; free(arg);

    client_state_t st;
    st.sock = client_sock; st.logged_in = 0; st.username[0] = '\0'; st.in_match = 0;

    char buf[BUF_SIZE], linebuf[BUF_SIZE];
    size_t linepos = 0;

    while (1) {
        ssize_t n = recv(client_sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        for (ssize_t i=0;i<n;++i) {
            if (linepos < sizeof(linebuf)-1) linebuf[linepos++] = buf[i];
            if (linepos >= 2 && linebuf[linepos-2] == '\r' && linebuf[linepos-1] == '\n') {
                linebuf[linepos] = '\0';
                trim_crlf(linebuf);
                if (strlen(linebuf) > 0) handle_line(client_sock, linebuf, &st);
                linepos = 0;
            }
        }
    }

    if (st.logged_in) {
        remove_online(st.username);
        remove_all_pending_for(st.username);
    }

    close(client_sock);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <port>\n", argv[0]); return 1; }
    int port = atoi(argv[1]);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(server_fd); return 1; }
    if (listen(server_fd, BACKLOG) < 0) { perror("listen"); close(server_fd); return 1; }

    printf("[SERVER] Listening on port %d...\n", port);

    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int *client_sock = malloc(sizeof(int));
        if (!client_sock) continue;
        *client_sock = accept(server_fd, (struct sockaddr*)&cli_addr, &cli_len);
        if (*client_sock < 0) { perror("accept"); free(client_sock); continue; }

        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(cli_addr.sin_addr), ipstr, sizeof(ipstr));
        printf("[SERVER] Client connected: %s:%d\n", ipstr, ntohs(cli_addr.sin_port));

        pthread_t th;
        pthread_create(&th, NULL, client_thread, client_sock);
        pthread_detach(th);
    }

    close(server_fd);
    return 0;
}

