// uart.c -  NS8250-compatible serial port
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef UART_TRACE
#define TRACE
#endif

#ifdef UART_DEBUG
#define DEBUG
#endif

#include "conf.h"
//#include "assert.h"
#include "uart.h"
#include "devimpl.h"
#include "intr.h"
#include "heap.h"
#include "thread.h"
#include "console.h"
#include "misc.h"

#include "error.h"

#include <stdint.h>

// COMPILE-TIME CONSTANT DEFINITIONS
//

#ifndef UART_RBUFSZ
#define UART_RBUFSZ 64
#endif

#ifndef UART_INTR_PRIO
#define UART_INTR_PRIO 1
#endif

#ifndef UART_DEVNAME
#define UART_DEVNAME "uart"
#endif


// INTERNAL TYPE DEFINITIONS
// 

struct uart_regs {
    union {
        char rbr; // DLAB=0 read
        char thr; // DLAB=0 write
        uint8_t dll; // DLAB=1
    };
    
    union {
        uint8_t ier; // DLAB=0
        uint8_t dlm; // DLAB=1
    };
    
    union {
        uint8_t iir; // read
        uint8_t fcr; // write
    };

    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
};

#define LCR_DLAB (1 << 7)
#define LSR_OE (1 << 1)
#define LSR_DR (1 << 0)
#define LSR_THRE (1 << 5)
#define IER_DRIE (1 << 0)
#define IER_THREIE (1 << 1)

// Simple fixed-size ring buffer

struct ringbuf {
    unsigned int hpos; // head of queue (from where elements are removed)
    unsigned int tpos; // tail of queue (where elements are inserted)
    char data[UART_RBUFSZ];
};

// UART device structure

struct uart_serial {
    struct serial base;
    volatile struct uart_regs * regs;
    int irqno;
    char opened;

    unsigned long rxovrcnt; ///< number of times OE was set
    
    struct condition rxbnotempty; ///< signalled when rxbuf becomes not empty
    struct condition txbnotfull;  ///< signalled when txbuf becomes not full

    struct ringbuf rxbuf;
    struct ringbuf txbuf;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int uart_serial_open(struct serial * ser);
static void uart_serial_close(struct serial * ser);
static int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz);
static int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz);

static void uart_isr(int srcno, void * aux);

// Ring buffer (struct rbuf) functions

static void rbuf_init(struct ringbuf * rbuf);
static int rbuf_empty(const struct ringbuf * rbuf);
static int rbuf_full(const struct ringbuf * rbuf);
static void rbuf_putc(struct ringbuf * rbuf, char c);
static char rbuf_getc(struct ringbuf * rbuf);

// INTERNAL GLOBAL VARIABLES
//

static const struct serial_intf uart_serial_intf = {
    .blksz = 1,
    .open = &uart_serial_open,
    .close = &uart_serial_close,
    .recv = &uart_serial_recv,
    .send = &uart_serial_send
};

// EXPORTED FUNCTION DEFINITIONS
// 


void attach_uart(void * mmio_base, int irqno) {
    struct uart_serial * uart;

    trace("%s(%p,%d)", __func__, mmio_base, irqno);
    
    // UART0 is used for the console and should not be attached as a normal
    // device. It should already be initialized by console_init(). We still
    // register the device (to reserve the name uart0), but pass a NULL device
    // pointer, so that find_serial("uart", 0) returns NULL.

    if (mmio_base == (void*)UART0_MMIO_BASE) {
        register_device(UART_DEVNAME, DEV_SERIAL, NULL);
        return;
    }
    
    uart = kcalloc(1, sizeof(struct uart_serial));

    uart->regs = mmio_base;
    uart->irqno = irqno;
    uart->opened = 0;

    // Initialize condition variables. The ISR is registered when our interrupt
    // source is enabled in uart_serial_open().

    condition_init(&uart->rxbnotempty, "uart.rxnotempty");
    condition_init(&uart->txbnotfull, "uart.txnotfull");


    // Initialize hardware

    uart->regs->ier = 0;
    uart->regs->lcr = LCR_DLAB;
    // fence o,o ?
    uart->regs->dll = 0x01;
    uart->regs->dlm = 0x00;
    // fence o,o ?
    uart->regs->lcr = 0; // DLAB=0

    serial_init(&uart->base, &uart_serial_intf);
    register_device(UART_DEVNAME, DEV_SERIAL, uart);
}

