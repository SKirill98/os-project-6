/*
 * oss.c - Operating System Simulator with FIFO page replacement
 * Author: kslab
 * Date:   2026-05-04
 *
 * Manages a 64-frame physical memory (64K / 1K page size).
 * Each of up to 20 worker processes has a 16-entry page table.
 * Page faults are resolved with FIFO replacement; dirty evictions
 * add an extra 14 ms of simulated I/O time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/msg.h>
#include "shared.h"

/* -- IPC handles (global for signal cleanup) --------------------------- */
static int    shm_id;
static int   *clockptr = NULL;
static int    msqid;
static FILE  *log_file = NULL;

/* -- System tables ----------------------------------------------------- */
static FrameEntry  frame_table[NUM_FRAMES];
static struct PCB  table[MAX_PCB];

/*
 * FIFO queue for page replacement.
 * Each entry is the frame index of a page that was loaded.
 * Stale entries (freed frames) are skipped during eviction.
 */
#define FIFO_CAP (NUM_FRAMES * 200)
static int fifo_queue[FIFO_CAP];
static int fifo_head = 0;   /* index of oldest entry  */
static int fifo_tail = 0;   /* index of next free slot */

/* -- Statistics -------------------------------------------------------- */
static int total_reads       = 0;
static int total_writes      = 0;
static int total_page_faults = 0;

/* -----------------------------------------------------------------------
 * Utility helpers
 * ----------------------------------------------------------------------- */

static void cleanup(int sig) {
    if (sig != 0) {
        printf("\nOSS: Caught signal %d. Cleaning up...\n", sig);
        signal(SIGTERM, SIG_IGN);
        kill(0, SIGTERM);
    }
    if (clockptr) shmdt(clockptr);
    shmctl(shm_id, IPC_RMID, NULL);
    msgctl(msqid, IPC_RMID, NULL);
    unlink("msgq.txt");
    if (log_file) fclose(log_file);
    exit(sig == 0 ? 0 : 1);
}

static void log_msg(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    if (log_file) {
        va_start(args, fmt);
        vfprintf(log_file, fmt, args);
        va_end(args);
        fflush(log_file);
    }
}

/* Add delta_ns nanoseconds to the shared simulated clock */
static void advance_clock(int *sec, int *nano, int delta_ns) {
    *nano += delta_ns;
    while (*nano >= BILLION) { (*sec)++; *nano -= BILLION; }
}

/* -----------------------------------------------------------------------
 * Frame / page-table management
 * ----------------------------------------------------------------------- */

/* Returns a free frame index, or -1 if all frames are occupied */
static int find_free_frame(void) {
    for (int i = 0; i < NUM_FRAMES; i++)
        if (!frame_table[i].occupied) return i;
    return -1;
}

/*
 * FIFO eviction: dequeue from the head, skipping any frames that have
 * since been freed (process terminated before they were evicted).
 * Falls back to a linear scan if the queue is exhausted somehow.
 */
static int fifo_evict(void) {
    while (fifo_head != fifo_tail) {
        int frame = fifo_queue[fifo_head % FIFO_CAP];
        fifo_head++;
        if (frame_table[frame].occupied) return frame;
    }
    /* Fallback: should not normally be reached */
    for (int i = 0; i < NUM_FRAMES; i++)
        if (frame_table[i].occupied) return i;
    return -1;
}

/*
 * Load process pcb_idx's logical page into physical frame.
 * Caller is responsible for clearing the old occupant first.
 */
static void load_page(int frame, int pcb_idx, int page) {
    frame_table[frame].occupied = 1;
    frame_table[frame].dirty    = 0;
    frame_table[frame].process  = pcb_idx;
    frame_table[frame].page     = page;
    table[pcb_idx].page_table[page] = frame;

    /* Record in FIFO */
    fifo_queue[fifo_tail % FIFO_CAP] = frame;
    fifo_tail++;
}

/* Release all frames owned by pcb_idx when the process terminates */
static void free_process_frames(int pcb_idx) {
    for (int i = 0; i < NUM_FRAMES; i++) {
        if (frame_table[i].occupied && frame_table[i].process == pcb_idx) {
            frame_table[i].occupied = 0;
            frame_table[i].dirty    = 0;
            frame_table[i].process  = -1;
            frame_table[i].page     = -1;
        }
    }
    for (int p = 0; p < NUM_PAGES; p++)
        table[pcb_idx].page_table[p] = -1;
}

/* -----------------------------------------------------------------------
 * Output helpers
 * ----------------------------------------------------------------------- */

