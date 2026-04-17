// viorng.c - VirtIO rng device
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "virtio.h"
#include "intr.h"
#include "heap.h"
#include "error.h"
#include "string.h"
#include "thread.h"
#include "devimpl.h"
#include "assert.h"
#include "conf.h"
#include "intr.h"
#include "console.h"

// INTERNAL CONSTANT DEFINITIONS
//

#ifndef VIORNG_BUFSZ
#define VIORNG_BUFSZ 256
#endif

#ifndef VIORNG_NAME
#define VIORNG_NAME "viorng"
#endif

#ifndef VIORNG_IRQ_PRIO
#define VIORNG_IRQ_PRIO 1
#endif

// INTERNAL TYPE DEFINITIONS
//

struct viorng_serial {
    // FIXME your code goes here
    struct serial base;
    volatile struct virtio_mmio_regs * regs;
    int irqno;
    char opened;

    struct virtq_desc * desc;
    struct virtq_avail* avail;
    struct virtq_used* used;

    char data[256]; //256 = VIORNG_BUFSZ
    int size;

    struct condition rngready;
};

// INTERNAL FUNCTION DECLARATIONS
//

static int viorng_serial_open(struct serial * ser);

static void viorng_serial_close(struct serial * ser);
static int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz);

static void viorng_isr(int irqno, void * aux);

// INTERNAL GLOBAL VARIABLES
//

static const struct serial_intf viorng_serial_intf = {
    .blksz = 1,
    .open = &viorng_serial_open,
    .close = &viorng_serial_close,
    .recv = &viorng_serial_recv
};

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO rng device. Declared and called directly from virtio.c.

//viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno)
//Inputs: volatile struct virtio_mmio_regs * regs - pointer to virtio regs
//        int irqno - interrupt number for device
//Outputs: none (void)
//Descriptions: attaches virtio device, negoriates features, sets up virtqueue, registers device
//Side Effects: allocates heap memory, registers device with manager
void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct viorng_serial * vrng;
    int result;
    
    //assert (regs->device_id == VIRTIO_ID_RNG);

    // Signal device that we found a driver

    regs->status |= VIRTIO_STAT_DRIVER;
    // fence o,io
    __sync_synchronize();

    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // Allocate and initialize device struct

    // FIXME your code goes here 
    //Init the struct and set necessary bits 
    vrng = kcalloc(1, sizeof(struct viorng_serial));
    vrng->regs = regs;
    vrng->irqno = irqno;
    vrng->opened = 0;

    //CP3: init condition var
    condition_init(&vrng->rngready, NULL);

    //allocate space for virtq
    vrng->desc = kcalloc(1, sizeof(struct virtq_desc));
    vrng->avail = kcalloc(1, VIRTQ_AVAIL_SIZE(1));
    vrng->used = kcalloc(1, VIRTQ_USED_SIZE(1));

    //Setup virtque
    vrng->avail->ring[0] = 0;
    vrng->desc[0].flags = VIRTQ_DESC_F_WRITE;
    vrng->desc[0].addr = (uint64_t)vrng->data;
    vrng->desc[0].len = VIORNG_BUFSZ;
    vrng->desc[0].next = 0;
    
    virtio_attach_virtq(regs, 0, 1, (uint64_t)vrng->desc, (uint64_t)vrng->used, (uint64_t)vrng->avail);

    regs->status |= VIRTIO_STAT_DRIVER_OK; //set the driver to OK
    // fence o,oi
    __sync_synchronize();

    // FIXME your code goes here
    //init serial interface and register device with manager
    serial_init(&vrng->base, &viorng_serial_intf);
    register_device(VIORNG_NAME, DEV_SERIAL, vrng);
}

//int viorng_serial_open(struct serial * ser)
//Inputs: struct serial * ser - pointer to viorng struct 
//Outputs: 0 on success, -EBUSY if already opened
//Descriptions: opens virtio device, enables virtq, regsiters isr
//Side Effects: marks device as opened
int viorng_serial_open(struct serial * ser) {
    // FIXME your code goes here
    struct viorng_serial * vrng = (struct viorng_serial *)ser;

    //check if already open
    if (vrng->opened == 1){
        return -EBUSY;
    }

    //make avail & used available for use
    virtio_enable_virtq(vrng->regs, 0);
    vrng->size = 0;

    //enable the interrupt source
    enable_intr_source(vrng->irqno, VIORNG_IRQ_PRIO, viorng_isr, vrng);

    //mark as opened
    vrng->opened = 1;
    return 0;
}

//void viorng_serial_close(struct serial * ser)
//Inputs: struct serial * ser - pointer to viorng struct 
//Outputs: none (void)
//Descriptions: closes virtio device, resets virtq, disables interrupts
//Side Effects: marks device as closed 
void viorng_serial_close(struct serial * ser) {
    // FIXME your code goes here
    struct viorng_serial * vrng = (struct viorng_serial *)ser;

    //do nothing if closed
    if (vrng->opened == 0){
        return;
    }
    
    //reset queues and prevent further interrupts
    virtio_reset_virtq(vrng->regs, 0);
    disable_intr_source(vrng->irqno);
    
    //mark as closed
    vrng->opened = 0;

}

//int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz)
//Inputs: struct serial * ser - pointer to viorng struct 
//        void * buf - buf that stores random data
//        unsigned int bufsz - size of buf
//Outputs: number of bytes copied, -EINVAL if device isn't opened
//Descriptions: reads random data from device buf, requests entropy from device if needed
//Side Effects: may request entropy from device and condition wait, shifts remaining data in buffer
int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    // FIXME your code goes here
    struct viorng_serial * vrng = (struct viorng_serial *)ser;

    //check if not open
    if (vrng->opened == 0){
        return -EINVAL;
    }

    //check if the bufsz is valid
    if (bufsz < 1) {return 0;}

    //set apropriate registers -> request entropy from device 
    //this only happens if size < bufsz
    if (vrng->size < bufsz){
        int last = vrng->used->idx;
        vrng->avail->idx++;
        virtio_notify_avail(vrng->regs, 0);

        int pie = disable_interrupts();
        
        //CP3: wait for device to provide data
        while (vrng->used->idx == last){
            condition_wait(&vrng->rngready);
        }

        restore_interrupts(pie);

        vrng->size = 256;
    }

    //copy from struct buf to void* buf
    int copiedBytes = (bufsz < VIORNG_BUFSZ) ? bufsz : VIORNG_BUFSZ;
    memcpy(buf, (void*)vrng->desc[0].addr, copiedBytes);

    //adjust buf size accourdingly 
    vrng->size -= copiedBytes;

    //shift the array left
    if (vrng->size > 0) {
        memcpy(vrng->data, vrng->data + copiedBytes, vrng->size);
    }
    
    return copiedBytes;
}

//void viorng_isr(int irqno, void * aux)
//Inputs: int irqno - interrupt source number 
//        void * aux - pointer to viorng struct 
//Outputs: none (void)
//Descriptions: handles device interrupts
//Side Effects: acknowldges interrupt, condition broadcasts to wake waiting threads
void viorng_isr(int irqno, void * aux) {
    // FIXME your code goes here
    struct viorng_serial * vrng = (struct viorng_serial *)aux;
    
    //set appropriate device regs
    vrng->regs->interrupt_ack = vrng->regs->interrupt_status;


    //wake thread up after waiting for device to finish (CP3)
    condition_broadcast(&vrng->rngready);

}