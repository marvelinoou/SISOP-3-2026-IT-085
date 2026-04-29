#include "arena.h"

static int shmid, msgid, semid;
static SharedData *shm;

// ===== FILE I/O =====

void save_players() {
    FILE *f = fopen(PLAYER_FILE, "wb");
    if (!f) return;
    fwrite(&shm->player_count, sizeof(int), 1, f);
    for (int i = 0; i < shm->player_count; i++) {
        Player tmp = shm->players[i];
        tmp.active = 0; tmp.in_battle = 0;
        tmp.in_mm  = 0; tmp.pid = 0;
        tmp.battle_idx = -1;
        fwrite(&tmp, sizeof(Player), 1, f);
    }
    fclose(f);
}

void load_players() {
    FILE *f = fopen(PLAYER_FILE, "rb");
    if (!f) { shm->player_count = 0; return; }
    fread(&shm->player_count, sizeof(int), 1, f);
    fread(shm->players, sizeof(Player), shm->player_count, f);
    fclose(f);
    for (int i = 0; i < shm->player_count; i++) {
        shm->players[i].active     = 0;
        shm->players[i].in_battle  = 0;
        shm->players[i].in_mm      = 0;
        shm->players[i].pid        = 0;
        shm->players[i].battle_idx = -1;
    }
}

// ===== HELPERS =====

int find_player(const char *username) {
    for (int i = 0; i < shm->player_count; i++)
        if (strcmp(shm->players[i].username, username) == 0)
            return i;
    return -1;
}

void respond(pid_t pid, int res_type, int value, const char *data) {
    Message m;
    memset(&m, 0, sizeof(m));
    m.mtype    = (long)pid;
    m.res_type = res_type;
    m.value    = value;
    if (data) strncpy(m.data, data, sizeof(m.data) - 1);
    msgsnd(msgid, &m, MSGSIZE, 0);
}

void add_log(Battle *b, int is_p1, const char *log) {
    if (is_p1) {
        strncpy(b->p1_log[b->p1_log_i % MAX_LOG], log, 127);
        b->p1_log_i++;
    } else {
        strncpy(b->p2_log[b->p2_log_i % MAX_LOG], log, 127);
        b->p2_log_i++;
    }
}

const char *latest_log(Battle *b, int is_p1) {
    int cnt = is_p1 ? b->p1_log_i : b->p2_log_i;
    if (cnt == 0) return "";
    char (*logs)[128] = is_p1 ? b->p1_log : b->p2_log;
    return logs[(cnt - 1) % MAX_LOG];
}

void broadcast_state(int bi) {
    Battle *b = &shm->battles[bi];
    Player *p1 = &shm->players[b->p1_idx];
    char buf[256];

    snprintf(buf, sizeof(buf), "%d|%d|%s", b->p1_hp, b->p2_hp, latest_log(b, 1));
    respond(p1->pid, RES_BATTLE_UPD, bi, buf);

    if (!b->is_bot) {
        Player *p2 = &shm->players[b->p2_idx];
        snprintf(buf, sizeof(buf), "%d|%d|%s", b->p2_hp, b->p1_hp, latest_log(b, 0));
        respond(p2->pid, RES_BATTLE_UPD, bi, buf);
    }
}

// ===== BATTLE =====

void end_battle(int bi, int p1_won) {
    Battle *b = &shm->battles[bi];
    if (!b->active) return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[16];
    strftime(ts, sizeof(ts), "%H:%M", t);

    int p1i = b->p1_idx;
    Player *p1 = &shm->players[p1i];
    int p1_xp  = p1_won ? 50 : 15;
    int p1_gld = p1_won ? 120 : 30;
    p1->xp   += p1_xp;
    p1->gold += p1_gld;
    p1->lvl   = (p1->xp / 100) + 1;

    MatchEntry e1;
    memset(&e1, 0, sizeof(e1));
    strncpy(e1.time_str, ts, 15);
    e1.result    = p1_won;
    e1.xp_gained = p1_xp;
    if (b->is_bot) strncpy(e1.opponent, b->bot_name, 63);
    else           strncpy(e1.opponent, shm->players[b->p2_idx].username, 63);

    if (p1->history_count < MAX_HISTORY)
        p1->history[p1->history_count++] = e1;
    else {
        for (int i = 0; i < MAX_HISTORY - 1; i++)
            p1->history[i] = p1->history[i+1];
        p1->history[MAX_HISTORY-1] = e1;
    }

    p1->in_battle  = 0;
    p1->battle_idx = -1;
    respond(p1->pid, RES_BATTLE_END, p1_won, p1_won ? "VICTORY" : "DEFEAT");

    if (!b->is_bot) {
        int p2i = b->p2_idx;
        Player *p2 = &shm->players[p2i];
        int p2_won = !p1_won;
        int p2_xp  = p2_won ? 50 : 15;
        int p2_gld = p2_won ? 120 : 30;
        p2->xp   += p2_xp;
        p2->gold += p2_gld;
        p2->lvl   = (p2->xp / 100) + 1;

        MatchEntry e2;
        memset(&e2, 0, sizeof(e2));
        strncpy(e2.time_str, ts, 15);
        strncpy(e2.opponent, p1->username, 63);
        e2.result    = p2_won;
        e2.xp_gained = p2_xp;

        if (p2->history_count < MAX_HISTORY)
            p2->history[p2->history_count++] = e2;
        else {
            for (int i = 0; i < MAX_HISTORY - 1; i++)
                p2->history[i] = p2->history[i+1];
            p2->history[MAX_HISTORY-1] = e2;
        }

        p2->in_battle  = 0;
        p2->battle_idx = -1;
        respond(p2->pid, RES_BATTLE_END, p2_won, p2_won ? "VICTORY" : "DEFEAT");
    }

    save_players();
    b->active = 0;
}

