/*
 * The trainer (F6): hle_trainer.c is the game side, renderer/app/trainer.cpp the window.
 */
#ifndef HPPOA_TRAINER_H
#define HPPOA_TRAINER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { HPPOA_TV_BOOL, HPPOA_TV_FLOAT, HPPOA_TV_INT, HPPOA_TV_UINT };

/* one of the title's tuning variables (trainer_vars.h) */
typedef struct HppoaTrainerVar {
    const char *name;
    int kind;          /* HPPOA_TV_* */
    uint32_t group;    /* the title's group mask */
    uint32_t va;       /* the value the game reads: a byte for bools, else 4 bytes */
    float def, min, max;
    float scale;       /* floats: stored = shown * scale */
} HppoaTrainerVar;

/* a value written back every game tick */
typedef struct HppoaFreeze {
    uint32_t va, size, value;
} HppoaFreeze;

#define HPPOA_MAX_FREEZES     512
#define HPPOA_CHARACTER_SLOTS 5
#define HPPOA_ITEM_IDS        106

typedef struct HppoaTrainer {
    volatile float speed;           /* the game clock's rate, 1 as the game */
    volatile int god_mode;          /* the characters' health kept full */
    volatile int debug_menu;        /* the native DebugMenu answers "true" */
    volatile int press_debug_key;   /* press the front end's debug menu key once */
    volatile int freeze_count;
    HppoaFreeze freezes[HPPOA_MAX_FREEZES];
} HppoaTrainer;

extern HppoaTrainer g_hppoa_trainer;

int hppoa_TrainerVarCount(void);
const HppoaTrainerVar *hppoa_TrainerVar(int i);

int hppoa_GuestOk(uint32_t va, uint32_t len);
void *hppoa_GuestPtr(uint32_t va);
uint32_t hppoa_GuestRead32(uint32_t va);

int hppoa_TrainerInGame(void);
uint32_t hppoa_TrainerCharacter(int slot);   /* 0 Harry, 1 Ron, 2 Hermione, 3 Ron (other), 4 Hermione (other) */
uint32_t hppoa_TrainerPlayer(void);          /* the character being played */
uint32_t hppoa_TrainerHealth(uint32_t chr);  /* +0x34 health, +0x3C most (floats) */
uint32_t hppoa_TrainerInventory(uint32_t chr);  /* a signed byte per item id */
uint32_t hppoa_TrainerSharedItems(void);     /* an int16 per item id */
int hppoa_TrainerItemName(int id, char *out, int len);

void hppoa_TrainerAfterTick(uint32_t clk, uint32_t t0);  /* hle_debug.c */

#ifdef __cplusplus
}
#endif

#endif