//int uart_serial_open(struct serial * ser)
//Inputs: struct serial * ser - points to serial struct
//Outputs: 0 if opened successfully else -EBUSY
//Descriptions: opens a uart, inits bufs, enables interrupts, registers isr
//Side Effects: enable DR, register an intr source. marks device as opened
int uart_serial_open(struct serial * ser) {
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);

    if (uart->opened)
        return -EBUSY;
    
    // Reset receive and transmit buffers
    
    rbuf_init(&uart->rxbuf);
    rbuf_init(&uart->txbuf);

    // Read receive buffer register to flush any stale data in hardware buffer

    uart->regs->rbr; // forces a read because uart->regs is volatile

    // Enable interrupts when data ready (DR) status asserted
    // FIXME your code goes here
    uart->regs->ier |= IER_DRIE;
 
    //Register the UART's interrupt handle
    enable_intr_source(uart->irqno, UART_INTR_PRIO, uart_isr, uart);

    //mark as opened
    uart->opened = 1;
    return 0;
}

//void uart_serial_close(struct serial * ser)
//Inputs: struct serial * ser - points to serial struct
//Outputs: none (void)
//Descriptions: closes a uart, disables interrupts, unregisters isr
//Side Effects: disables interrupts, marks device closed
void uart_serial_close(struct serial * ser) {
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);

    // FIXME your code goes here
    //make sure the given ser is open
    if (uart->opened == 0) {return;}

    //disable interupts
    uart->regs->ier = 0;

    //disable interupt source
    disable_intr_source(uart->irqno);

    //indicate the serial is closed
    uart->opened = 0;
}


//int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz)
//Inputs: struct serial * ser - points to serial struct
//        void * buf - buf that stores recieved data
//        unsigned int bufsz - size of buf
//Outputs: number of bytes read, -EINVAL if device is not opened
//Descriptions: reads character from rxbuf into buf
//Side Effects: condition waits if rxbuf empty, enables DR
int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    // FIXME your code goes here
    //init uart
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);

    //make sure ser is open
    if (uart->opened == 0) {return -EINVAL;}

    //check if the bufsz is valid
    if (bufsz < 1) {return 0;}

    //enable interupts
    uart->regs->ier |= IER_DRIE;

    int pie = disable_interrupts();

    //spin if empty
    while (rbuf_empty(&uart->rxbuf)) {
        condition_wait(&uart->rxbnotempty);
    }

    restore_interrupts(pie);

    //copy chars from recieve ring to provided buffer
    int counter = 0;
    char* writePointer = buf;
    while (!rbuf_empty(&uart->rxbuf) && counter < bufsz){
        //copy char to buf by pointer and increment
        char charToCopy = rbuf_getc(&uart->rxbuf);
        *(writePointer) = charToCopy;
        writePointer += 1;

        counter++;
    }

    return counter;
}



//int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz)
//Inputs: struct serial * ser - points to serial struct
//        void * buf - buf that contains data to be sent
//        unsigned int bufsz - size of buf
//Outputs: number of bytes written, -EINVAL if device not opened
//Descriptions: writes characters from buf to txbuf
//Side Effects: condition waits if txbuf full, enables THREIE
int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz) {
    // FIXME your code goes here
    //init uart
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);

    //make sure ser is open
    if (uart->opened == 0) {return -EINVAL;}

    //check if the bufsz is valid
    if (bufsz < 1) {return 0;}

    int pie = disable_interrupts();

    //spin if full
    while (rbuf_full(&uart->txbuf)){
        condition_wait(&uart->txbnotfull);
    }

    restore_interrupts(pie);

    //write chars from the buffer to the ring
    int counter = 0;
    const char* readPointer = buf;
    while (!rbuf_full(&uart->txbuf) && counter < bufsz){        
        //copy char to ring by pointer and increment
        const char charToCopy = *(readPointer);
        rbuf_putc(&uart->txbuf, charToCopy);      
        readPointer += 1;

        //set IER_THREIE to start write if first iter.
        if (counter == 0){
            uart->regs->ier |= IER_THREIE;
        }
        counter++;
    }
    
    return counter;
}

