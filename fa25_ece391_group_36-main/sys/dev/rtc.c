// rtc.c - Goldfish RTC driver
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef RTC_TRACE
#define TRACE
#endif

#ifdef RTC_DEBUG
#define DEBUG
#endif

#include "rtc.h"
#include "conf.h"
//#include "assert.h"
#include "devimpl.h"
#include "console.h"
#include "string.h"
#include "heap.h"
#include "misc.h"

#include "error.h"

#include <stdint.h>

// INTERNAL TYPE DEFINITIONS
// 

struct rtc_regs {
    uint32_t time_low;  // read first, latches time_high
    uint32_t time_high; //
};

struct rtc_device {
    struct serial base; // must be first
    volatile struct rtc_regs * regs;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int rtc_open(struct serial * ser);
static void rtc_close(struct serial * ser);
static int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz);

static uint64_t read_real_time(volatile struct rtc_regs * regs);

// INTERNAL GLOBAL VARIABLES AND CONSTANTS
//

static const struct serial_intf rtc_serial_intf = {
    .blksz = 8,
    .open = &rtc_open,
    .close = &rtc_close,
    .recv = &rtc_recv
};

// EXPORTED FUNCTION DEFINITIONS
// 

//void rtc_attach(void * mmio_base)
//Inputs: void * mmio_base - pointer to base address for Goldfish device
//Outputs: none (void)
//Descriptions: allocates the inits RTC device and registers it
//Side Effects: allocates heap memory and adds a registered device

void rtc_attach(void * mmio_base) {
    // FIXME your code goes here
    struct rtc_device* rtc;
    rtc = kcalloc(1, sizeof(struct rtc_device)); //declare struct and allocate memory
    rtc->regs = mmio_base;
    serial_init(&rtc->base, &rtc_serial_intf);
    register_device("rtc", DEV_SERIAL, rtc);
}

int rtc_open(struct serial * ser) {
    trace("%s()", __func__);
    return 0;
}

void rtc_close(struct serial * ser) {
    trace("%s()", __func__);
}


//int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz)
//Inputs: struct serial * ser - points to serial interface
//        void * buf - buf that stores timestamp
//        unsigned int bufsz - size of buf
//Outputs: number of bytes read
//Descriptions: reads the current time from an rtc device and copies it to the buf
//Side Effects: reads from RTC regs
int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    // FIXME your code goes here
    struct rtc_device* const src = (void*)ser - offsetof(struct rtc_device, base);
    if (bufsz == 0 || bufsz < 8){ return 0;}

    uint64_t time_now = read_real_time(src->regs);
    memcpy(buf, &time_now, sizeof(uint64_t));
    return sizeof(uint64_t);
}

//uint64_t read_real_time(volatile struct rtc_regs * regs)
//Inputs: volatile struct rtc_regs * regs - pointer to RTC regs
//Outputs: uint64 timestamp
//Descriptions: reads low and high regs and fuses them to one global time
//Side Effects: reads from rtc regs
uint64_t read_real_time(volatile struct rtc_regs * regs) {
    // FIXME your code goes here
    uint32_t low, high;
    low = regs->time_low;
    high = regs->time_high;
    uint64_t time = ((uint64_t)high<<32)|low;
    return time;
}