CC = gcc
CFLAGS = -Wall -g

OSS = oss
WORKER = worker

all: $(OSS) $(WORKER)

$(OSS): oss.c
	$(CC) $(CFLAGS) oss.c -o $(OSS)

$(WORKER): worker.c
	$(CC) $(CFLAGS) worker.c -o $(WORKER)

clean:
	rm -f $(OSS) $(WORKER) *.o