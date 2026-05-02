#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>

const int BUFF_SZ = sizeof(int)*2;
int shm_key;
int shm_id;

// Define PERMS
#define PERMS 0700

// Define message buffer structure for message queue communication
typedef struct msgbuffer {
    long mtype;
    int intData;
} msgbuffer;

int main(int argc, char *argv[]) {
    msgbuffer buf;
    buf.mtype = 1; // Set message type to 1 for worker messages
    int msqid = 0;
    key_t msg_key;

    // Get a key for our message queue
    if ((msg_key = ftok("msgq.txt", 1)) == -1) {
        perror("ftok");
        exit(1);
    }
    
    // Create message queue
    if ((msqid = msgget(msg_key, PERMS)) == -1) {
        perror("msgget in child");
        exit(1);
    }

    printf("Worker %d has access to the queue \n", getpid());

    if (argc != 3) {
        fprintf(stderr, "Usage: worker sec nonosec\n");
        exit(1);
    }
    
    int run_sec = atoi(argv[1]);
    int run_nano = atoi(argv[2]);
    
    
    // Shared memory
    shm_key = ftok("oss.c", 'R');
    if (shm_key <= 0 ) {
        fprintf(stderr,"Child:... Error in ftok\n");
        exit(1);
    }
    
    // Create shared memory segment
    shm_id = shmget(shm_key,BUFF_SZ, PERMS);
    if (shm_id <= 0 ) {
        fprintf(stderr,"child:... Error in shmget\n");
        exit(1);
    }
    
    // Attach to the shared memory segment
    int *clock = (int *)shmat(shm_id,0,0);
    if (clock == (void *) -1) {
        fprintf(stderr,"Child:... Error in shmat\n");
        exit(1);
    }
    
    // Access the shared memory
    int *sec = &(clock[0]);
    int *nano = &(clock[1]);

    int start_sec = *sec;
    int start_nano = *nano;

    int term_sec = start_sec + run_sec;
    int term_nano = start_nano + run_nano;

    if (term_nano >= 1000000000) {
        term_sec++;
        term_nano -= 1000000000;
    }

    printf("WORKER PID:%d PPID:%d\n", getpid(), getppid());
    printf("SysClockS:%d SysClockNano:%d TermTimeS:%d TermTimeNano:%d\n", start_sec, start_nano, term_sec, term_nano);
    printf("--Just Starting\n");

    int last_printed_sec = start_sec;

    int messages_recieved = 0;

    // Main loop to check time and print status
    while (1) {
        // Wait for a message from the parent befor checking time and printing status
        if (msgrcv(msqid, &buf, sizeof(msgbuffer)-sizeof(long), getpid(), 0) == -1) {
            perror("Failed to recieve message from parent\n");
            exit(1);
        }
        
        messages_recieved++;

        // Check the clock against the termination time
        if (*sec > term_sec || (*sec == term_sec && *nano >= term_nano)) {
            printf("WORKER PID:%d PPID:%d\n", getpid(), getppid());
            printf("SysClockS:%d SysClockNano:%d TermTimeS:%d TermTimeNano:%d\n", *sec, *nano, term_sec, term_nano);
            printf("--Terminating after %d recieved messages.\n", messages_recieved);
            buf.mtype = 1; // Set message type to 1 for worker messages
            buf.intData = 0; // Set intData to 0 to indicate termination to the parent
            
            // Send a message back to the parent  
            if (msgsnd(msqid, &buf, sizeof(msgbuffer)-sizeof(long), 0) == -1) {
                perror("Failed to send msgsnd to parent.\n");
                exit(1);
            }
            break;
        }

        // Periodically print status every second
        if (*sec > last_printed_sec) {
            printf("WORKER PID:%d PPID:%d\n", getpid(), getppid());
            printf("SysClockS:%d SysClockNano:%d TermTimeS:%d TermTimeNano:%d\n", *sec, *nano, term_sec, term_nano);
            printf("--%d messages recieved from oss\n", messages_recieved);
            last_printed_sec = *sec;
        }

        buf.mtype = 1; // Set message type to 1 for worker messages
        buf.intData = 1; // Set intData to 1 to indicate still running to the parent

        // Send a message back to the parent  
        if (msgsnd(msqid, &buf, sizeof(msgbuffer)-sizeof(long), 0) == -1) {
            perror("Failed to send msgsnd to parent.\n");
            exit(1);
        }
    }

    shmdt(clock);
    return 0;
}