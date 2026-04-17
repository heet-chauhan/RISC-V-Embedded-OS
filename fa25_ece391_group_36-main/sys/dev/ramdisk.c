/*! @file ramdisk.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌⁠‍‍‌‌‍‌​⁠⁠‌
    @brief Memory-backed storage implementation
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef RAMDISK_DEBUG
#define DEBUG
#endif

#ifdef RAMDISK_TRACE
#define TRACE
#endif

#include <stddef.h>

#include "console.h"
#include "devimpl.h"
#include "error.h"
#include "heap.h"
#include "misc.h"
#include "string.h"
#include "uio.h"

#ifndef RAMDISK_NAME
#define RAMDISK_NAME "ramdisk"
#endif

// INTERNAL TYPE DEFINITIONS
//

/**
 * @brief Storage device backed by a block of memory. Allows modification of the backing memory
 * block.
 */
struct ramdisk {
    struct storage storage;  ///< Storage struct of memory storage
    void *buf;               ///< Block of memory
    size_t size;             ///< Size of memory block
};

// INTERNAL FUNCTION DECLARATIONS
//

static int ramdisk_open(struct storage *sto);
static void ramdisk_close(struct storage *sto);
static long ramdisk_fetch(struct storage *sto, unsigned long long pos, void *buf,
                          unsigned long bytecnt);
static int ramdisk_cntl(struct storage *sto, int cmd, void *arg);

// INTERNAL GLOBAL CONSTANTS
//

static const struct storage_intf ramdisk_intf = {
    .blksz = 1, // Ask TA whether I need to change it to 512 or not
    .open = &ramdisk_open,
    .close = &ramdisk_close,
    .fetch = &ramdisk_fetch,
    .store = NULL,  // Read-only storage (blob data in .rodata)
    .cntl = &ramdisk_cntl};

// EXPORTED FUNCTION DEFINITIONS
//

/**
 * @brief Creates and registers a memory-backed storage device
 * @return None
 */
void ramdisk_attach() {
    // External symbols from linker script for embedded blob data
    extern char _kimg_blob_start[], _kimg_blob_end[];

    // FIXME
    struct ramdisk * r_disk;

    r_disk = kcalloc(1, sizeof(struct ramdisk));

    if(!r_disk) {
        return;
    }

    unsigned long long size = (unsigned long long)(((char *)_kimg_blob_end) - ((char *)_kimg_blob_start));
    
    storage_init(&(((*r_disk).storage)), &(ramdisk_intf), size);

    void * buf_ptr = (void *)_kimg_blob_start;

    (*r_disk).buf = buf_ptr;

    (*r_disk).size = (size_t) size;

    register_device(RAMDISK_NAME, DEV_STORAGE, &((*r_disk).storage));
}

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * @brief Opens the _ramdisk_ device.
 * @param sto Storage struct pointer for memory storage
 * @return 0 on success
 */
static int ramdisk_open(struct storage *sto) {
    // FIXME
    if (!sto) {
        return -EINVAL;
    } else {
        return 0;
    }
}

/**
 * @brief Closes the _ramdisk_ device.
 * @param sto Storage struct pointer for memory storage
 */
static void ramdisk_close(struct storage *sto) {
    // FIXME
    return;
}

/**
 * @brief Reads bytecnt number of bytes from the disk and writes them to buf.
 * @details Performs proper bounds checks, then copies data from memory block to passed buffer
 * @param sto Storage struct pointer for memory storage
 * @param pos Position in storage to read from
 * @param buf Buffer to copy data from memory to
 * @param bytecnt Number of bytes to read from memory
 * @return Number of bytes successfully read
 */
static long ramdisk_fetch(struct storage *sto, unsigned long long pos, void *buf,
                          unsigned long bytecnt) {
    // FIXME
    if (!sto) {
        return -EINVAL;
    } else if(!buf) {
        return -EINVAL;
    }
    struct ramdisk * r_disk_ptr = (struct ramdisk *)(((char * ) sto) - offsetof(struct ramdisk, storage));

    struct ramdisk ramd = (*r_disk_ptr);

    if(pos < (unsigned long long) ramd.size) {
        unsigned long long how_many_bytes = ((unsigned long long) ramd.size) - pos;
        if (how_many_bytes > bytecnt) {
            how_many_bytes = bytecnt;
        }
        memcpy(buf, (void *)((char *)(ramd.buf) + (pos)), how_many_bytes);
        return (long) how_many_bytes;
    } else {
        return 0;
    }

    return -ENOTSUP;
}

/**
 * @brief _cntl_ functions for memory storage.
 * @details Memory storage supports basic control operations
 * @details Any commands such as FCNTL_GETEND should pass back through the arg variable. Do not
 * directly return the value.
 * @details FCNTL_GETEND should return the capacity of the VirtIO block device in bytes.
 * @param sto Storage struct pointer for memory storage
 * @param cmd command to execute. ramdisk should support FCNTL_GETEND.
 * @param arg Argument for commands
 * @return 0 on success, error on failure or unsupported command
 */
static int ramdisk_cntl(struct storage *sto, int cmd, void *arg) {
    // FIXME
    if(!sto) {
        return -EINVAL;
    } else if (!arg) {
        return -EINVAL;
    } else {
        // Ask TA for help
        if (cmd == FCNTL_GETEND) {
            struct ramdisk *r_disk = (struct ramdisk *)((char *)sto - offsetof(struct ramdisk, storage));
            *(unsigned long long *)arg = (unsigned long long)(*r_disk).size;
            return 0;
        } else {
            return -EINVAL;
        }
    }

    return -EINVAL;
}