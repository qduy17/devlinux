#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

#define QUEUE_SIZE 5

/*
 * THEORY QUESTION:
 * 1. Why must pthread_cond_wait() be inside a while loop rather than an if?
 * 2. What is a spurious wakeup?
 *
 * ANSWER:
 * 1. Threads must re-check the condition after waking up. If multiple threads 
 *    are awakened simultaneously (e.g., via broadcast) or another thread 
 *    intervenes and alters the state before the awakened thread secures the 
 *    mutex, the condition might no longer hold true. The `while` loop ensures 
 *    the thread only proceeds when the condition is genuinely met.
 * 2. A "spurious wakeup" occurs when a thread blocked on a condition variable 
 *    is awakened without the condition variable actually being signaled or 
 *    broadcasted by another thread (often due to underlying OS mechanics). 
 *    The `while` loop safely catches these by forcing the thread back to sleep 
 *    if the logical condition isn't met.
 */

typedef struct {
    int  doc_id;
    char filename[60];
    int  pages;
} Document;

Document queue[QUEUE_SIZE];
int head = 0, tail = 0, count = 0;
int all_sent = 0;

int total_submitted = 0;
int total_printed = 0;
int total_pages = 0;

pthread_mutex_t q_lock;
pthread_cond_t  not_full;
pthread_cond_t  not_empty;

// Producer arguments holding the documents they are responsible for
typedef struct {
    int producer_id;
    Document docs[3];
} ProducerArgs;

void enqueue(Document doc) {
    queue[tail] = doc;
    tail = (tail + 1) % QUEUE_SIZE;
    count++;
    total_submitted++;
}

Document dequeue() {
    Document doc = queue[head];
    head = (head + 1) % QUEUE_SIZE;
    count--;
    total_printed++;
    total_pages += doc.pages;
    return doc;
}

void* producer(void* arg) {
    ProducerArgs* p_args = (ProducerArgs*)arg;
    
    for (int i = 0; i < 3; i++) {
        pthread_mutex_lock(&q_lock);
        
        while (count == QUEUE_SIZE) {
            printf("[Producer %d] Queue full — waiting...\n", p_args->producer_id);
            pthread_cond_wait(&not_full, &q_lock);
        }
        
        enqueue(p_args->docs[i]);
        printf("[Producer %d] Submitting: %-19s (%2d pages) — queue: %d/%d\n", 
               p_args->producer_id, p_args->docs[i].filename, p_args->docs[i].pages, count, QUEUE_SIZE);
               
        pthread_cond_signal(&not_empty);
        pthread_mutex_unlock(&q_lock);
        
        usleep(100000); // small delay to encourage thread interleaving
    }
    pthread_exit(NULL);
}

void* printer(void* arg) {
    while (1) {
        pthread_mutex_lock(&q_lock);
        
        while (count == 0 && !all_sent) {
            pthread_cond_wait(&not_empty, &q_lock);
        }
        
        // If queue is empty and producers are totally done, time to exit
        if (count == 0 && all_sent) {
            pthread_mutex_unlock(&q_lock);
            break;
        }
        
        Document doc = dequeue();
        printf("[Printer]    Printing:   %-19s (%2d pages) — queue: %d/%d\n", 
               doc.filename, doc.pages, count, QUEUE_SIZE);
               
        pthread_cond_signal(&not_full);
        pthread_mutex_unlock(&q_lock);
        
        sleep(1); // Simulate time taken to print the document
    }
    
    printf("[Printer]    All documents printed. Exiting.\n");
    pthread_exit(NULL);
}

int main() {
    pthread_t producers[3], printer_thread;
    
    printf("==============================================\n");
    printf("   OFFICE PRINT QUEUE (3 producers, 1 printer)\n");
    printf("   Queue capacity: %d documents                \n", QUEUE_SIZE);
    printf("==============================================\n\n");
    
    pthread_mutex_init(&q_lock, NULL);
    pthread_cond_init(&not_full, NULL);
    pthread_cond_init(&not_empty, NULL);
    
    ProducerArgs p_args[3] = {
        {1, {{1, "report_Q1.pdf", 12}, {4, "slides.pdf", 20}, {7, "summary.pdf", 4}}},
        {2, {{2, "contract.pdf", 5},   {5, "memo.pdf", 2},    {8, "budget.pdf", 7}}},
        {3, {{3, "invoice.pdf", 3},    {6, "proposal.pdf", 8}, {9, "report_2nd_copy.pdf", 5}}}
    };
    
    // Start printer
    pthread_create(&printer_thread, NULL, printer, NULL);
    
    // Start producers
    for (int i = 0; i < 3; i++) {
        pthread_create(&producers[i], NULL, producer, &p_args[i]);
    }
    
    // Wait for all producers to finish submitting
    for (int i = 0; i < 3; i++) {
        pthread_join(producers[i], NULL);
    }
    
    // Signal printer that no more documents will arrive
    pthread_mutex_lock(&q_lock);
    all_sent = 1;
    pthread_cond_broadcast(&not_empty); 
    pthread_mutex_unlock(&q_lock);
    
    // Wait for printer to finish printing the remaining queue
    pthread_join(printer_thread, NULL);
    
    printf("\n================ SUMMARY ================\n");
    printf("  Documents submitted : %d\n", total_submitted);
    printf("  Documents printed   : %d\n", total_printed);
    printf("  Total pages printed : %d\n", total_pages);
    printf("=========================================\n");
    
    pthread_mutex_destroy(&q_lock);
    pthread_cond_destroy(&not_full);
    pthread_cond_destroy(&not_empty);
    
    return 0;
}