void start_battle(int p1i, int p2i, int is_bot) {
    int bi = -1;
    for (int i = 0; i < MAX_BATTLES; i++)
        if (!shm->battles[i].active) { bi = i; break; }
    if (bi < 0) return;

    Battle *b = &shm->battles[bi];
    memset(b, 0, sizeof(Battle));
    b->active = 1;
    b->p1_idx = p1i;
    b->p2_idx = p2i;
    b->is_bot = is_bot;

    Player *p1 = &shm->players[p1i];
    b->p1_hp = get_health(p1);
    p1->in_battle = 1; p1->in_mm = 0; p1->battle_idx = bi;

    char buf[256];
    if (is_bot) {
        const char *names[] = {"Wild Beast","Shadow Demon","Iron Golem","Dark Knight","Void Dragon"};
        strncpy(b->bot_name, names[rand() % 5], 63);
        b->bot_dmg = BASE_DAMAGE + rand() % 15;
        b->p2_hp   = BASE_HEALTH + rand() % 50;

        snprintf(buf, sizeof(buf), "%s|%d|%d", b->bot_name, b->p1_hp, b->p2_hp);
        respond(p1->pid, RES_BATTLE_START, bi, buf);
    } else {
        Player *p2 = &shm->players[p2i];
        b->p2_hp = get_health(p2);
        p2->in_battle = 1; p2->in_mm = 0; p2->battle_idx = bi;

        snprintf(buf, sizeof(buf), "%s|%d|%d", p2->username, b->p1_hp, b->p2_hp);
        respond(p1->pid, RES_BATTLE_START, bi, buf);

        snprintf(buf, sizeof(buf), "%s|%d|%d", p1->username, b->p2_hp, b->p1_hp);
        respond(p2->pid, RES_BATTLE_START, bi, buf);
    }
}

// ===== REQUEST HANDLERS =====

void h_ping(Message *m) {
    respond(m->pid, RES_GENERAL, 1, "PONG");
}

void h_register(Message *m) {
    sem_lock(semid);
    if (find_player(m->username) >= 0) {
        sem_unlock(semid);
        respond(m->pid, RES_GENERAL, 0, "Username already taken.");
        return;
    }
    if (shm->player_count >= MAX_PLAYERS) {
        sem_unlock(semid);
        respond(m->pid, RES_GENERAL, 0, "Server full.");
        return;
    }
    int i = shm->player_count++;
    memset(&shm->players[i], 0, sizeof(Player));
    strncpy(shm->players[i].username, m->username, 63);
    strncpy(shm->players[i].password, m->password, 63);
    shm->players[i].gold       = 150;
    shm->players[i].lvl        = 1;
    shm->players[i].xp         = 0;
    shm->players[i].weapon_idx = -1;
    shm->players[i].battle_idx = -1;
    save_players();
    sem_unlock(semid);
    respond(m->pid, RES_GENERAL, 1, "Account created!");
}

void h_login(Message *m) {
    sem_lock(semid);
    int i = find_player(m->username);
    if (i < 0) {
        sem_unlock(semid);
        respond(m->pid, RES_GENERAL, 0, "Username not found.");
        return;
    }
    if (strcmp(shm->players[i].password, m->password) != 0) {
        sem_unlock(semid);
        respond(m->pid, RES_GENERAL, 0, "Wrong password.");
        return;
    }
    if (shm->players[i].active) {
        sem_unlock(semid);
        respond(m->pid, RES_GENERAL, 0, "Account already logged in.");
        return;
    }
    shm->players[i].active = 1;
    shm->players[i].pid    = m->pid;

    char buf[256];
    snprintf(buf, sizeof(buf), "%d|%d|%d|%d|%d",
        i,
        shm->players[i].gold,
        shm->players[i].lvl,
        shm->players[i].xp,
        shm->players[i].weapon_idx);
    sem_unlock(semid);
    respond(m->pid, RES_GENERAL, 1, buf);
}