static void print_memory_map(int sec, int nano) {
    log_msg("\nOSS: Current memory layout at time %d:%09d\n", sec, nano);
    log_msg("%-10s %-10s %-10s %-10s %-6s\n",
            "Frame", "Occupied", "DirtyBit", "Process", "Page");
    for (int i = 0; i < NUM_FRAMES; i++) {
        log_msg("Frame %-4d %-10s %-10d %-10d %-6d\n",
                i,
                frame_table[i].occupied ? "Yes" : "No",
                frame_table[i].dirty,
                frame_table[i].process,
                frame_table[i].page);
    }
    log_msg("\n");
    for (int i = 0; i < MAX_PCB; i++) {
        if (!table[i].occupied) continue;
        log_msg("P%d page table: [ ", i);
        for (int p = 0; p < NUM_PAGES; p++)
            log_msg("%d ", table[i].page_table[p]);
        log_msg("]\n");
    }
    log_msg("\n");
}

/* -----------------------------------------------------------------------
 * Memory request handler
 * Returns 1 on page fault (process blocked), 0 on page hit (grant sent).
 * ----------------------------------------------------------------------- */
static int handle_memory_request(int pcb_idx, int address, int is_write,
                                 int *sec, int *nano) {
    int page  = address / PAGE_SIZE;
    int frame = table[pcb_idx].page_table[page];

    if (is_write) total_writes++; else total_reads++;

    if (frame != -1) {
        /* -- Page hit ----------------------------------------------- */
        if (is_write) {
            frame_table[frame].dirty = 1;
            log_msg("OSS: Address %d in frame %d, writing data to frame at time %d:%09d\n",
                    address, frame, *sec, *nano);
        } else {
            log_msg("OSS: Address %d in frame %d, giving data to P%d at time %d:%09d\n",
                    address, frame, pcb_idx, *sec, *nano);
        }
        advance_clock(sec, nano, 100); /* 100 ns memory access */
        msgbuffer grant = { .mtype = table[pcb_idx].pid, .intData = 1 };
        msgsnd(msqid, &grant, sizeof(msgbuffer) - sizeof(long), 0);
        return 0;
    }

    /* -- Page fault ------------------------------------------------- */
    total_page_faults++;
    log_msg("OSS: Address %d is not in a frame, pagefault for P%d page %d at time %d:%09d\n",
            address, pcb_idx, page, *sec, *nano);

    int extra_ns    = 0;
    int target_frame = find_free_frame();

    if (target_frame == -1) {
        /* FIFO: evict the oldest occupied frame */
        target_frame = fifo_evict();
        int old_proc = frame_table[target_frame].process;
        int old_page = frame_table[target_frame].page;

        log_msg("OSS: Clearing frame %d and swapping in P%d page %d\n",
                target_frame, pcb_idx, page);

        if (frame_table[target_frame].dirty) {
            log_msg("OSS: Dirty bit of frame %d set, adding additional time to the clock\n",
                    target_frame);
            extra_ns = DISK_IO_NS; /* extra write-back cost */
        }

        /* Invalidate the evicted page in its owner's page table */
        if (old_proc >= 0 && table[old_proc].occupied)
            table[old_proc].page_table[old_page] = -1;

        /* Clear the frame before reuse */
        frame_table[target_frame].occupied = 0;
    } else {
        log_msg("OSS: Swapping in P%d page %d into free frame %d\n",
                pcb_idx, page, target_frame);
    }

    load_page(target_frame, pcb_idx, page);
    if (is_write) frame_table[target_frame].dirty = 1;

    /* Block the process until disk I/O finishes */
    int ub_nano = *nano + DISK_IO_NS + extra_ns;
    int ub_sec  = *sec;
    while (ub_nano >= BILLION) { ub_sec++; ub_nano -= BILLION; }

    table[pcb_idx].blocked          = 1;
    table[pcb_idx].blocked_address  = address;
    table[pcb_idx].blocked_is_write = is_write;
    table[pcb_idx].unblock_sec      = ub_sec;
    table[pcb_idx].unblock_nano     = ub_nano;

    return 1;
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    srand(time(NULL));
    int opt;
    int    n     = -1, s = -1;
    double t     = -1, i_val = -1;

    while ((opt = getopt(argc, argv, "hn:s:t:i:f:")) != -1) {
        switch (opt) {
        case 'h':
            printf("Usage: %s [-h] [-n proc] [-s simul] [-t timeLimitForChildren]"
                   " [-i intervalInSecondsToLaunchChildren] [-f logfile]\n", argv[0]);
            return 0;
        case 'n': n     = atoi(optarg); break;
        case 's': s     = atoi(optarg); break;
        case 't': t     = atof(optarg); break;
        case 'i': i_val = atof(optarg); break;
        case 'f':
            log_file = fopen(optarg, "w");
            if (!log_file) { perror("OSS: fopen log"); return 1; }
            break;
        default:
            fprintf(stderr, "Usage: %s [-h] [-n proc] [-s simul] [-t timeLimitForChildren]"
                    " [-i intervalInSecondsToLaunchChildren] [-f logfile]\n", argv[0]);
            return 1;
        }
    }

    if (n <= 0 || s <= 0 || t <= 0 || i_val < 0) {
        fprintf(stderr, "OSS: Missing or invalid required arguments\n");
        return 1;
    }

    signal(SIGINT,  cleanup);
    signal(SIGALRM, cleanup);
    alarm(60); /* 60 real-time second safety net */

    /* -- Shared memory (simulated clock: two ints) ---------------- */
    int shm_key = ftok("oss.c", 'R');
    if (shm_key <= 0) { fprintf(stderr, "OSS: ftok shm failed\n"); return 1; }
    shm_id = shmget(shm_key, sizeof(int) * 2, PERMS | IPC_CREAT);
    if (shm_id <= 0) { fprintf(stderr, "OSS: shmget failed\n"); return 1; }
    clockptr = (int *)shmat(shm_id, NULL, 0);
    if (clockptr == (void *)-1) { fprintf(stderr, "OSS: shmat failed\n"); return 1; }
    int *sec  = &clockptr[0];
    int *nano = &clockptr[1];
    *sec = 0; *nano = 0;

    /* -- Message queue -------------------------------------------- */
    FILE *tmp = fopen("msgq.txt", "w");
    if (!tmp) { perror("OSS: fopen msgq.txt"); exit(1); }
    fclose(tmp);
    key_t msg_key = ftok("msgq.txt", 1);
    if (msg_key == -1) { perror("OSS: ftok msgq"); exit(1); }
    if ((msqid = msgget(msg_key, PERMS | IPC_CREAT)) == -1) {
        perror("OSS: msgget"); exit(1);
    }

    /* -- Initialize tables ---------------------------------------- */
    for (int j = 0; j < NUM_FRAMES; j++) {
        frame_table[j] = (FrameEntry){ .occupied = 0, .dirty = 0,
                                       .process  = -1, .page = -1 };
    }
    memset(table, 0, sizeof(table));
    for (int j = 0; j < MAX_PCB; j++)
        for (int p = 0; p < NUM_PAGES; p++)
            table[j].page_table[p] = -1;

    log_msg("OSS: Starting, PID:%d\n", getpid());
    log_msg("OSS: -n %d -s %d -t %.3f -i %.3f\n\n", n, s, t, i_val);

    int running        = 0;
    int total_launched = 0;
    int last_launch_sec  = 0, last_launch_nano  = 0;
    int last_print_sec   = -1, last_print_nano   = -1;
    int current = -1;
    msgbuffer msg;

    while (total_launched < n || running > 0) {

        /* -- 1. Advance simulated clock by 10 ms ------------------- */
        advance_clock(sec, nano, 10000000);

        /* -- 2. Soft-deadlock prevention: if every running process
                is blocked, jump the clock to the earliest unblock
                time so the simulation doesn't spin forever.          */
        if (running > 0) {
            bool all_blocked = true;
            bool found_blocked = false;
            int  earliest_sec = 0, earliest_nano = 0;

            for (int j = 0; j < MAX_PCB; j++) {
                if (!table[j].occupied) continue;
                if (!table[j].blocked) { all_blocked = false; break; }
                if (!found_blocked ||
                    table[j].unblock_sec < earliest_sec ||
                    (table[j].unblock_sec == earliest_sec &&
                     table[j].unblock_nano < earliest_nano)) {
                    earliest_sec  = table[j].unblock_sec;
                    earliest_nano = table[j].unblock_nano;
                    found_blocked = true;
                }
            }

            if (all_blocked && found_blocked) {
                if (earliest_sec > *sec ||
                    (earliest_sec == *sec && earliest_nano > *nano)) {
                    *sec  = earliest_sec;
                    *nano = earliest_nano;
                }
            }
        }

        /* -- 3. Unblock processes whose I/O wait has elapsed -------- */
        for (int j = 0; j < MAX_PCB; j++) {
            if (!table[j].occupied || !table[j].blocked) continue;
            if (*sec > table[j].unblock_sec ||
                (*sec == table[j].unblock_sec && *nano >= table[j].unblock_nano)) {
                log_msg("OSS: Indicating to P%d that %s has happened to address %d"
                        " at time %d:%09d\n",
                        j,
                        table[j].blocked_is_write ? "write" : "read",
                        table[j].blocked_address,
                        *sec, *nano);
                table[j].blocked = 0;
                msgbuffer grant = { .mtype = table[j].pid, .intData = 1 };
                msgsnd(msqid, &grant, sizeof(msgbuffer) - sizeof(long), 0);
            }
        }

        /* -- 4. Launch a new worker if conditions are met ----------- */
        {
            int diff_sec  = *sec  - last_launch_sec;
            int diff_nano = *nano - last_launch_nano;
            if (diff_nano < 0) { diff_sec--; diff_nano += BILLION; }

            bool interval_ok = (total_launched == 0) ||
                (diff_sec  >  (int)i_val) ||
                (diff_sec  == (int)i_val &&
                 diff_nano >= (int)((i_val - (int)i_val) * BILLION));

            if (running < s && total_launched < n &&
                running < 18 && interval_ok) {

                int index = -1;
                for (int j = 0; j < MAX_PCB; j++)
                    if (!table[j].occupied) { index = j; break; }

                if (index != -1) {
                    double rr = ((double)rand() / RAND_MAX) * t;
                    if (rr == 0) rr = 0.000001;
                    int run_sec  = (int)rr;
                    int run_nano = (int)((rr - run_sec) * BILLION);
                    int term_sec  = *sec  + run_sec;
                    int term_nano = *nano + run_nano;
                    if (term_nano >= BILLION) { term_sec++; term_nano -= BILLION; }

                    pid_t child = fork();
                    if (child == 0) {
                        char ss[20], ns[20];
                        sprintf(ss, "%d", term_sec);
                        sprintf(ns, "%d", term_nano);
                        execl("./worker", "worker", ss, ns, NULL);
                        perror("OSS: execl failed");
                        exit(1);
                    }

                    table[index].occupied      = 1;
                    table[index].pid           = child;
                    table[index].start_sec     = *sec;
                    table[index].start_nanosec = *nano;
                    table[index].end_sec       = term_sec;
                    table[index].end_nano      = term_nano;
                    table[index].blocked       = 0;
                    for (int p = 0; p < NUM_PAGES; p++)
                        table[index].page_table[p] = -1;

                    running++;
                    total_launched++;
                    last_launch_sec  = *sec;
                    last_launch_nano = *nano;
                    log_msg("OSS: Launching P%d (PID %d) at time %d:%09d\n",
                            index, child, *sec, *nano);
                }
            }
        }

        /* -- 5. Round-robin: pick the next unblocked process ------- */
        int found = -1;
        for (int k = 0; k < MAX_PCB; k++) {
            current = (current + 1) % MAX_PCB;
            if (table[current].occupied && !table[current].blocked) {
                found = current;
                break;
            }
        }

        if (found != -1) {
            advance_clock(sec, nano, 100); /* small tick before sending */

            /* Send turn token */
            msg.mtype   = table[found].pid;
            msg.intData = 1;
            msgsnd(msqid, &msg, sizeof(msgbuffer) - sizeof(long), 0);

            /* Receive response */
            if (msgrcv(msqid, &msg, sizeof(msgbuffer) - sizeof(long), 1, 0) != -1) {

                if (msg.intData == 0) {
                    /* Process terminating */
                    log_msg("OSS: P%d terminating at time %d:%09d\n",
                            found, *sec, *nano);
                    free_process_frames(found);
                    waitpid(table[found].pid, NULL, 0);
                    memset(&table[found], 0, sizeof(struct PCB));
                    for (int p = 0; p < NUM_PAGES; p++)
                        table[found].page_table[p] = -1;
                    running--;

                } else {
                    /* Memory request: decode address and direction */
                    int raw      = msg.intData;
                    int is_write = (raw < 0);
                    int address  = (is_write ? -raw : raw) - 1;

                    log_msg("OSS: P%d requesting %s of address %d at time %d:%09d\n",
                            found, is_write ? "write" : "read",
                            address, *sec, *nano);

                    handle_memory_request(found, address, is_write, sec, nano);
                }
            }
        }

        /* -- 6. Print memory map + blocked list every 0.5 sim seconds */
        if (*sec > last_print_sec ||
            (*sec == last_print_sec && *nano >= last_print_nano + 500000000)) {
            last_print_sec  = *sec;
            last_print_nano = *nano;

            print_memory_map(*sec, *nano);

            log_msg("OSS: Blocked processes: ");
            bool any = false;
            for (int j = 0; j < MAX_PCB; j++) {
                if (table[j].occupied && table[j].blocked) {
                    log_msg("P%d ", j);
                    any = true;
                }
            }
            if (!any) log_msg("none");
            log_msg("\n");
        }
    }

    /* -- Final statistics -------------------------------------------- */
    int total_accesses = total_reads + total_writes;
    log_msg("\nOSS: Final Report:\n");
    log_msg("  Total reads:       %d\n", total_reads);
    log_msg("  Total writes:      %d\n", total_writes);
    log_msg("  Total page faults: %d\n", total_page_faults);
    log_msg("  Page fault rate:   %.2f%%\n",
            total_accesses > 0
            ? (float)total_page_faults / total_accesses * 100.0f
            : 0.0f);

    cleanup(0);
    return 0;
}