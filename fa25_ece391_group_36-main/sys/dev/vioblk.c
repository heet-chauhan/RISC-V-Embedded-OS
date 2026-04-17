/*! @file vioblk.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌⁠‍‍‌‌‍‌​⁠⁠‌
    @brief VirtIO block device
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#include "devimpl.h"
#ifdef VIOBLK_TRACE
#define TRACE
#endif

#ifdef VIOBLK_DEBUG
#define DEBUG
#endif

#include <limits.h>

#include "conf.h"
#include "console.h"
#include "device.h"
#include "error.h"
#include "heap.h"
#include "intr.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uio.h"  // FCNTL
#include "virtio.h"


// COMPILE-TIME PARAMETERS
//

#ifndef VIOBLK_INTR_PRIO
#define VIOBLK_INTR_PRIO 1
#endif

#ifndef VIOBLK_NAME
#define VIOBLK_NAME "vioblk"
#endif

#ifndef ENODEV
#define ENODEV 19
#endif
// INTERNAL CONSTANT DEFINITIONS
//

// VirtIO block device feature bits (number, *not* mask)

#define VIRTIO_BLK_F_SIZE_MAX 1
#define VIRTIO_BLK_F_SEG_MAX 2
#define VIRTIO_BLK_F_GEOMETRY 4
#define VIRTIO_BLK_F_RO 5
#define VIRTIO_BLK_F_BLK_SIZE 6
#define VIRTIO_BLK_F_FLUSH 9
#define VIRTIO_BLK_F_TOPOLOGY 10
#define VIRTIO_BLK_F_CONFIG_WCE 11
#define VIRTIO_BLK_F_MQ 12
#define VIRTIO_BLK_F_DISCARD 13
#define VIRTIO_BLK_F_WRITE_ZEROES 14


//Make struct for vioblk_storage for attach function and have the request struct from reference


//from the reference 
#define VIRTIO_BLK_T_IN     0   // read
#define VIRTIO_BLK_T_OUT    1   // write
struct virtio_blk_req {
    uint32_t type;      // VIRTIO_BLK_T_IN or VIRTIO_BLK_T_OUT (read or write )
    uint32_t reserved;  // always 0
    uint64_t sector;    // sector offset (LBA), pos / 512
};

//storage
struct vioblk_storage{
    struct storage sto;
    volatile struct virtio_mmio_regs * regs;
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    int irqno;
    uint32_t blksz;
    int qlen;
    int opened;
    struct condition finished;
    int completed;
    struct lock locky;
    uint8_t status;                               
    struct virtio_blk_req req_header;
    
};

// INTERNAL FUNCTION DECLARATIONS
//

/**
    * @brief Sets the virtq avail and virtq used queues such that they are available for use. (Hint,
    * read virtio.h) Enables the interupt line for the virtio device and sets necessary flags in vioblk
    * device.
    * @param sto Storage IO struct for the storage device
    * @return Return 0 on success or negative error code if error. If the given sto is already opened,
    * then return -EBUSY.
    */
static int vioblk_storage_open(struct storage* sto);

/**
    * @brief Resets the virtq avail and virtq used queues and sets necessary flags in vioblk device. If
    * the given sto is not opened, this function does nothing.
    * @param sto Storage IO struct for the storage device
    * @return None
    */
static void vioblk_storage_close(struct storage* sto);

/**
    * @brief Reads bytecnt number of bytes from the disk and writes them to buf. Achieves this by
    * repeatedly setting the appropriate registers to request a block from the disk, waiting until the
    * data has been populated in block buffer cache, and then writes that data out to buf. Thread
    * sleeps while waiting for the disk to service the request.
    * @param sto Storage IO struct for the storage device
    * @param pos The starting position for the read within the VirtIO device
    * @param buf A pointer to the buffer to fill with the read data
    * @param bytecnt The number of bytes to read from the VirtIO device into the buffer
    * @return The number of bytes read from the device, or negative error code if error
    */
static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                unsigned long bytecnt);

/**
    * @brief Writes bytecnt number of bytes from the parameter buf to the disk. The size of the virtio
    * device should not change. You should only overwrite existing data. Write should also not create
    * any new files. Achieves this by filling up the block buffer cache and then setting the
    * appropriate registers to request the disk write the contents of the cache to the specified block
    * location. Thread sleeps while waiting for the disk to service the request.
    * @param sto Storage IO struct for the storage device
    * @param pos The starting position for the write within the VirtIO device
    * @param buf A pointer to the buffer with the data to write
    * @param bytecnt The number of bytes to write to the VirtIO device from the buffer
    * @return The number of bytes written to the device, or negative error code if error
    */