void h_logout(Message *m) {
    sem_lock(semid);
    int i = m->value;
    if (i >= 0 && i < shm->player_count) {
        shm->players[i].active = 0;
        shm->players[i].in_mm  = 0;
        save_players();
    }
    sem_unlock(semid);
    respond(m->pid, RES_GENERAL, 1, "Goodbye.");
}

void h_battle(Message *m) {
    int i = m->value;
    sem_lock(semid);
    if (shm->players[i].in_battle || shm->players[i].in_mm) {
        sem_unlock(semid);
        respond(m->pid, RES_GENERAL, 0, "Already in queue or battle.");
        return;
    }
    shm->mm_queue[shm->mm_count] = i;
    shm->mm_join[shm->mm_count]  = time(NULL);
    shm->mm_count++;
    shm->players[i].in_mm = 1;
    sem_unlock(semid);
    respond(m->pid, RES_GENERAL, 1, "Searching...");
}

void h_attack(Message *m, int ultimate) {
    int pi = m->value >> 16;
    int bi = m->value & 0xFFFF;
    if (bi < 0 || bi >= MAX_BATTLES || !shm->battles[bi].active) return;

    Battle *b   = &shm->battles[bi];
    int    is_p1 = (b->p1_idx == pi);
    time_t now   = time(NULL);
    time_t *last = is_p1 ? &b->p1_atk : &b->p2_atk;
    if (now - *last < ATTACK_CD) return;
    *last = now;

    Player *atk = &shm->players[pi];
    int dmg = get_damage(atk);
    if (ultimate) {
        if (atk->weapon_idx < 0) return;
        dmg *= 3;
    }

    char log1[128], log2[128];
    if (is_p1) {
        b->p2_hp -= dmg;
        if (b->p2_hp < 0) b->p2_hp = 0;
        snprintf(log1, sizeof(log1), "> You hit for %d damage!", dmg);
        snprintf(log2, sizeof(log2), "> Opponent hit you for %d damage!", dmg);
        add_log(b, 1, log1);
        if (!b->is_bot) add_log(b, 0, log2);
    } else {
        b->p1_hp -= dmg;
        if (b->p1_hp < 0) b->p1_hp = 0;
        snprintf(log1, sizeof(log1), "> You hit for %d damage!", dmg);
        snprintf(log2, sizeof(log2), "> Opponent hit you for %d damage!", dmg);
        add_log(b, 0, log1);
        add_log(b, 1, log2);
    }

    broadcast_state(bi);

    if (b->p1_hp <= 0 || b->p2_hp <= 0) {
        int p1_won = (b->p2_hp <= 0);
        end_battle(bi, p1_won);
    }
}

void h_buy(Message *m) {
    int pi = m->value >> 8;
    int wi = m->value & 0xFF;

    sem_lock(semid);
    Player *p = &shm->players[pi];
    if (wi < 0 || wi >= NUM_WEAPONS) {
        sem_unlock(semid);
        respond(m->pid, RES_GENERAL, 0, "Invalid weapon.");
        return;
    }
    if (p->gold < WEAPONS[wi].price) {
        sem_unlock(semid);
        respond(m->pid, RES_GENERAL, 0, "Not enough gold.");
        return;
    }
    p->gold -= WEAPONS[wi].price;
    if (p->weapon_idx < 0 || WEAPONS[wi].bonus_dmg > WEAPONS[p->weapon_idx].bonus_dmg)
        p->weapon_idx = wi;

    char buf[64];
    snprintf(buf, sizeof(buf), "%d|%d", p->gold, p->weapon_idx);
    save_players();
    sem_unlock(semid);
    respond(m->pid, RES_GENERAL, 1, buf);
}

void h_history(Message *m) {
    int pi = m->value;
    Player *p = &shm->players[pi];

    char buf[256] = "";
    int shown = 0;
    for (int i = p->history_count - 1; i >= 0 && shown < MAX_LOG; i--, shown++) {
        char line[80];
        snprintf(line, sizeof(line), "%s|%s|%d|%d\n",
            p->history[i].time_str,
            p->history[i].opponent,
            p->history[i].result,
            p->history[i].xp_gained);
        strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
    }
    respond(m->pid, RES_HISTORY, p->history_count, buf);
}

// ===== THREADS =====

typedef struct { int bi; } BotArg;

