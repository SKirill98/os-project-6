/*
Child process for memory management simulation
Author: Kirill Slabun
Date:   2026-05-04

Receives a termination time from oss, then loops sending random
memory read/write requests until the simulated clock expires.
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <time.h>
#include "shared.h"

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: worker term_sec term_nano\n");
        exit(1);
    }

    int term_sec  = atoi(argv[1]);
    int term_nano = atoi(argv[2]);
    srand(time(NULL) ^ (getpid() << 16));

    // Attach to the shared-memory clock
    int shm_key = ftok("oss.c", 'R');
    if (shm_key <= 0) { fprintf(stderr, "worker: ftok shm failed\n"); exit(1); }
    int shm_id = shmget(shm_key, sizeof(int) * 2, PERMS);
    if (shm_id <= 0) { fprintf(stderr, "worker: shmget failed\n"); exit(1); }
    int *clockptr = (int *)shmat(shm_id, NULL, 0);
    if (clockptr == (void *)-1) { fprintf(stderr, "worker: shmat failed\n"); exit(1); }
    int *sec  = &clockptr[0];
    int *nano = &clockptr[1];

    // Attach to the message queue
    key_t msg_key = ftok("msgq.txt", 1);
    if (msg_key == -1) { perror("worker: ftok msgq"); exit(1); }
    int msqid = msgget(msg_key, PERMS);
    if (msqid == -1) { perror("worker: msgget"); exit(1); }

    printf("WORKER PID:%d PPID:%d\n", getpid(), getppid());
    printf("SysClockS:%d SysClockNano:%d TermTimeS:%d TermTimeNano:%d\n", *sec, *nano, term_sec, term_nano);
    printf("--Just Starting\n");

    msgbuffer msg;

    while (1) {
        // Wait for turn token from oss
        if (msgrcv(msqid, &msg, sizeof(msgbuffer) - sizeof(long), getpid(), 0) == -1)
            break;

        // Check if simulated lifetime has expired
        if (*sec > term_sec || (*sec == term_sec && *nano >= term_nano)) {
            printf("WORKER PID:%d PPID:%d\n", getpid(), getppid());
            printf("SysClockS:%d SysClockNano:%d TermTimeS:%d TermTimeNano:%d\n", *sec, *nano, term_sec, term_nano);
            printf("--Terminating\n");
            msg.mtype    = 1;
            msg.intData  = 0; /* signal termination */
            msgsnd(msqid, &msg, sizeof(msgbuffer) - sizeof(long), 0);
            break;
        }

        // Generate a random logical address in [0, NUM_PAGES*PAGE_SIZE)
        int page    = rand() % NUM_PAGES;
        int offset  = rand() % PAGE_SIZE;
        int address = page * PAGE_SIZE + offset;

        // Bias toward reads: 20% write, 80% read
        int is_write = (rand() % 100) < 20;

        // Encode: positive = read (addr+1), negative = write (addr+1)
        msg.mtype   = 1;
        msg.intData = is_write ? -(address + 1) : (address + 1);
        msgsnd(msqid, &msg, sizeof(msgbuffer) - sizeof(long), 0);

        // Wait for grant from oss (may be delayed by a page fault)
        msgrcv(msqid, &msg, sizeof(msgbuffer) - sizeof(long), getpid(), 0);
    }

    shmdt(clockptr);
    return 0;
}
