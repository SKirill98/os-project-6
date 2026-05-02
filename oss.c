#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <time.h> // For random runtime generation
#include <errno.h>
#include <sys/msg.h>

#define PERMS 0700
#define MAX_PCB 20
#define BILLION 1000000000

// Global variables for signal cleanup
const int BUFF_SIZE = sizeof(int) * 2;
int shm_key;
int shm_id;
int *clockptr;

// Cleanup handler for SIGINT and SIGALRM
void cleanup(int sig) {
    printf("\nOSS: Caught signal %d. Cleaning up and terminating...\n", sig);

    // Kill all children
    kill(0, SIGTERM);

    // Detach and remove shared memory
    if (clockptr != NULL)
        shmdt(clockptr);

    shmctl(shm_id, IPC_RMID, NULL);

    exit(1);
}

// Message buffer structure for message queue communication
typedef struct msgbuffer {
    long mtype;
    int intData;
} msgbuffer;

int main(int argc, char *argv[]) {

    srand(time(NULL)); // Seed with time and PID
    int opt;
    int n = -1;          // total processes to launch
    int s = -1;          // max simultaneous
    double t = -1;       // child runtime (float seconds)
    double i = -1;       // interval between launches (float seconds)
    FILE *f = NULL;    // log file pointer
    // Create a file for ftok to use for message queue key generation
    FILE *tmp = fopen("msgq.txt", "a");
    if (tmp == NULL) {
        perror("fopen msgq.txt");
        exit(1);
    }
    fclose(tmp);
    int msqid;
    key_t msg_key;

    // get a key for our message queue
    if ((msg_key = ftok("msgq.txt", 1)) == -1) {
        perror("ftok");
        exit(1);
    }
    // Create message queue
    if ((msqid = msgget(msg_key, PERMS | IPC_CREAT)) == -1) {
        perror("msgget in parent");
        exit(1);
    }
    printf("Message queue created with ID: %d\n", msqid);

    // Parse command line arguments
    while ((opt = getopt(argc, argv, "hn:s:t:i:f:")) != -1) {
        switch (opt) {
            case 'h':
                printf("Usage: %s [-h] [-n proc] [-s simul] [-t timelimitForChildren] [-i intervalInSecondsToLaunchChildren] [-f logfile]\n", argv[0]);
                printf("  -h Display help message and exit\n");
                printf("  -n proc   Total number of child processes to launch\n");
                printf("  -s simul  Maximum number of children running simultaneously\n");
				        printf("  -t iter   Upper bound of simulated time each child runs\n");
                printf("  -i sec    Interval in simulated seconds to launch new children\n");
                printf("  -f file   Log output to specified file\n");
                return 0;
            case 'n':
                n = atoi(optarg);
                break;
            case 's':
                s = atoi(optarg);
                break;
            case 't':
                t = atof(optarg);
                break;
            case 'i':
                i = atof(optarg);
                break;
            case 'f':
                // log file
                f = fopen(optarg, "w");
                break;
            default:
                printf("Usage: %s [-h] [-n proc] [-s simul] [-t timelimitForChildren] [-i intervalInSecondsToLaunchChildren] [-f logfile]\n", argv[0]);
                printf("  -h Display help message and exit\n");
                printf("  -n proc   Total number of child processes to launch\n");
				        printf("  -s simul  Maximum number of children running simultaneously\n");
				        printf("  -t iter   Upper bound of simulated time each child runs\n");
                printf("  -i sec    Interval in simulated seconds to launch new children\n");
                printf("  -f file   Log output to specified file\n");
                return 1;
        }
    }

    if (n <= 0 || s <= 0 || t <= 0 || i < 0) {
        fprintf(stderr, "Missing or invalid required arguments\n");
        printf("Usage: %s [-h] [-n proc] [-s simul] [-t timelimitForChildren] [-i intervalInSecondsToLaunchChildren]\n", argv[0]);
        printf("-n value must be greater then 0. \n");
        printf("-s value must be greater then 0. \n");
        printf("-t value must be greater then 0. \n");
        printf("-i value must be greater or equal to 0. \n");
        return 1;
    }

    // Print initial configuration
    printf("OSS starting, PID:%d PPID:%d\n", getpid(), getppid());
    printf("Called with:\n-n %d\n-s %d\n-t %.3f\n-i %.3f\n\n", n, s, t, i);
    // Log initial configuration to file if specified
    fprintf(f, "OSS starting, PID:%d PPID:%d\n", getpid(), getppid());
    fprintf(f, "Called with:\n-n %d\n-s %d\n-t %.3f\n-i %.3f\n\n", n, s, t, i);

    // Setup signal handling
    signal(SIGINT, cleanup);
    signal(SIGALRM, cleanup);
    alarm(60);  // force terminate after 60 real seconds

    // Create shared memory
    shm_key = ftok("oss.c", 'R'); // Generate a unique key for shared memory
    if (shm_key <= 0) {
        fprintf(stderr, "Parent: Failed to generate shared memory key (ftok failed)\n");
        return 1;
    }

    shm_id = shmget(shm_key, BUFF_SIZE, PERMS|IPC_CREAT); // Create shared memory segment
    if (shm_id <= 0) {
        fprintf(stderr, "Parent: Failed to create shared memory segment (shmget failed)\n");
        return 1;
    }

    clockptr = (int *)shmat(shm_id, NULL, 0); // Attach to shared memory
    if (clockptr == (void *) -1) {
        fprintf(stderr, "Parent: Failed to attach to shared memory (shmat failed)\n");
        return 1;
    }

    // Initialize shared clock
    int *sec = &clockptr[0];
    int *nano = &clockptr[1];

    *sec = 0;
    *nano = 0;

    // Convert interval to sec/nano
    int interval_sec = (int)i;
    int interval_nano = (int)((i - interval_sec) * BILLION);

    // PCB structure
    struct PCB {
        int occupied; // 0 for free, 1 for occupied
        pid_t pid; // Process ID of this child
        int start_sec; // Start time in seconds
        int start_nanosec; // Start time in nanoseconds
        int end_sec; // Ending time in seconds
        int end_nano; // Ending time in nanoseconds
        int messages_sent; // Total times oss sent message to this child
    };

    struct PCB table[MAX_PCB];

    // Initialize table
    memset(table, 0, sizeof(table));

    int running = 0;
    int total_launched = 0;
    int total_messages_sent = 0;

    int last_launch_sec = 0;
    int last_launch_nano = 0;

    int current = -1;
    msgbuffer msg;

    // Initialize last print time to -1 to ensure first print at time 0
    int last_print_sec = -1;

    // Main scheduling loop
    while (total_launched < n || running > 0) {

        // Increment the clock by 250ms divided by the number of current children.
        *nano += 250000000 / (running > 0 ? running : 1);
        if (*nano >= BILLION) {
            (*sec)++;
            *nano -= BILLION;
        }

        // Print and log process table every 0.5 seconds
        if (*sec > last_print_sec) {
            last_print_sec = *sec;

            printf("\nOSS PID:%d SysClockS:%d SysClockNano:%d\n",
                   getpid(), *sec, *nano);
            fprintf(f, "\nOSS PID:%d SysClockS:%d SysClockNano:%d\n",
                   getpid(), *sec, *nano);

            printf("Entry Occupied PID StartS StartN EndS EndN MsgsSent\n");
            fprintf(f, "Entry Occupied PID StartS StartN EndS EndN MsgsSent\n");

            for (int j = 0; j < MAX_PCB; j++) {
                printf("%2d %8d %6d %6d %6d %6d %6d %6d\n",
                       j,
                       table[j].occupied,
                       table[j].pid,
                       table[j].start_sec,
                       table[j].start_nanosec,
                       table[j].end_sec,
                       table[j].end_nano,
                       table[j].messages_sent);
                fprintf(f, "%2d %8d %6d %6d %6d %6d %6d %6d\n",
                       j,
                       table[j].occupied,
                       table[j].pid,
                       table[j].start_sec,
                       table[j].start_nanosec,
                       table[j].end_sec,
                       table[j].end_nano,
                       table[j].messages_sent);
            }
        }

        // Check if enough interval time has passed
        int diff_sec = *sec - last_launch_sec;
        int diff_nano = *nano - last_launch_nano;

        if (diff_nano < 0) {
            diff_sec--;
            diff_nano += BILLION;
        }

        int enough_time = 0;
        if (diff_sec > interval_sec ||
           (diff_sec == interval_sec && diff_nano >= interval_nano)) {
            enough_time = 1;
        }

        // Launch new worker if allowed
        if (running < s && total_launched < n && (total_launched == 0 || enough_time)){

            int index = -1;
            for (int j = 0; j < MAX_PCB; j++) {
                if (!table[j].occupied) {
                    index = j;
                    break;
                }
            }

            if (index != -1) {

                // Generate random runtime for child between 1 second and t seconds
                double random_runtime = 1 + ((double)rand() / RAND_MAX) * (t - 1);

                // Convert runtime to sec/nano
                int run_sec = (int)random_runtime;
                int run_nano = (int)((random_runtime - run_sec) * BILLION);
                
                pid_t child = fork();

                if (child == 0) {
                    char secStr[20], nanoStr[20];
                    sprintf(secStr, "%d", run_sec);
                    sprintf(nanoStr, "%d", run_nano);

                    execl("./worker", "worker", secStr, nanoStr, NULL);
                    perror("execl failed");
                    exit(1);
                }

                if (child > 0) {

                    running++;
                    total_launched++;

                    last_launch_sec = *sec;
                    last_launch_nano = *nano;

                    table[index].occupied = 1;
                    table[index].pid = child;
                    table[index].start_sec = *sec;
                    table[index].start_nanosec = *nano;

                    table[index].end_sec = *sec + run_sec;
                    table[index].end_nano = *nano + run_nano;

                    if (table[index].end_nano >= BILLION) {
                        table[index].end_sec++;
                        table[index].end_nano -= BILLION;
                    }
                }
            }
        }

        // Choose next worker
        int found = 0;

        for(int k = 0; k < MAX_PCB; k++){
            current = (current + 1) % MAX_PCB;
            if(table[current].occupied){
                found = 1;
                break;
            }
        }

        if(!found){ 
            continue;
        }

        // Send message
        msg.mtype = table[current].pid;
        msg.intData = 1;

        if (msgsnd(msqid, &msg, sizeof(msgbuffer) - sizeof(long), 0) == -1) {
            perror("msgsnd failed");
            exit(1);
        }

        printf("OSS: Sending message to worker %d PID %d at %d:%d\n",
            current, table[current].pid, *sec, *nano);
        fprintf(f, "OSS: Sending message to worker %d PID %d at %d:%d\n",
            current, table[current].pid, *sec, *nano);
        
        table[current].messages_sent++;
        total_messages_sent++;

        // Recieve response
        if (msgrcv(msqid, &msg, sizeof(msgbuffer) - sizeof(long), 1, 0) == -1) {
            perror("OSS msgrcv failed");
            exit(1);
        }

        printf("OSS: Received message from worker %d PID %d\n",
            current, table[current].pid);
        fprintf(f, "OSS: Received message from worker %d PID %d\n",
            current, table[current].pid);
        
        // Terminating Child
        if(msg.intData == 0){
            printf("OSS: Worker %d terminating\n", table[current].pid);
            fprintf(f, "OSS: Worker %d terminating\n", table[current].pid);

            waitpid(table[current].pid, NULL, 0);

            table[current].occupied = 0;
            running--;
        }
    }

    // Final Report
    printf("\nOSS PID:%d Terminating\n", getpid());
    printf("%d workers were launched and terminated\n", total_launched);
    printf(" %d messages were sent to children\n", total_messages_sent);
    // Log final report to file
    fprintf(f, "\nOSS PID:%d Terminating\n", getpid());
    fprintf(f, "%d workers were launched and terminated\n", total_launched);
    fprintf(f, "%d messages were sent to children\n", total_messages_sent);

    // Cleanup
    shmdt(clockptr);
    shmctl(shm_id, IPC_RMID, NULL);
    if (f) {
        fclose(f); // Close log file
    }

    return 0;
}