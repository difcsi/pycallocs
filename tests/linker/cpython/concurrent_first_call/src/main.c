#include <pthread.h>
#include <stdio.h>
#include "race.py.h"

#define THREADS 8

static pthread_barrier_t barrier;
static int results[THREADS];

static void *worker(void *arg)
{
    int i = (int) (long) arg;
    pthread_barrier_wait(&barrier);          /* all first calls together */
    results[i] = answer_race();
    return NULL;
}

int main(void)
{
    pthread_t threads[THREADS];
    pthread_barrier_init(&barrier, NULL, THREADS);
    for (long i = 0; i < THREADS; i++)
        pthread_create(&threads[i], NULL, worker, (void *) i);
    int ok = 1;
    for (int i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
        ok = ok && results[i] == 42;
    }
    printf("%d threads made the first call together: %s\n", THREADS, ok ? "ok" : "WRONG");
    return ok ? 0 : 1;
}
