#ifndef SHARED_H
#define SHARED_H

#include <sys/types.h>

#define NUM_RESOURCES 10
#define INSTANCES_PER_RESOURCE 5
#define MAX_PCB 20
#define PERMS 0700
#define BILLION 1000000000

typedef struct msgbuffer {
    long mtype;
    int intData;
} msgbuffer;

struct PCB {
    int occupied; // 0 for free, 1 for occupied
    pid_t pid; // Process ID of this child
    int start_sec; // Start time in seconds
    int start_nanosec; // Start time in nanoseconds
    int end_sec; // End time in seconds
    int end_nano; // End time in nanoseconds
    int blocked; // Is this process waiting on event?
    int resources_allocated[NUM_RESOURCES]; // Number of each resource allocated to this process
    int requested_resource; // -1 if none, otherwise 0 to NUM_RESOURCES-1
};

#endif