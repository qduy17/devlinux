#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

// Loop control flag, using sig_atomic_t for thread safety
volatile sig_atomic_t keep_running = 1;

// SIGTERM handler called when systemd issues 'systemctl stop'
void handle_sigterm(int signum) {
    (void)signum; // Suppress unused variable warning
    keep_running = 0;
}

int main() {
    // Disable stdout buffering to write logs to journal immediately
    setbuf(stdout, NULL);

    // Register SIGTERM signal handler
    struct sigaction action;
    action.sa_handler = handle_sigterm;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGTERM, &action, NULL);

    int counter = 1;
    
    // Infinite loop runs until keep_running becomes 0
    while (keep_running) {
        printf("Monitor service is running (tick %d)...\n", counter++);
        sleep(1);
    }

    // Print message before clean exit
    printf("Service shutting down...\n");
    return 0;
}