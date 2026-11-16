/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "crypto.h"
#include "d3des.h"

static const unsigned char input[8] = {0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
static const unsigned char key_one[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
/* Produced by the previous D3DES implementation for the key and input above. */
static const unsigned char expected_one[8] = {0xbd, 0x80, 0xd0, 0x93, 0x88, 0x79, 0x80, 0xac};

struct worker {
    unsigned char key[8];
    unsigned char expected[8];
    pthread_barrier_t *barrier;
    int failed;
};

static void *encrypt_concurrently(void *arg)
{
    struct worker *worker = arg;
    unsigned long schedule[32];
    unsigned char output[8];
    int round;
    for (round = 0; round < 100; round++) {
        rfbDesKey(worker->key, EN0, schedule);
        pthread_barrier_wait(worker->barrier);
        rfbDes((unsigned char *)input, output, schedule);
        if (memcmp(output, worker->expected, sizeof(output)) != 0)
            worker->failed = 1;
        pthread_barrier_wait(worker->barrier);
    }
    return NULL;
}

int main(void)
{
    static const unsigned char key_two[8] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe};
    unsigned long schedule[32];
    unsigned char encrypted[8], restored[8];
    pthread_barrier_t barrier;
    pthread_t threads[2];
    struct worker workers[2] = {0};
    int out_len;

    if (!encrypt_rfbdes(encrypted, &out_len, key_one, input, sizeof(input)) ||
        out_len != (int)sizeof(input) ||
        memcmp(encrypted, expected_one, sizeof(encrypted)) != 0)
        return 1;
    if (!decrypt_rfbdes(restored, &out_len, key_one, encrypted, sizeof(encrypted)) ||
        out_len != (int)sizeof(encrypted) ||
        memcmp(restored, input, sizeof(restored)) != 0)
        return 2;

    memcpy(workers[0].key, key_one, sizeof(key_one));
    memcpy(workers[0].expected, expected_one, sizeof(expected_one));
    memcpy(workers[1].key, key_two, sizeof(key_two));
    rfbDesKey(workers[1].key, EN0, schedule);
    rfbDes((unsigned char *)input, workers[1].expected, schedule);
    if (memcmp(workers[0].expected, workers[1].expected, sizeof(expected_one)) == 0)
        return 3;

    if (pthread_barrier_init(&barrier, NULL, 2) != 0)
        return 4;
    workers[0].barrier = workers[1].barrier = &barrier;
    if (pthread_create(&threads[0], NULL, encrypt_concurrently, &workers[0]) != 0)
        return 5;
    if (pthread_create(&threads[1], NULL, encrypt_concurrently, &workers[1]) != 0)
        return 6;
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
    pthread_barrier_destroy(&barrier);
    if (workers[0].failed || workers[1].failed)
        return 7;
    puts("DES vector, round-trip, and concurrent key isolation passed");
    return 0;
}
