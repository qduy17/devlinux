CC = gcc
CFLAGS = -Wall -pthread

all: print_queue

print_queue: print_queue.c
	$(CC) $(CFLAGS) -o print_queue print_queue.c

clean:
	rm -f print_queue