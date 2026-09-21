#ifndef SYNC_H
#define SYNC_H
/* Single definition of the semaphore type; sync.c and main.c must both use it. */
#include "tcb.h"
#include "spinlock.h"

typedef struct {
    volatile int count;
    tcb_t *waiter;   /* single-waiter simplification (see sync.c) */
    spinlock_t lock; /* protects count and waiter across CPUs */
} sem_t;

_Static_assert(sizeof(sem_t) == 24, "sem_t size changed");

void sem_init(sem_t *s, int initial_count);
void sem_wait(sem_t *s);
void sem_post(sem_t *s);
#endif