static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                unsigned long bytecnt);

/**
    * @brief Given a file io object, a specific command, and possibly some arguments, execute the
    * corresponding functions on the VirtIO block device.
    * @details Any commands such as FCNTL_GETEND should pass back through the arg variable. Do not
    * directly return the value.
    * @details FCNTL_GETEND should return the capacity of the VirtIO block device in bytes.
    * @param sto Storage IO struct for the storage device
    * @param op Operation to execute. vioblk should support FCNTL_GETEND.
    * @param arg Argument specific to the operation being performed
    * @return Status code on the operation performed
    */
static int vioblk_storage_cntl(struct storage* sto, int op, void* arg);

/**
    * @brief The interrupt handler for the VirtIO device. When an interrupt occurs, the system will
    * call this function.
    * @param irqno The interrupt request number for the VirtIO device
    * @param aux A generic pointer for auxiliary data.
    * @return None
    */
static void vioblk_isr(int irqno, void* aux);

// EXPORTED FUNCTION DEFINITIONS
//
static const struct storage_intf vioblk_intf = {
    .blksz = 1,
    .open  = vioblk_storage_open,
    .close = vioblk_storage_close,
    .fetch = vioblk_storage_fetch,
    .store = vioblk_storage_store,
    .cntl  = vioblk_storage_cntl,
};
// Attaches a VirtIO block device. Declared and called directly from virtio.c.
/**
    * @brief Initializes virtio block device with the necessary IO operation functions and sets the
    * required feature bits.
    * @param regs Memory mapped register of Virtio
    * @param irqno Interrupt request number of the device
    * @return None
    */
void vioblk_attach(volatile struct virtio_mmio_regs* regs, int irqno) {
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct vioblk_storage* vbd;
    unsigned int blksz;
    int result;

    trace("%s(regs=%p,irqno=%d)", __func__, regs, irqno);

    assert(regs->device_id == VIRTIO_ID_BLOCK);

    // Signal device that we found a driver

    regs->status |= VIRTIO_STAT_DRIVER;
    __sync_synchronize();  // fence o,io

    // Negotiate features. We need:
    //  - VIRTIO_F_RING_RESET and
    //  - VIRTIO_F_INDIRECT_DESC
    // We want:
    //  - VIRTIO_BLK_F_BLK_SIZE and
    //  - VIRTIO_BLK_F_TOPOLOGY.

    virtio_featset_init(needed_features);
    virtio_featset_add(needed_features, VIRTIO_F_RING_RESET);
    virtio_featset_add(needed_features, VIRTIO_F_INDIRECT_DESC);
    virtio_featset_init(wanted_features);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_BLK_SIZE);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_TOPOLOGY);
    result = virtio_negotiate_features(regs, enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // If the device provides a block size, use it. Otherwise, use 512.

    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        blksz = regs->config.blk.blk_size;
    else
        blksz = 512;

    // blksz must be a power of two
    assert(((blksz - 1) & blksz) == 0);

    // FIXME

    //initialize the vbd struct and maybe add more members later 
    vbd = kcalloc(1, sizeof(struct vioblk_storage));
    // vbd = kcalloc(1, sizeof(struct virtio_blk_req));

    vbd->regs = regs;
    vbd->blksz = blksz;
    vbd->irqno = irqno;
    vbd->qlen = 4;
    vbd->completed = 1;
    vbd->status = 0xFF;
    condition_init(&vbd->finished,"fetch");
    lock_init(&vbd->locky);
    vbd->desc= kcalloc(1, sizeof(struct virtq_desc)*4); //allocated space for the descs
    vbd->avail  = kcalloc(1, VIRTQ_AVAIL_SIZE(vbd->qlen));
    vbd->used = kcalloc(1,VIRTQ_USED_SIZE(vbd->qlen));


    memset(vbd->desc, 0, sizeof(struct virtq_desc) * vbd->qlen); // reset all desc to 0

    vbd->used->flags= 0;// setup the virtqs
    vbd->used->idx= 0;
    vbd->avail->flags = 0;
    vbd->avail->idx= 0;


//attach the virtqueues
    virtio_attach_virtq(regs, 0, vbd->qlen, (uint64_t)vbd->desc, (uint64_t)vbd->used, (uint64_t)vbd->avail);
    virtio_enable_virtq(vbd->regs, 0); // enable the virtqueue may be not necessary


    //set driver to okay 
    regs->status |= VIRTIO_STAT_DRIVER_OK;
    __sync_synchronize();
    unsigned long long capacity_bytes = (unsigned long long) regs->config.blk.capacity * 512ULL;
    vbd->sto.intf  = &vioblk_intf;   // attach our function table


    //register the device
    storage_init(&vbd->sto, &vioblk_intf, capacity_bytes);

    register_device(VIOBLK_NAME, DEV_STORAGE,&vbd->sto); // i think, but im not sure about the last two arguments.


}

