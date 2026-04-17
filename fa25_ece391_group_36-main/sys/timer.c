// timer.c - A timer system
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//


#ifdef TIMER_TRACE
#define TRACE
#endif

#ifdef TIMER_DEBUG
#define DEBUG
#endif

#include "timer.h"
#include "thread.h"
#include "riscv.h"
#include "misc.h"
#include "intr.h"
#include "conf.h"
#include "see.h" // for set_stcmp
#include <stddef.h>

// EXPORTED GLOBAL VARIABLE DEFINITIONS
// 

char timer_initialized = 0;

// INTERNVAL GLOBAL VARIABLE DEFINITIONS
//

static struct alarm * sleep_list;

static uint64_t premptive_clock;
#define PREM_DUR (TIMER_FREQ / 100) //currently 10ms

// INTERNAL FUNCTION DECLARATIONS
//

// EXPORTED FUNCTION DEFINITIONS
//

void timer_init(void) {
    premptive_clock = rdtime() + PREM_DUR;
    set_stcmp(premptive_clock);
    timer_initialized = 1;
}

//void alarm_init(struct alarm * al, const char * name)
//Inputs: struct alarm * al - pointer to alarm struct
//        const char * name - name for alarm
//Outputs: none (void)
//Descriptions: inits alarm's condition var, sets name, resets its state
//Side Effects: sets twake to current time
void alarm_init(struct alarm * al, const char * name) {
    // FIXME your code goes here
    condition_init(&al->cond, name);
    al->next = NULL;
    al->twake = rdtime();
}

//void alarm_sleep(struct alarm * al, unsigned long long tcnt)
//Inputs: struct alarm * al - pointer to alarm struct
//        unsigned long long tcnt - 
//Outputs: none (void)
//Descriptions: puts current alarm to sleep , inserts alarm into sleep list sorted
//Side Effects: disables interrupts, updates timer compare if new earliest wakeup, puts thread to sleep 
void alarm_sleep(struct alarm * al, unsigned long long tcnt) {
    unsigned long long now;
    //struct alarm * prev;
    int pie;

    now = rdtime();

    // If the tcnt is so large it wraps around, set it to UINT64_MAX

    if (UINT64_MAX - al->twake < tcnt)
        al->twake = UINT64_MAX;
    else
        al->twake += tcnt;
    
    // If the wake-up time has already passed, return

    if (al->twake < now)
        return;
    
    // FIXME your code goes here

    //disable interupts
    pie = disable_interrupts();

    
    struct alarm * pointer = sleep_list;
    struct alarm * prev_pointer = NULL;

    //find where to insert the alarm in sleep list
    while(pointer != NULL && pointer->twake < al->twake){
        prev_pointer = pointer;
        pointer = pointer->next;
    }
    
    int changed = 0;
    //insert alarm at head of list if it has earliest wake time 
    if (prev_pointer == NULL){
        changed = 1;
        al->next = sleep_list;
        sleep_list = al;
    }
    // insert alarm between prev_pointer and pointer
    else{
        prev_pointer->next = al;
        al->next = pointer;
    }
    

    //update mtimecmp to trigger at the first elements twake
    if (changed == 1){
        set_stcmp(sleep_list->twake);
    }
    

    //enable timer interrupts
    restore_interrupts(pie);

    //put current thread to sleep
    condition_wait(&al->cond);
    
}
// Resets the alarm so that the next sleep increment is relative to the time
// alarm_reset is called.

void alarm_reset(struct alarm * al) {
    al->twake = rdtime();
}

void alarm_sleep_sec(struct alarm * al, unsigned int sec) {
    alarm_sleep(al, sec * TIMER_FREQ);
}

void alarm_sleep_ms(struct alarm * al, unsigned long ms) {
    alarm_sleep(al, ms * (TIMER_FREQ / 1000));
}

void alarm_sleep_us(struct alarm * al, unsigned long us) {
    alarm_sleep(al, us * (TIMER_FREQ / 1000 / 1000));
}

void sleep_sec(unsigned int sec) {
    sleep_ms(1000UL * sec);
}

void sleep_ms(unsigned long ms) {
    sleep_us(1000UL * ms);
}

void sleep_us(unsigned long us) {
    struct alarm al;

    alarm_init(&al, "sleep");
    alarm_sleep_us(&al, us);
}

//void handle_timer_interrupt(void)
//Inputs: none (void)
//Outputs: none (void)
//Descriptions: wakes threads whose alarms have expired
//Side Effects: broadcasts to expired alarms, removes them from sleep list, uptates timer compare register
void handle_timer_interrupt(void) {
    struct alarm * head = sleep_list;
    uint64_t now;
    uint64_t next_alarm;


    now = rdtime();

    trace("[%lu] %s()", now, __func__);
    debug("[%lu] mtcmp = %lu", now, rdtime());

    // FIXME your code goes here

    //wake all waiting threads that wait on conditions that have already passed
    while (head != NULL && head->twake <= now){
        condition_broadcast(&head->cond);
        head = head->next;
    }

    //point sleep list to the next item that is sleeping
    sleep_list = head;

    //check if the preemptive alarm triggered
    if (now >= premptive_clock){
        //set clock forward and yield
        premptive_clock = now + PREM_DUR;
        running_thread_yield();
    }

    //set the next timer
    if (sleep_list == NULL){
        //no alarms
        next_alarm = UINT64_MAX;
    }
    else{
        next_alarm = sleep_list->twake;
    }

    //set the timer to min(premp, nalarm)
    if (premptive_clock < next_alarm) {
        set_stcmp(premptive_clock);
    } else {
        set_stcmp(next_alarm);
    }
}