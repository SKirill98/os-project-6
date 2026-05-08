/*
Shared definitions for oss and worker (memory management simulation)
Author: Kirill Slabun
Date:   2026-05-04
*/

#ifndef SHARED_H
#define SHARED_H

#include <sys/types.h>

#define NUM_PAGES    16          // logical pages per process
#define PAGE_SIZE    1024        // bytes per page
#define NUM_FRAMES   64          // total physical frames (64K / 1K)
#define MAX_PCB      20          // maximum concurrent processes
#define PERMS        0700
#define BILLION      1000000000
#define DISK_IO_NS   14000000    // 14 ms disk I/O latency in nanoseconds

/*
Message encoding (intData field):
0        -> worker is terminating
positive -> read  request: address = intData - 1
negative -> write request: address = (-intData) - 1
*/
typedef struct {
    long mtype;
    int  intData;
} msgbuffer;

// One entry in the system frame table
typedef struct {
    int occupied;  // 1 = frame is in use
    int dirty;     // 1 = frame has been written to
    int process;   // PCB index of owning process, -1 = free
    int page;      // which page of that process, -1 = free
} FrameEntry;

// Per-process control block
struct PCB {
    int   occupied;
    pid_t pid;
    int   start_sec;
    int   start_nanosec;
    int   end_sec;
    int   end_nano;

    // Page-fault blocking state
    int   blocked;          // 1 if waiting for disk I/O to complete
    int   blocked_address;  // address that caused the fault
    int   blocked_is_write; // 1 if the pending request is a write
    int   unblock_sec;      // simulated time when I/O completes
    int   unblock_nano;

    // Page table: page_table[p] = frame index, or -1 if not in memory
    int   page_table[NUM_PAGES];

    int   total_accesses; // memory accesses made (reads + writes), used for EMAT
};

#endif