#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define LOG_ERR     "<3>"
#define LOG_WARNING "<4>"
#define LOG_INFO    "<6>"

int main() {
    // Disable buffering for both stdout and stderr
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    int cycle = 1;
    
    // Run 15 cycles * 2 seconds = 30 seconds
    while (cycle <= 15) {
        fprintf(stderr, LOG_INFO    "Service running normally, cycle %d\n", cycle);
        fprintf(stderr, LOG_WARNING "Memory usage high: %d%%\n", 80 + (cycle % 15));
        fprintf(stderr, LOG_ERR     "Failed to connect to database, retry %d\n", cycle);
        
        sleep(2);
        cycle++;
    }

    // Simulate application crash
    fprintf(stderr, LOG_ERR "Fatal error encountered. Aborting...\n");
    abort(); 

    return 0;
}