static int vioblk_storage_open(struct storage* sto) {
    // FIXME
    struct vioblk_storage *vbd = (struct vioblk_storage *)sto;
    if(vbd->opened == 1){ // check if already open 
        return -EBUSY;
    }

    virtio_enable_virtq(vbd->regs, 0); // enabke the virtq
    enable_intr_source(vbd->irqno, VIOBLK_INTR_PRIO, vioblk_isr, vbd); // enable interrupt line


    vbd->opened = 1; //set the flags in vioblk
    return 0;
}

static void vioblk_storage_close(struct storage* sto) {
    // FIXME
    struct vioblk_storage *vbd = (struct vioblk_storage *)sto;
    if ( vbd->opened == 0){ //cehck if already closed
        return;
    }
    virtio_reset_virtq(vbd->regs, 0); // reset the virtq
    disable_intr_source(vbd->irqno); // oposite of open, disable interrupot line when reset virtq

    vbd->opened = 0; // set the flag in vioblk device 
    return;
}



static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
    unsigned long bytecnt) {
    // FIXME
    struct vioblk_storage *vbd = (struct vioblk_storage *)sto;

    if (vbd->opened == 0){
        return -EINVAL;
    }

    // truncate bytecnt to nearest multiple of 512
    bytecnt = (bytecnt / 512) * 512;

    // Calculate capacity
    uint64_t capacity_bytes = vbd->regs->config.blk.capacity * 512;

    // Bounds check - can't read past end
    if (pos >= capacity_bytes) {
        return 0;
    }
    if (pos + bytecnt > capacity_bytes) {
        bytecnt = capacity_bytes - pos;
    }
    if (bytecnt == 0) {
        return 0;
    }

    long total_read = 0;

    // read one sector 512 B at a time
    while (bytecnt > 0) {
        // how much to read this iteration.
        unsigned long to_read;

        if (bytecnt > 512) {
            to_read = 512;
        } else {
            to_read = bytecnt;
        }

        uint64_t sector = pos / 512;

        //request header
        vbd->req_header.type = VIRTIO_BLK_T_IN;
        vbd->req_header.reserved = 0;
        vbd->req_header.sector = sector;
        vbd->status = 0xFF;
        vbd->completed = 0;

        //  descriptors
        vbd->desc[0].addr  = (uint64_t)&vbd->req_header;
        vbd->desc[0].len   = sizeof(vbd->req_header);
        vbd->desc[0].flags = VIRTQ_DESC_F_NEXT;
        vbd->desc[0].next  = 1;

        // data buffer
        vbd->desc[1].addr  = (uint64_t)buf;  // Current position in buffer
        vbd->desc[1].len   = to_read;
        vbd->desc[1].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
        vbd->desc[1].next  = 2;

        // status byte
        vbd->desc[2].addr  = (uint64_t)&vbd->status;
        vbd->desc[2].len   = 1;
        vbd->desc[2].flags = VIRTQ_DESC_F_WRITE;

        // Add to available ring
        uint16_t idx = vbd->avail->idx % vbd->qlen;
        vbd->avail->ring[idx] = 0;
        vbd->avail->idx++;

        // Notify device
        virtio_notify_avail(vbd->regs, 0);

        // Wait for completion
        lock_acquire(&vbd->locky);
        int pie = disable_interrupts();
        while (!vbd->completed) {
            condition_wait(&vbd->finished);
        }
        restore_interrupts(pie);
        lock_release(&vbd->locky);

        // Check status
        if (vbd->status != 0) {
            return (total_read > 0) ? total_read : -EIO;
        }

        // Advance for next iteration
        total_read += to_read;
        buf = (char*)buf + to_read;  // Move buffer pointer forward
        pos += to_read;               // Move position forward
        bytecnt -= to_read;           // Decrease remaining bytes
    }

    return total_read;
}


