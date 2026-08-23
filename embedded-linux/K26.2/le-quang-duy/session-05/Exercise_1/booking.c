#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

/*
 * THEORY QUESTION: 
 * Why must the check and the deduct be inside the SAME lock/unlock block?
 * 
 * ANSWER:
 * The check-and-deduct operation must be an "atomic" critical section. 
 * If you split them into two separate lock acquisitions (e.g., lock -> check -> 
 * unlock -> lock -> deduct -> unlock), another thread could acquire the lock 
 * in between your unlock and the second lock. That intervening thread might 
 * deduct the remaining seats, making the original thread's "check" invalid 
 * by the time it attempts to deduct. This leads to a race condition where 
 * more tickets could be sold than are actually available.
 */

typedef struct {
    int  agent_id;
    char customer[50];
    int  seats_wanted;
} BookingRequest;

BookingRequest requests[5] = {
    {1, "Nguyen Van An",  2},
    {2, "Tran Thi Bich",  1},
    {3, "Le Van Cuong",   3},
    {4, "Pham Thi Dung",  1},
    {5, "Hoang Van Em",   2}
};

int seats_available = 10;
int seats_sold = 0;
int failed_bookings = 0;
pthread_mutex_t seat_lock;

void* book_ticket(void* arg) {
    BookingRequest* req = (BookingRequest*)arg;
    
    // Print intent before sleeping to show threads starting concurrently
    printf("[Agent %d | TID %lu] Booking %d seat(s) for %s...\n", 
           req->agent_id, (unsigned long)pthread_self(), req->seats_wanted, req->customer);
           
    // Force real concurrency: all threads pause here, then race for the mutex
    sleep(1); 
    
    pthread_mutex_lock(&seat_lock);
    
    // Critical Section: Check AND Deduct
    if (seats_available >= req->seats_wanted) {
        seats_available -= req->seats_wanted;
        seats_sold += req->seats_wanted;
        printf("[Agent %d] CONFIRMED: %d seat(s) for %-15s Remaining: %d\n", 
               req->agent_id, req->seats_wanted, req->customer, seats_available);
    } else {
        failed_bookings++;
        printf("[Agent %d] SOLD OUT:  needs %d seats, only %d left — booking failed.\n", 
               req->agent_id, req->seats_wanted, seats_available);
    }
    
    pthread_mutex_unlock(&seat_lock);
    
    pthread_exit(NULL);
}

int main() {
    pthread_t agents[5];
    
    printf("==============================================\n");
    printf("   TICKET BOOKING SYSTEM (5 agents, 10 seats) \n");
    printf("==============================================\n");
    
    pthread_mutex_init(&seat_lock, NULL);
    
    for (int i = 0; i < 5; i++) {
        pthread_create(&agents[i], NULL, book_ticket, &requests[i]);
    }
    
    for (int i = 0; i < 5; i++) {
        pthread_join(agents[i], NULL);
    }
    
    printf("\n================ SUMMARY ================\n");
    printf("  Total seats     : 10\n");
    printf("  Seats sold      : %d\n", seats_sold);
    printf("  Seats remaining : %d\n", seats_available);
    printf("  Failed bookings : %d\n", failed_bookings);
    printf("=========================================\n");
    
    pthread_mutex_destroy(&seat_lock);
    return 0;
}