void *bot_thread(void *arg) {
    BotArg *ba = (BotArg *)arg;
    int bi = ba->bi;
    free(ba);

    while (1) {
        sleep(1 + rand() % 3);
        sem_lock(semid);
        Battle *b = &shm->battles[bi];
        if (!b->active) { sem_unlock(semid); break; }

        int dmg = b->bot_dmg;
        b->p1_hp -= dmg;
        if (b->p1_hp < 0) b->p1_hp = 0;

        char log[128];
        snprintf(log, sizeof(log), "> %s hit you for %d damage!", b->bot_name, dmg);
        add_log(b, 1, log);

        Player *p1 = &shm->players[b->p1_idx];
        char buf[256];
        snprintf(buf, sizeof(buf), "%d|%d|%s", b->p1_hp, b->p2_hp, latest_log(b, 1));
        respond(p1->pid, RES_BATTLE_UPD, bi, buf);

        if (b->p1_hp <= 0) {
            sem_unlock(semid);
            end_battle(bi, 0);
            break;
        }
        sem_unlock(semid);
    }
    return NULL;
}

void *mm_thread(void *arg) {
    (void)arg;
    while (1) {
        sleep(1);
        sem_lock(semid);
        time_t now = time(NULL);

        if (shm->mm_count >= 2) {
            int p1i = shm->mm_queue[0];
            int p2i = shm->mm_queue[1];
            for (int i = 0; i < shm->mm_count - 2; i++) {
                shm->mm_queue[i] = shm->mm_queue[i+2];
                shm->mm_join[i]  = shm->mm_join[i+2];
            }
            shm->mm_count -= 2;
            sem_unlock(semid);
            start_battle(p1i, p2i, 0);
            continue;
        }

        for (int i = 0; i < shm->mm_count; i++) {
            if (now - shm->mm_join[i] >= MATCHMAKE_TIME) {
                int p1i = shm->mm_queue[i];
                for (int j = i; j < shm->mm_count - 1; j++) {
                    shm->mm_queue[j] = shm->mm_queue[j+1];
                    shm->mm_join[j]  = shm->mm_join[j+1];
                }
                shm->mm_count--;
                sem_unlock(semid);

                start_battle(p1i, -1, 1);

                int bi = shm->players[p1i].battle_idx;
                if (bi >= 0) {
                    BotArg *ba = malloc(sizeof(BotArg));
                    ba->bi = bi;
                    pthread_t tid;
                    int err = pthread_create(&tid, NULL, bot_thread, ba);
                    if (err != 0) {
                        printf("\nThread can't be created : [%s]", strerror(err));
                        free(ba);
                    } else {
                        pthread_detach(tid);
                    }
                }
                goto next;
            }
        }
        sem_unlock(semid);
        next:;
    }
    return NULL;
}

void cleanup(int sig) {
    (void)sig;
    shmdt(shm);
    shmctl(shmid, IPC_RMID, NULL);
    msgctl(msgid, IPC_RMID, NULL);
    semctl(semid, 0, IPC_RMID);
    exit(0);
}

int main() {
    srand(time(NULL));
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    shmid = shmget(SHM_KEY, sizeof(SharedData), IPC_CREAT | 0666);
    if (shmid < 0) { perror("shmget"); exit(EXIT_FAILURE); }
    shm = (SharedData *)shmat(shmid, NULL, 0);
    if (shm == (void *)-1) { perror("shmat"); exit(EXIT_FAILURE); }
    memset(shm, 0, sizeof(SharedData));

    msgid = msgget(MSG_KEY, IPC_CREAT | 0666);
    if (msgid < 0) { perror("msgget"); exit(EXIT_FAILURE); }

    semid = semget(SEM_KEY, 1, IPC_CREAT | 0666);
    if (semid < 0) { perror("semget"); exit(EXIT_FAILURE); }
    semctl(semid, 0, SETVAL, 1);

    load_players();
    printf("Orion is ready (PID: %d)\n", getpid());

    pthread_t mm_tid;
    int err = pthread_create(&mm_tid, NULL, mm_thread, NULL);
    if (err != 0) {
        printf("\nThread can't be created : [%s]", strerror(err));
        exit(EXIT_FAILURE);
    }
    pthread_detach(mm_tid);

    Message msg;
    while (1) {
        if (msgrcv(msgid, &msg, MSGSIZE, 1, 0) < 0) continue;
        switch (msg.req_type) {
            case REQ_PING:     h_ping(&msg);       break;
            case REQ_REGISTER: h_register(&msg);   break;
            case REQ_LOGIN:    h_login(&msg);       break;
            case REQ_LOGOUT:   h_logout(&msg);      break;
            case REQ_BATTLE:   h_battle(&msg);      break;
            case REQ_ATTACK:   h_attack(&msg, 0);   break;
            case REQ_ULTIMATE: h_attack(&msg, 1);   break;
            case REQ_BUY:      h_buy(&msg);         break;
            case REQ_HISTORY:  h_history(&msg);     break;
        }
    }
    return 0;
}