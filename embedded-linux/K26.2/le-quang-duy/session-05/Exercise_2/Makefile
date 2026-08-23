CC = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -pthread -g

all: print_queue

print_queue: print_queue.c
	$(CC) $(CFLAGS) -o print_queue print_queue.c

clean:
	rm -f print_queue