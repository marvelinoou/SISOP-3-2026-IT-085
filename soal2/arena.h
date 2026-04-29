#ifndef ARENA_H
#define ARENA_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/sem.h>

// IPC Keys (sesuai Makefile soal)
#define SHM_KEY   0x00001234
#define MSG_KEY   0x00005678
#define SEM_KEY   0x00009012

// Game constants
#define MAX_PLAYERS    100
#define MAX_HISTORY    50
#define MAX_LOG        5
#define MAX_BATTLES    50
#define BASE_DAMAGE    10
#define BASE_HEALTH    100
#define MATCHMAKE_TIME 35
#define ATTACK_CD      1
#define PLAYER_FILE    "players.dat"
#define NUM_WEAPONS    5

// Request types (eternal → orion, mtype = 1)
#define REQ_PING     1
#define REQ_REGISTER 2
#define REQ_LOGIN    3
#define REQ_LOGOUT   4
#define REQ_BATTLE   5
#define REQ_ATTACK   6
#define REQ_ULTIMATE 7
#define REQ_BUY      8
#define REQ_HISTORY  9

// Response subtypes (orion → eternal, mtype = eternal pid)
#define RES_GENERAL      0
#define RES_BATTLE_START 1
#define RES_BATTLE_UPD   2
#define RES_BATTLE_END   3
#define RES_HISTORY      4

#define MSGSIZE (sizeof(Message) - sizeof(long))

typedef struct {
    char name[32];
    int  price;
    int  bonus_dmg;
} Weapon;

static const Weapon WEAPONS[NUM_WEAPONS] = {
    {"Wood Sword",   100,   5},
    {"Iron Sword",   300,  15},
    {"Steel Axe",    600,  30},
    {"Demon Blade", 1500,  60},
    {"God Slayer",  5000, 150}
};

typedef struct {
    char time_str[16];
    char opponent[64];
    int  result;     // 1=win, 0=loss
    int  xp_gained;
} MatchEntry;

typedef struct {
    char     username[64];
    char     password[64];
    int      gold, lvl, xp;
    int      weapon_idx;         // -1 = none
    MatchEntry history[MAX_HISTORY];
    int      history_count;
    // Runtime fields (not saved to file)
    int      active;
    pid_t    pid;
    int      in_battle;
    int      in_mm;
    int      battle_idx;
} Player;

typedef struct {
    int    active;
    int    p1_idx, p2_idx;       // p2_idx = -1 if bot
    int    p1_hp,  p2_hp;
    time_t p1_atk, p2_atk;      // last attack timestamp
    char   p1_log[MAX_LOG][128];
    char   p2_log[MAX_LOG][128];
    int    p1_log_i, p2_log_i;
    int    is_bot;
    char   bot_name[64];
    int    bot_dmg;
} Battle;

typedef struct {
    Player  players[MAX_PLAYERS];
    int     player_count;
    int     mm_queue[MAX_PLAYERS];
    time_t  mm_join[MAX_PLAYERS];
    int     mm_count;
    Battle  battles[MAX_BATTLES];
} SharedData;

typedef struct {
    long   mtype;
    int    req_type;
    int    res_type;
    pid_t  pid;        // eternal's pid (routing)
    int    value;      // multipurpose
    char   username[64];
    char   password[64];
    char   data[256];
} Message;

// Semaphore helpers
static inline void sem_lock(int semid) {
    struct sembuf sb = {0, -1, 0};
    semop(semid, &sb, 1);
}

static inline void sem_unlock(int semid) {
    struct sembuf sb = {0, 1, 0};
    semop(semid, &sb, 1);
}

// Stat calculators
static inline int get_damage(Player *p) {
    int bonus = (p->weapon_idx >= 0 && p->weapon_idx < NUM_WEAPONS)
                ? WEAPONS[p->weapon_idx].bonus_dmg : 0;
    return BASE_DAMAGE + (p->xp / 50) + bonus;
}

static inline int get_health(Player *p) {
    return BASE_HEALTH + (p->xp / 10);
}

static inline int calc_max_hp(int xp) {
    return BASE_HEALTH + (xp / 10);
}

#endif