static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
    unsigned long bytecnt) {
    // FIXME

    struct vioblk_storage *vbd = (struct vioblk_storage *)sto;

    if (vbd->opened == 0){
        return -EINVAL;
    }


    // round to 512 byte multiple
    bytecnt = (bytecnt / 512) * 512;

    uint64_t capacity_bytes = vbd->regs->config.blk.capacity * 512;

    // Bounds check
    if (pos >= capacity_bytes) {
        return -EINVAL; // can't write past EOF
    }
    if (pos + bytecnt > capacity_bytes){
        bytecnt = capacity_bytes - pos;
    }
    if(bytecnt == 0){
        return 0;
    }

    long total_written = 0;

    // write one sector 512B at a time
    while (bytecnt > 0) {
        // How much to write this iteration max 512B
        unsigned long to_write;
        
        if (bytecnt > 512) {
            to_write = 512;
        } else {
            to_write = bytecnt;
        }
        
        uint64_t sector = pos / 512;

        //set up the write operation with the descriptors.
        vbd->req_header.type = VIRTIO_BLK_T_OUT;  // write request
        vbd->req_header.reserved = 0;
        vbd->req_header.sector = sector;  
        vbd->status = 0xFF;
        vbd->completed = 0;

        //set up all the descriptors and connect with the next one 
        //header
        vbd->desc[0].addr  = (uint64_t)&vbd->req_header;
        vbd->desc[0].len   = sizeof(vbd->req_header);
        vbd->desc[0].flags = VIRTQ_DESC_F_NEXT; 
        vbd->desc[0].next  = 1;   

        // data buf device reads so no write flag
        vbd->desc[1].addr = (uint64_t)buf;
        vbd->desc[1].len = to_write;
        vbd->desc[1].flags = VIRTQ_DESC_F_NEXT;  // Device READS from buf
        vbd->desc[1].next = 2;

        //status (device writes status byte)
        vbd->desc[2].addr  = (uint64_t)&vbd->status;
        vbd->desc[2].len   = 1;
        vbd->desc[2].flags = VIRTQ_DESC_F_WRITE;

        // Pass to the avail     
        uint16_t idx = vbd->avail->idx % vbd->qlen;
        vbd->avail->ring[idx] = 0;  
        vbd->avail->idx++;

        // notify the device that everything is ready 
        virtio_notify_avail(vbd->regs, 0);

        // make the thread sleep until ISR says its done 
        lock_acquire(&vbd->locky);
        int pie = disable_interrupts();
        while (!vbd->completed) {
            condition_wait(&vbd->finished);
        }
        restore_interrupts(pie);
        lock_release(&vbd->locky);

        if(vbd->status != 0){
            return (total_written > 0) ? total_written : -EIO;
        }

        // Advance for next iteration
        total_written += to_write;
        buf = (const char*)buf + to_write;
        pos += to_write;
        bytecnt -= to_write;
    }

    //return the bytes that we wrote if we were successful
    return total_written;
}

static int vioblk_storage_cntl(struct storage* sto, int op, void* arg) {
    // FIXME
    // struct vioblk_storage *vbd = (struct vioblk_storage *)sto;
    // unsigned long long capacity_bytes;

    // capacity_bytes = (unsigned long long) vbd->regs->config.blk.capacity * 512ULL;

    if (op == FCNTL_GETEND) {
        if (arg == NULL) {
            return -EINVAL;
        }
        *(unsigned long long *)arg = sto->capacity;
        return 0;
    }
    // else if (op == FCNTL_GETPOS) {
    //     // we don't track a file pointer for block devs; pretend it's 0
    //     if (arg != NULL) {
    //         *(unsigned long long *)arg = 0;
    //     }
    //     return 0;
    // }
    // else if (op == FCNTL_SETPOS) {
    //     // ignore requested position, claim success
    //     return 0;
    // }
    // else if (op == FCNTL_SETEND) {
    //     // block device size is fixed; say "ok" but don't change anything
    //     return 0;
    // }
    // else if (op == FCNTL_MMAP) {
        
    //     if (arg != NULL) {
    //         *(void **)arg = NULL;
    //     }
    //     return 0;
    // } // idk if we actually need these for now.

    // unknown op
    return -ENOTSUP;
}

static void vioblk_isr(int irqno, void* aux) {
    // FIXME
    struct vioblk_storage* vbd = (struct vioblk_storage*) aux;
    vbd->regs->interrupt_ack = vbd->regs->interrupt_status;
    vbd->completed = 1;
    condition_broadcast(&vbd->finished);
    return;
}
