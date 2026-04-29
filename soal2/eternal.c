#include "arena.h"

static int   msgid;
static pid_t my_pid;

// Player state
static int  player_idx  = -1;
static char player_name[64];
static int  player_gold, player_lvl, player_xp, player_weapon;

// Battle state
static int  battle_idx  = -1;
static int  my_hp, opp_hp, max_hp, opp_max_hp;
static char opp_name[64];
static char combat_log[MAX_LOG][128];
static int  log_count    = 0;
static volatile int battle_active = 0;
static volatile int battle_result = -1;
static pthread_mutex_t bmtx = PTHREAD_MUTEX_INITIALIZER;

// Terminal
static struct termios orig_term;

void set_raw() {
    struct termios t;
    tcgetattr(STDIN_FILENO, &orig_term);
    t = orig_term;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

void restore_term() {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
}

// ===== MESSAGING =====

void send_req(int req_type, int value, const char *uname, const char *pass) {
    Message m;
    memset(&m, 0, sizeof(m));
    m.mtype    = 1;
    m.req_type = req_type;
    m.pid      = my_pid;
    m.value    = value;
    if (uname) strncpy(m.username, uname, 63);
    if (pass)  strncpy(m.password, pass,  63);
    msgsnd(msgid, &m, MSGSIZE, 0);
}

// Receive with timeout (uses alarm)
static volatile int alarm_hit = 0;
void alarm_handler(int sig) { (void)sig; alarm_hit = 1; }

int recv_msg(Message *out, int timeout_sec) {
    signal(SIGALRM, alarm_handler);
    alarm_hit = 0;
    alarm(timeout_sec);
    int ret = msgrcv(msgid, out, MSGSIZE, (long)my_pid, 0);
    alarm(0);
    if (alarm_hit || ret < 0) return -1;
    return 0;
}

// ===== DISPLAY =====

void clear_scr() { printf("\033[2J\033[H"); fflush(stdout); }

void print_bar(int cur, int max, int w) {
    int filled = (max > 0) ? (cur * w / max) : 0;
    printf("[");
    for (int i = 0; i < w; i++) printf(i < filled ? "=" : " ");
    printf("] %d/%d", cur, max);
}

void draw_battle() {
    pthread_mutex_lock(&bmtx);
    clear_scr();
    printf("=== ARENA ===\n\n");
    printf("%-20s Lvl %d\n", player_name, player_lvl);
    printf("HP: "); print_bar(my_hp, max_hp, 20); printf("\n\n");
    printf("VS\n\n");
    printf("%-20s\n", opp_name);
    printf("HP: "); print_bar(opp_hp > 0 ? opp_hp : 0, opp_max_hp, 20); printf("\n\n");
    printf("Combat Log:\n");
    int start = log_count > MAX_LOG ? log_count - MAX_LOG : 0;
    for (int i = start; i < log_count; i++)
        printf("%s\n", combat_log[i % MAX_LOG]);
    printf("\n");
    char wname[32] = "None";
    if (player_weapon >= 0 && player_weapon < NUM_WEAPONS)
        strncpy(wname, WEAPONS[player_weapon].name, 31);
    printf("Weapon: %s\n", wname);
    printf("Cb: Atk(a) | Ult(u)\n");
    fflush(stdout);
    pthread_mutex_unlock(&bmtx);
}

// ===== BATTLE RECV THREAD =====

void *battle_recv(void *arg) {
    (void)arg;
    Message m;
    while (battle_active) {
        if (msgrcv(msgid, &m, MSGSIZE, (long)my_pid, 0) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (m.res_type == RES_BATTLE_UPD) {
            pthread_mutex_lock(&bmtx);
            char buf[256];
            strncpy(buf, m.data, sizeof(buf)-1);
            char *tok = strtok(buf, "|");
            if (tok) my_hp  = atoi(tok);
            tok = strtok(NULL, "|");
            if (tok) opp_hp = atoi(tok);
            tok = strtok(NULL, "");
            if (tok && strlen(tok) > 0) {
                strncpy(combat_log[log_count % MAX_LOG], tok, 127);
                log_count++;
            }
            pthread_mutex_unlock(&bmtx);
            draw_battle();
        } else if (m.res_type == RES_BATTLE_END) {
            battle_result = m.value;
            battle_active = 0;
            break;
        }
    }
    return NULL;
}

// ===== MATCHMAKING COUNTDOWN THREAD =====

static volatile int mm_running = 0;
static volatile int mm_secs    = MATCHMAKE_TIME;

void *mm_countdown(void *arg) {
    (void)arg;
    while (mm_running && mm_secs > 0) {
        printf("\rSearching for an opponent... (%d s)  ", mm_secs);
        fflush(stdout);
        sleep(1);
        mm_secs--;
    }
    return NULL;
}

// ===== BATTLE FLOW =====

void do_battle() {
    send_req(REQ_BATTLE, player_idx, NULL, NULL);
    Message m;
    if (recv_msg(&m, 5) < 0 || !m.value) {
        printf("Failed to enter matchmaking.\n");
        return;
    }

    // Wait for battle start (with countdown display)
    mm_running = 1; mm_secs = MATCHMAKE_TIME;
    pthread_t mm_tid;
    int err = pthread_create(&mm_tid, NULL, mm_countdown, NULL);
    if (err != 0) printf("\nThread can't be created : [%s]", strerror(err));

    while (1) {
        if (msgrcv(msgid, &m, MSGSIZE, (long)my_pid, 0) < 0) continue;
        if (m.res_type == RES_BATTLE_START) break;
    }
    mm_running = 0;
    pthread_join(mm_tid, NULL);
    printf("\n");

    // Parse "opp_name|my_hp|opp_hp"
    battle_idx = m.value;
    char tmp[256];
    strncpy(tmp, m.data, sizeof(tmp)-1);
    char *tok = strtok(tmp, "|");
    if (tok) strncpy(opp_name, tok, 63);
    tok = strtok(NULL, "|");
    if (tok) { my_hp  = max_hp  = atoi(tok); }
    tok = strtok(NULL, "|");
    if (tok) { opp_hp = opp_max_hp = atoi(tok); }
    log_count     = 0;
    battle_active = 1;
    battle_result = -1;

    pthread_t btid;
    err = pthread_create(&btid, NULL, battle_recv, NULL);
    if (err != 0) {
        printf("\nThread can't be created : [%s]", strerror(err));
        battle_active = 0;
        return;
    }

    draw_battle();
    set_raw();

    while (battle_active) {
        char c = 0;
        if (read(STDIN_FILENO, &c, 1) > 0) {
            if (c == 'a') {
                send_req(REQ_ATTACK, (player_idx << 16) | battle_idx, NULL, NULL);
            } else if (c == 'u') {
                if (player_weapon >= 0)
                    send_req(REQ_ULTIMATE, (player_idx << 16) | battle_idx, NULL, NULL);
                else {
                    pthread_mutex_lock(&bmtx);
                    strncpy(combat_log[log_count % MAX_LOG], "> No weapon equipped!", 127);
                    log_count++;
                    pthread_mutex_unlock(&bmtx);
                    draw_battle();
                }
            }
        }
        usleep(50000);
    }

    restore_term();
    pthread_join(btid, NULL);

    // Update local stats
    player_xp   += battle_result ? 50 : 15;
    player_gold += battle_result ? 120 : 30;
    player_lvl   = (player_xp / 100) + 1;

    clear_scr();
    printf("\n%s\n", battle_result ? "=== VICTORY ===" : "=== DEFEAT ===");
    printf("\nBattle ended. Press [ENTER] to continue...\n");
    getchar(); getchar();
}

// ===== ARMORY =====

void do_armory() {
    clear_scr();
    printf("=== ARMORY ===\n");
    printf("Gold: %d\n\n", player_gold);
    for (int i = 0; i < NUM_WEAPONS; i++)
        printf("%d. %-15s %5d G  +%d Dmg\n",
            i+1, WEAPONS[i].name, WEAPONS[i].price, WEAPONS[i].bonus_dmg);
    printf("0. Back\n");
    printf("Choice: ");

    int choice;
    scanf("%d", &choice);
    while (getchar() != '\n');
    if (choice < 1 || choice > NUM_WEAPONS) return;

    int wi = choice - 1;
    send_req(REQ_BUY, (player_idx << 8) | wi, NULL, NULL);

    Message m;
    if (recv_msg(&m, 5) < 0) { printf("No response.\n"); return; }

    if (!m.value) {
        printf("%s\n", m.data);
        sleep(1);
        return;
    }

    // Parse "gold|weapon_idx"
    char tmp[64];
    strncpy(tmp, m.data, sizeof(tmp)-1);
    char *tok = strtok(tmp, "|");
    if (tok) player_gold   = atoi(tok);
    tok = strtok(NULL, "|");
    if (tok) player_weapon = atoi(tok);

    printf("Purchased! Gold remaining: %d\n", player_gold);
    sleep(1);
}

// ===== HISTORY =====

void do_history() {
    send_req(REQ_HISTORY, player_idx, NULL, NULL);
    Message m;
    if (recv_msg(&m, 5) < 0 || m.res_type != RES_HISTORY) {
        printf("Failed to load history.\n");
        sleep(1);
        return;
    }

    clear_scr();
    printf("=== MATCH HISTORY ===\n\n");
    printf("%-8s %-20s %-6s %s\n", "Time", "Opponent", "Res", "XP");
    printf("%-8s %-20s %-6s %s\n", "----", "--------", "---", "--");

    if (strlen(m.data) == 0) {
        printf("No matches yet.\n");
    } else {
        char buf[256];
        strncpy(buf, m.data, sizeof(buf)-1);
        char *line = strtok(buf, "\n");
        while (line) {
            char ts[16]="", opp[64]="";
            int  res=0, xp=0;
            sscanf(line, "%15[^|]|%63[^|]|%d|%d", ts, opp, &res, &xp);
            printf("%-8s %-20s %-6s +%d XP\n",
                ts, opp, res ? "WIN" : "LOSS", xp);
            line = strtok(NULL, "\n");
        }
    }

    printf("\nPress any key...\n");
    getchar();
}

// ===== GAME MENU =====

void game_menu() {
    while (1) {
        clear_scr();
        printf("PROFILE\n");
        printf("Name : %-20s Lvl : %d\n", player_name, player_lvl);
        printf("Gold : %-20d XP  : %d\n\n", player_gold, player_xp);
        printf("1. Battle\n2. Armory\n3. History\n4. Logout\n");
        printf("> Choice: ");

        int c; scanf("%d", &c); while (getchar() != '\n');
        switch (c) {
            case 1: do_battle();  break;
            case 2: do_armory();  break;
            case 3: do_history(); break;
            case 4:
                send_req(REQ_LOGOUT, player_idx, NULL, NULL);
                printf("Logging out...\n");
                return;
            default: break;
        }
    }
}

// ===== AUTH MENU =====

void do_register() {
    char uname[64], pass[64];
    printf("\nCREATE ACCOUNT\n");
    printf("Username: "); scanf("%63s", uname); while (getchar() != '\n');
    printf("Password: "); scanf("%63s", pass);  while (getchar() != '\n');

    send_req(REQ_REGISTER, 0, uname, pass);
    Message m;
    if (recv_msg(&m, 5) < 0) { printf("No response from Orion.\n"); return; }
    printf("%s\n", m.data);
    sleep(1);
}

void do_login() {
    char uname[64], pass[64];
    printf("\nLOGIN\n");
    printf("Username: "); scanf("%63s", uname); while (getchar() != '\n');
    printf("Password: "); scanf("%63s", pass);  while (getchar() != '\n');

    send_req(REQ_LOGIN, 0, uname, pass);
    Message m;
    if (recv_msg(&m, 5) < 0) { printf("No response from Orion.\n"); return; }

    if (!m.value) { printf("%s\n", m.data); sleep(1); return; }

    // Parse "idx|gold|lvl|xp|weapon"
    char tmp[256];
    strncpy(tmp, m.data, sizeof(tmp)-1);
    char *tok = strtok(tmp, "|");
    if (tok) player_idx    = atoi(tok);
    tok = strtok(NULL, "|"); if (tok) player_gold   = atoi(tok);
    tok = strtok(NULL, "|"); if (tok) player_lvl    = atoi(tok);
    tok = strtok(NULL, "|"); if (tok) player_xp     = atoi(tok);
    tok = strtok(NULL, "|"); if (tok) player_weapon = atoi(tok);

    strncpy(player_name, uname, 63);
    printf("Welcome, %s!\n", player_name);
    sleep(1);
    game_menu();
    player_idx = -1;
}

// ===== MAIN =====

int main() {
    my_pid = getpid();

    // Check if orion is running
    msgid = msgget(MSG_KEY, 0666);
    if (msgid < 0) {
        printf("Orion are you there?\n");
        return 1;
    }

    send_req(REQ_PING, 0, NULL, NULL);
    Message pm;
    if (recv_msg(&pm, 2) < 0 || !pm.value) {
        printf("Orion are you there?\n");
        return 1;
    }

    while (1) {
        clear_scr();
        printf("1. Register\n2. Login\n3. Exit\n");
        printf("Choice: ");
        int c; scanf("%d", &c); while (getchar() != '\n');
        switch (c) {
            case 1: do_register(); break;
            case 2: do_login();    break;
            case 3: return 0;
            default: break;
        }
    }
    return 0;
}