//void uart_isr(int srcno, void * aux)
//Inputs: int srcno - interrupt source number 
//        void * aux - points to serial struct
//Outputs: none (void)
//Descriptions: handles interrupts for DR and THREIE
//Side Effects: transfers data between ring buffer and uart, broadcasts condition vars, can disable interrupts
void uart_isr(int srcno, void * aux) {
    // FIXME your code goes here
    //set aux object
    struct uart_serial * const uart = aux;

    //check if its the read command 
    //if read buff isnt full, then putchar into ring
    //CP3: brodcast condition change
    //else turn off interrupts
    if (uart->regs->lsr & LSR_DR){
        if (!rbuf_full(&uart->rxbuf)){
            rbuf_putc(&uart->rxbuf, uart->regs->rbr);
            condition_broadcast(&uart->rxbnotempty);
        }
        else{
            uart->regs->ier &= ~IER_DRIE;
        }
    }

    //check if its the write command
    //if write buff isnt full, then getc from cpu into uart
    //CP3: broadcast condition change
    //else turn off interrupts
    if (uart->regs->lsr & LSR_THRE){
        if (!rbuf_empty(&uart->txbuf)){
            uart->regs->thr = rbuf_getc(&uart->txbuf);
            condition_broadcast(&uart->txbnotfull);
        }
        else{
            uart->regs->ier &= ~IER_THREIE;
        }
    }
}

void rbuf_init(struct ringbuf * rbuf) {
    rbuf->hpos = 0;
    rbuf->tpos = 0;
}



int rbuf_empty(const struct ringbuf * rbuf) {
    return (rbuf->hpos == rbuf->tpos);
}


int rbuf_full(const struct ringbuf * rbuf) {
    return (rbuf->tpos - rbuf->hpos == UART_RBUFSZ);
}


void rbuf_putc(struct ringbuf * rbuf, char c) {
    uint_fast16_t tpos;

    tpos = rbuf->tpos;
    rbuf->data[tpos % UART_RBUFSZ] = c;
    asm volatile ("" ::: "memory");
    rbuf->tpos = tpos + 1;
}

char rbuf_getc(struct ringbuf * rbuf) {
    uint_fast16_t hpos;
    char c;

    hpos = rbuf->hpos;
    c = rbuf->data[hpos % UART_RBUFSZ];
    asm volatile ("" ::: "memory");
    rbuf->hpos = hpos + 1;
    return c;
}

// The functions below provide polled uart input and output for the console.

#define UART0 (*(volatile struct uart_regs*)UART0_MMIO_BASE)

void console_device_init(void) {
    UART0.ier = 0x00;

    // Configure UART0. We set the baud rate divisor to 1, the lowest value,
    // for the fastest baud rate. In a physical system, the actual baud rate
    // depends on the attached oscillator frequency. In a virtualized system,
    // it doesn't matter.
    
    UART0.lcr = LCR_DLAB;
    UART0.dll = 0x01;
    UART0.dlm = 0x00;

    // The com0_putc and com0_getc functions assume DLAB=0.

    UART0.lcr = 0;
}

void console_device_putc(char c) {
    // Spin until THR is empty
    while (!(UART0.lsr & LSR_THRE))
        continue;

    UART0.thr = c;
}

char console_device_getc(void) {
    // Spin until RBR contains a byte
    while (!(UART0.lsr & LSR_DR))
        continue;
    
    return UART0.rbr;
}