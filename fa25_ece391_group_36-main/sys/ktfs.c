/*! @file ktfs.c‌‌‍‍‌‍⁠‌‌‌‌‌⁠‍‌‌⁠‍‌‌‌‍⁠‍‌‌‍⁠‌‌‍‌⁠‍‌‌‌‌‌⁠‍‍‌⁠⁠‌‌‌‌‌‍‌‍‌‍‌‌‍‍⁠⁠‌‍‍‌⁠‌‍‌‍‌‌‌‍‌‍‌‍‌‍‌⁠‌‌⁠‍‍‌‌‍‌⁠⁠‌
    @brief KTFS Implementation.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef KTFS_TRACE
#define TRACE
#endif

#ifdef KTFS_DEBUG
#define DEBUG
#endif

#include "ktfs.h"

#include "cache.h"
#include "console.h"
#include "device.h"
#include "devimpl.h"
#include "error.h"
#include "filesys.h"
#include "fsimpl.h"
#include "heap.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uio.h"
#include "uioimpl.h"

// INTERNAL TYPE DEFINITIONS
//





// INTERNAL FUNCTION DECLARATIONS
//

int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr);
void ktfs_close(struct uio* uio);
int ktfs_cntl(struct uio* uio, int cmd, void* arg);
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len);
long ktfs_store(struct uio* uio, const void* buf, unsigned long len);
int ktfs_create(struct filesystem* fs, const char* name);
int ktfs_delete(struct filesystem* fs, const char* name);
void ktfs_flush(struct filesystem* fs);

void ktfs_listing_close(struct uio* uio);
long ktfs_listing_read(struct uio* uio, void* buf, unsigned long bufsz);

//----------------------------------------------------------------------

//global cache tracker
static struct cache* theCache;

//-----------------------------------------------------------------------

static struct ktfs_superblock theSuperBlock;

static uint32_t data_block_offset;

//----------------------------------------------------------------------

struct inodeblock {
    struct ktfs_inode entrys[16]; //one inode = 32Bytes so 512/32 = 16
};

//----------------------------------------------------------------------

#define dentrySize 16
#define DENTRIES_PER_BLOCK   (KTFS_BLKSZ / sizeof(struct ktfs_dir_entry))
#define PTRS_PER_BLOCK       (KTFS_BLKSZ / sizeof(uint32_t))
struct dentryblock {
    struct ktfs_dir_entry entrys[dentrySize]; //one inode = 32Bytes so 512/32 = 16
};

//----------------------------------------------------------------------
static struct filesystem theFS;
//----------------------------------------------------------------------

/// @brief File struct for a file in the Keegan Teal Filesystem
struct ktfs_file {
    // Fill to fulfill spec
    struct uio uio;
    uint64_t file_size;
    struct ktfs_dir_entry * dir_entry;
    uint64_t pos;
    int opened;
    struct lock filet_lck;
};

struct opened_file_list{
    struct ktfs_file * file;
    struct opened_file_list * next;
};
struct ktfs_listing{
    struct uio base;
    uint32_t next;
};
static const struct uio_intf ktfs_listing_uio_intf = {
    .close = &ktfs_listing_close,
    .read  = &ktfs_listing_read,
    .write = NULL,  
    .cntl  = NULL    
};
static struct opened_file_list * theOpenFileList;
//------------------------------------------------------------------------

/**
 * @brief Mounts the file system with associated backing cache
 * @param cache Pointer to cache struct for the file system
 * @return 0 if mount successful, negative error code if error
 */
int mount_ktfs(const char* name, struct cache* cache) {
    // FIXME
    //keep track of cache in global var
    theCache = cache;

    //get the supernode
    struct ktfs_superblock * superblock;
    int result = cache_get_block(theCache, 0, (void**)&superblock);
    if (result < 0){
        return result;
    }
    
    theSuperBlock = *superblock;

    data_block_offset = 1 + theSuperBlock.inode_bitmap_block_count + theSuperBlock.bitmap_block_count + theSuperBlock.inode_block_count;

    cache_release_block(theCache, (void*)superblock, 0);

    //init the filesystem
    theFS.open = &ktfs_open;
    theFS.create = &ktfs_create;
    theFS.delete = &ktfs_delete;
    theFS.flush = &ktfs_flush;

    //init the filesystem
    result = attach_filesystem(name, &theFS);
    if (result < 0){
        return result;
    }

    //init the open list
    theOpenFileList = NULL;
   
    return 0;
   
}

static int get_inode_block(struct inodeblock** inodePtr){
    int inodeIndex = theSuperBlock.inode_bitmap_block_count + theSuperBlock.bitmap_block_count + 1;
    return cache_get_block(theCache, inodeIndex* KTFS_BLKSZ, (void**)inodePtr);
}

static void release_inode_block(struct inodeblock* inodePtr, int dirty) {
    cache_release_block(theCache, (void*)inodePtr, dirty);
}

static const struct uio_intf ktfs_file_uio_intf = {
    .close = &ktfs_close,
    .read = &ktfs_fetch,
    .write = &ktfs_store,
    .cntl = &ktfs_cntl
};

struct ktfs_dir_entry* search_direct_block(uint32_t idx, const char * name) {
    if (idx == 0) {
        return NULL;
    }

    struct dentryblock *dBlock;
    int result = cache_get_block(theCache, idx* KTFS_BLKSZ , (void **) &dBlock);
    if (result < 0) {
        return NULL;
    }


    struct ktfs_dir_entry *found = NULL;

    for (int i = 0; i < DENTRIES_PER_BLOCK; i++) {
        if (dBlock->entrys[i].inode == 0) {
           
            continue;
        }

        if (strcmp(name, dBlock->entrys[i].name) == 0) {
            found = kmalloc(sizeof(struct ktfs_dir_entry));
            if (found != NULL) {
                *found = dBlock->entrys[i];
            }
            break;
        }
    }

    cache_release_block(theCache, (void *) dBlock, 0);
    return found;
}

struct ktfs_dir_entry* search_indirect_block(uint32_t blocknum, const char *name) {
    if (blocknum == 0)
        return NULL;

    uint32_t *ptrs;
    int ret = cache_get_block(theCache, blocknum* KTFS_BLKSZ, (void**)&ptrs);
    if (ret < 0)
        return NULL;

    struct ktfs_dir_entry *found = NULL;

    for (int i = 0; i < PTRS_PER_BLOCK; i++) {
        uint32_t direct_block = ptrs[i];

        if (direct_block == 0)
            continue;

        found = search_direct_block(direct_block + data_block_offset, name);
        if (found != NULL)
            break;
    }

    cache_release_block(theCache, ptrs, 0);
    return found;
}

struct ktfs_dir_entry* search_dindirect_block(uint32_t blocknum, const char *name) {
    if (blocknum == 0)
        return NULL;

    uint32_t *indptrs;
    int ret = cache_get_block(theCache, blocknum* KTFS_BLKSZ, (void**)&indptrs);
    if (ret < 0)
        return NULL;

    struct ktfs_dir_entry *found = NULL;

    for (int i = 0; i < PTRS_PER_BLOCK; i++) {
        uint32_t indirect_block = indptrs[i];

        if (indirect_block == 0)
            continue;

        found = search_indirect_block((indirect_block + data_block_offset), name);
        if (found != NULL)
            break;
    }

    cache_release_block(theCache, indptrs, 0);
    return found;
}

/**
 * @brief Opens a file or ls (listing) with the given name and returns a pointer to the uio through
 * the double pointer
 * @param name The name of the file to open or "\" for listing (CP3)
 * @param uioptr Will return a pointer to a file or ls (list) uio pointer through this double
 * pointer
 * @return 0 if open successful, negative error code if error
 */
int ktfs_open(struct filesystem* fs, const char* name, struct uio** uioptr) {
        // FIXME
   
    if (name == NULL || *name == '\0') {
        struct ktfs_listing *ls = kcalloc(1, sizeof(struct ktfs_listing));
        if (!ls) {
            return -ENOMEM;
        }

        ls->next = 0;   // start from first dentry

        // same pattern as fs_open_listing
        *uioptr = uio_init1(&ls->base, &ktfs_listing_uio_intf);
        return 0;
    }

    struct ktfs_dir_entry * entry_to_open = NULL;

    //get the inode block
    struct inodeblock * theInodeBlock;
    int result = get_inode_block(&theInodeBlock);
    if(result < 0){
        return result;
    }

    //get root inode
    struct ktfs_inode * root_inode = &theInodeBlock->entrys[0];
        
    //check the 4 direct blocks
    for (int i=0; i<KTFS_NUM_DIRECT_DATA_BLOCKS; i++){
        entry_to_open = search_direct_block(root_inode->block[i] + data_block_offset, name);
        if (entry_to_open != NULL) {break;}
    }
   

    //check the indirect block
    if (entry_to_open == NULL){
        entry_to_open = search_indirect_block(root_inode->indirect + data_block_offset, name);
    }

    //check the 2 dindirect blocks
    if (entry_to_open == NULL){
        for (int i=0; i<KTFS_NUM_DINDIRECT_BLOCKS; i++){
            entry_to_open = search_dindirect_block(root_inode->dindirect[i] + data_block_offset, name);
            if (entry_to_open != NULL) {break;}
        }
       
    }

    //make sure we found the file
    if (entry_to_open == NULL){
        release_inode_block(theInodeBlock, 0);
        return -ENOENT;
    }

    //allocate the file
    struct ktfs_file * file = kcalloc(1, sizeof(struct ktfs_file));

    lock_init(&(file -> filet_lck));

    file->uio.refcnt = 1;
    file->uio.intf = &ktfs_file_uio_intf;

    file->dir_entry = entry_to_open;
   
    //init file params
    file->pos = 0;
    file->opened = 1;

    //find the file size
    struct ktfs_inode * file_inode = &theInodeBlock->entrys[entry_to_open->inode];
    file->file_size = file_inode->size;

    //add it to the open list
    struct opened_file_list * newFile = kcalloc(1, sizeof(struct opened_file_list));
    newFile->file = file;
    newFile->next = theOpenFileList;
    theOpenFileList = newFile;

    //set created uio to function signature
    *uioptr = (struct uio*)file;

    //release the inode block
    release_inode_block(theInodeBlock, 0);

    return 0;
}

/**
 * @brief Closes the file that is represented by the uio struct
 * @param uio The file io to be closed
 * @return None
 */
void ktfs_close(struct uio* uio) {
    // FIXME
    //cast to a file struct
    struct ktfs_file * file = (struct ktfs_file*)uio;

    //set the file to closed
    file->opened = 0;

    //decrement the refcount
    file->uio.refcnt--;

    //remove it from the list
    struct opened_file_list * iter = theOpenFileList;
    struct opened_file_list * prev = NULL;
    while (iter != NULL){
        if (file == iter->file){
            break;
        }
        prev = iter;
        iter = iter->next;
       
    }

    //remove from the lsit if we found it
    if (iter != NULL){
        //case 1: its at the head
        if (prev == NULL){
            theOpenFileList = iter->next;
            kfree(iter);
        }
        else{
            //case 2: its in the middle/end of list
            prev->next = iter->next;
            kfree(iter);
        }
    }

    //free the entire file if recnt is 0
    if (file->uio.refcnt == 0){
        kfree(file->dir_entry);
        kfree(file);
    }
   
    return;
}

// Helper function to allocate free data block from the bitmap

static int allocate_data_block(void) {
    
    struct ktfs_bitmap *bitmap;
    int bits_per_block = KTFS_BLKSZ * 8;   // 4096 bits per bitmap block

   
    int first_data_block =
        1 +
        theSuperBlock.inode_bitmap_block_count +
        theSuperBlock.bitmap_block_count +
        theSuperBlock.inode_block_count;

    for (int bmp_idx = 0; bmp_idx < theSuperBlock.bitmap_block_count; bmp_idx++) {

        int status = cache_get_block( theCache, (1 + theSuperBlock.inode_bitmap_block_count + bmp_idx)*KTFS_BLKSZ,(void**)&bitmap);

        if (status < 0) return status;

        for (int bit = 0; bit < bits_per_block; bit++) {

            int byte = bit / 8;
            int bitnum = bit % 8;

            if (!(bitmap->bytes[byte] & (1 << bitnum))) {

                bitmap->bytes[byte] |= (1 << bitnum);

                cache_release_block(theCache, bitmap, 1);

                int data_block_number = first_data_block + (bmp_idx * bits_per_block) + bit;

                if (data_block_number >= theSuperBlock.block_count)
                    return -ENOTSUP;

                return data_block_number;
            }
        }

        cache_release_block(theCache, bitmap, 0);
    }

    return -ENOTSUP;  
}
static long read_direct_block(uint32_t idx, void* buf, unsigned long len, unsigned long start){
    //get the data from the cache
    void* dataBlock;
    int result = cache_get_block(theCache, idx* KTFS_BLKSZ, &dataBlock);
    if (result < 0){
        return result;
    }

    //figure out how many bytes we have to read
    unsigned long bytes_to_read = len;
    if (start+len > 512){
        bytes_to_read = 512-start;
    }

    //copy the data from the block to the buffer
    memcpy(buf, (char*)dataBlock+start, bytes_to_read);

    //release the block
    cache_release_block(theCache, dataBlock, 0);

    //return how many bytes we read
    return bytes_to_read;

}

static long write_direct_block(uint32_t idx, void* buf, unsigned long len, unsigned long start){
    //get the data from the cache
    void* dataBlock;
    int result = cache_get_block(theCache, idx * KTFS_BLKSZ, &dataBlock);
    if (result < 0){
        return result;
    }

    //figure out how many bytes we have to write
    unsigned long bytes_to_write = len;
    if (start+len > 512){
        bytes_to_write = 512-start;
    }

    //copy the data from the block to the buffer
    memcpy((char*)dataBlock+start, buf, bytes_to_write);

    //release the block and mark as dirty
    cache_release_block(theCache, dataBlock, 1); // Set the dirty argument as 1

    //return how many bytes we read
    return bytes_to_write;

}


static long read_indirect_block(uint32_t idx, void* buf, unsigned long len, unsigned long start){
    //get the indirect block from the cache
    uint32_t* indirectBlock;
    int result = cache_get_block(theCache, idx * KTFS_BLKSZ, (void**)&indirectBlock);
    //check if error occured
    if (result < 0){
        return result;
    }

    //figure out which direct data block we need to read from
    //each direct block is 512 bytes
    unsigned long block_to_read_from = start / 512;

    //now we need to figure out where in that block we start reading from
    unsigned long byte_to_read_from = start % 512;

    //keep track of how many bytes we've read
    long total = 0;

    //start reading direct blocks from that block
    for (unsigned long i =block_to_read_from; i < 128 && total < len; i++){
        uint32_t blockIdx = indirectBlock[i];

        //make sure the block is allocated
        if (blockIdx == 0 && i != 0){ break;}

        //figure out how many bytes we still need to read
        unsigned long bytes_left_to_read = len - total;

        //read from this direct block
        long bytes_read = read_direct_block(blockIdx + data_block_offset, (char*)buf + total, bytes_left_to_read, byte_to_read_from);

        //make sure no errors occured
        if (bytes_read < 0){
            cache_release_block(theCache, (void*)indirectBlock, 0);
            return bytes_read;
        }

        //update total
        total += bytes_read;

        //start at 0 for next block
        byte_to_read_from = 0;
    }
    //release the block
    cache_release_block(theCache, (void*)indirectBlock, 0);

    //return how many bytes we read
    return total;
}

static long write_indirect_block(uint32_t idx, void* buf, unsigned long len, unsigned long start){
    //get the indirect block from the cache
    uint32_t* indirectBlock;
    int result = cache_get_block(theCache, idx * KTFS_BLKSZ, (void**)&indirectBlock);
    //check if error occured
    if (result < 0){
        return result;
    }

    //figure out which direct data block we need to write to
    //each direct block is 512 bytes
    unsigned long block_to_write_to = start / 512;

    //now we need to figure out where in that block we start reading from
    unsigned long byte_to_write_at = start % 512;

    //keep track of how many bytes we've read
    long total = 0;

    //start reading direct blocks from that block
    for (unsigned long i =block_to_write_to; i < 128 && total < len; i++){
        uint32_t blockIdx = indirectBlock[i];

        // //make sure the block is allocated
        // if (blockIdx == 0 && i != 0){ break;} // Might want to allocate blocks here
        if (blockIdx == 0) {
            int new_blk = allocate_data_block();
            if (new_blk < 0) {
                cache_release_block(theCache, indirectBlock, 1);
                return new_blk;
            }

            indirectBlock[i] = new_blk - data_block_offset;

            // mark this indirect block as dirty
            cache_release_block(theCache, indirectBlock, 1);

            // re-acquire after modification
            cache_get_block(
                theCache,
                idx * KTFS_BLKSZ,
                (void **)&indirectBlock
            );

            blockIdx = indirectBlock[i];
        }
        //figure out how many bytes we still need to write
        unsigned long bytes_left_to_write = len - total;

        //read from this direct block
        //long bytes_written = (blockIdx, (char*)buf + total, bytes_left_to_read, byte_to_read_from);

        long bytes_written = write_direct_block(blockIdx + data_block_offset, (char*)buf + total, bytes_left_to_write, byte_to_write_at);

        //make sure no errors occured
        if (bytes_written < 0){
            cache_release_block(theCache, (void*)indirectBlock, 1); // Mark dirty
            return bytes_written;
        }

        //update total
        total += bytes_written;

        //start at 0 for next block
        byte_to_write_at = 0;
    }
    //release the block and mark as dirty
    cache_release_block(theCache, (void*)indirectBlock, 1);

    //return how many bytes we wrote
    return total;
}

static long read_dindirect_block(uint32_t idx, void* buf, unsigned long len, unsigned long start){
    //get the dindirect block from cache
    uint32_t* dindirectBlock;
    int result = cache_get_block(theCache, idx * KTFS_BLKSZ, (void**)&dindirectBlock);
    if (result < 0){
        return result;
    }

    //figure out which indirect data block we need to read from
    //each direct block covers 128 * 512 = 65536 bytes
    unsigned long block_to_read_from = start / 65536;

    //now we need to figure out where in that block we start reading from
    unsigned long byte_to_read_from = start % 65536;

    //keep track of how many bytes we've read
    long total = 0;

    //start reading direct blocks from that block
    for (unsigned long i =block_to_read_from; i < 128 && total < len; i++){
        uint32_t blockIdx = dindirectBlock[i];

        //make sure the block is allocated
        if (blockIdx == 0 && i != 0){ break;}

        //figure out how many bytes we still need to read
        unsigned long bytes_left_to_read = len - total;

        //read from this indirect block
        long bytes_read = read_indirect_block(blockIdx + data_block_offset, (char*)buf + total, bytes_left_to_read, byte_to_read_from);

        //make sure no errors occured
        if (bytes_read < 0){
            cache_release_block(theCache, (void*)dindirectBlock, 0);
            return bytes_read;
        }

        //update total
        total += bytes_read;

        //start at 0 for next block
        byte_to_read_from = 0;
    }    
    //release the block
    cache_release_block(theCache, (void*)dindirectBlock, 0);

    //return how many bytes we read
    return total;
}

static long write_dindirect_block(uint32_t idx, void* buf, unsigned long len, unsigned long start){
    //get the dindirect block from cache
    uint32_t* dindirectBlock;
    int result = cache_get_block(theCache, idx * KTFS_BLKSZ, (void**)&dindirectBlock);
    if (result < 0){
        return result;
    }

    //figure out which indirect data block we need to read from
    //each direct block covers 128 * 512 = 65536 bytes
    unsigned long block_to_write_to = start / 65536;

    //now we need to figure out where in that block we start reading from
    unsigned long byte_to_write_at = start % 65536;

    //keep track of how many bytes we've read
    long total = 0;
    //start reading direct blocks from that block
    for (unsigned long i = block_to_write_to; i < 128 && total < len; i++){
        uint32_t blockIdx = dindirectBlock[i];

        //make sure the block is allocated
        // if (blockIdx == 0 && i != 0){ break;} // Might have to allocate blocks here
        if (blockIdx == 0) {
            int new_ind = allocate_data_block();
            if (new_ind < 0) {
                cache_release_block(theCache, dindirectBlock, 1);
                return new_ind;
            }

            // store pointer in dindirect table
            dindirectBlock[i] = new_ind - data_block_offset;

            // mark as dirty and flush table
            cache_release_block(theCache, dindirectBlock, 1);

            // initialize the new indirect block to all zeros
            uint32_t *tbl;
            cache_get_block(theCache, new_ind * KTFS_BLKSZ, (void**)&tbl);
            memset(tbl, 0, KTFS_BLKSZ);
            cache_release_block(theCache, tbl, 1);

            // re-acquire original dindirect block
            cache_get_block(theCache, idx * KTFS_BLKSZ, (void**)&dindirectBlock);

            blockIdx = dindirectBlock[i];
        }
        //figure out how many bytes we still need to write
        unsigned long bytes_left_to_write = len - total;

        //read from this indirect block
        long bytes_written = write_indirect_block(blockIdx + data_block_offset, (char*)buf + total, bytes_left_to_write, byte_to_write_at);

        //make sure no errors occured
        if (bytes_written < 0){
            cache_release_block(theCache, (void*)dindirectBlock, 1); // Mark as dirty and release the block is there is an error
            return bytes_written;
        }

        //update total
        total += bytes_written;

        //start at 0 for next block
        byte_to_write_at = 0;
    }    
    //release the block and mark as dirty
    cache_release_block(theCache, (void*)dindirectBlock, 1);

    //return how many bytes we read
    return total;
}



/**
 * @brief Reads data from file attached to uio into provided argument buffer
 * @param uio uio of file to be read
 * @param buf Buffer to be filled
 * @param len Number of bytes to read
 * @return Number of bytes read if successful, negative error code if error
 */
long ktfs_fetch(struct uio* uio, void* buf, unsigned long len) {
    // FIXME
    //get the file object
    struct ktfs_file * file = (struct ktfs_file*)uio;

    lock_acquire(&(file -> filet_lck));

    //make sure the file is open
    if (file->opened == 0){
        lock_release(&(file -> filet_lck));
        return -EBADFD;
    }

    //get the inode block
    struct inodeblock * theInodeBlock;
    int result = get_inode_block(&theInodeBlock);
    if (result < 0){
        lock_release(&(file -> filet_lck));
        return result;
    }

    //get the inode number
    uint16_t inodeNum = file->dir_entry->inode;

    //get the correct entry
    struct ktfs_inode * file_inode = &theInodeBlock->entrys[inodeNum];

    //check if the request is to read past the end of the file
    //unsigned long bytes_available = file->file_size - file->pos;
    unsigned long bytes_available = 0;
    if ((*file).pos < (*file).file_size){
        bytes_available = file->file_size - file->pos;
    }

    unsigned long bytes_to_read;
    if (bytes_available < len) {
        bytes_to_read = bytes_available;
    } else {
        bytes_to_read = len;
    }
   

    //make sure we haven't reached the end of the file
    if (bytes_to_read == 0){
        release_inode_block(theInodeBlock, 0);
        lock_release(&(file -> filet_lck));
        return 0;
    }

    //keep track of how many bytes we've read
    long total = 0;

    //there are 3 cases we must consider while keeping in mind the length
    //case 1: reading from direct blocks (bytes 0-2047)
    if (file->pos + total < KTFS_NUM_DIRECT_DATA_BLOCKS * 512 && total < bytes_to_read){
        //figure out which of the 4 blocks we need to start from
        unsigned long block_to_read_from = file->pos / 512;

        //figure out where in the block we want to start from
        unsigned long byte_to_read_from = file->pos % 512;

        //call get read on each block remaining
        for (unsigned long i=block_to_read_from; i<KTFS_NUM_DIRECT_DATA_BLOCKS && (unsigned long)total < bytes_to_read; i++){
            uint32_t blockIdx = file_inode->block[i];

            //make sure the block is allocated
            if (blockIdx == 0 && i != 0){ break;}

            //figure out how many bytes we still need to read
            unsigned long bytes_left_to_read = bytes_to_read - total;

            //read from this direct block
            long bytes_read = read_direct_block(blockIdx + data_block_offset, (char*)buf + total, bytes_left_to_read, byte_to_read_from);

            //make sure no errors occured
            if (bytes_read < 0){
                release_inode_block(theInodeBlock, 0);
                lock_release(&(file -> filet_lck));
                return bytes_read;
            }

            //update total
            total += bytes_read;

            //start at 0 for next block
            byte_to_read_from = 0;
        }

    }
    //case 2: reading from indirect block (bytes 2048-67583)
    if (file->pos + total < (KTFS_NUM_DIRECT_DATA_BLOCKS + 128) * 512 && (unsigned long)total < bytes_to_read){
        //get the current pos
        unsigned long current_pos = file->pos + total;
       
        //figure out where we are in the indirect block
        unsigned long indirect_start = current_pos - (KTFS_NUM_DIRECT_DATA_BLOCKS * 512);

        //figure out how many bytes we still need to read
        unsigned long bytes_left_to_read = bytes_to_read - total;

        //read from this direct block
        long bytes_read = read_indirect_block(file_inode->indirect + data_block_offset, (char*)buf + total, bytes_left_to_read, indirect_start);

        //make sure no errors occured
        if (bytes_read < 0){
            release_inode_block(theInodeBlock, 0);
            lock_release(&(file -> filet_lck));
            return bytes_read;
        }

        //update total
        total += bytes_read;
    }
    //case 3: reading from double indirect blocks (bytes 67584+)
    if ((unsigned long)total < bytes_to_read){
        //get the current pos
        unsigned long current_pos = file->pos + total;

        //figure out where we are in the dindirect block
        unsigned long begining = (KTFS_NUM_DIRECT_DATA_BLOCKS + 128) * 512;
        unsigned long dindirect_start = current_pos - begining;

        //figure out which of the 2 blocks we need to start from
        unsigned long start_block = dindirect_start / (128 * 512);

        //call get read on each block remaining
        for (unsigned long i=start_block; i<KTFS_NUM_DINDIRECT_BLOCKS && total < bytes_to_read; i++){
            uint32_t blockIdx = file_inode->dindirect[i];
       
            //make sure the block is allocated
            if (blockIdx == 0 && i != 0){ break;}

            //figure out where in the block we want to start from
            unsigned long byte_to_read_from = 0;
            if (i == start_block){
                byte_to_read_from = dindirect_start % (128 * 512);
            }

            //figure out how many bytes we still need to read
            unsigned long bytes_left_to_read = bytes_to_read - total;

            //read from the dindirect block
            long bytes_read = read_dindirect_block(blockIdx + data_block_offset, (char*)buf + total, bytes_left_to_read, byte_to_read_from);

             //check for error
            if (bytes_read < 0){
                release_inode_block(theInodeBlock, 0);
                lock_release(&(file -> filet_lck));
                return bytes_read;
            }

            //update total
            total += bytes_read;
        }    
    }
   
    //update the file position
    file->pos += total;

    //release the inode block
    release_inode_block(theInodeBlock, 0);

    lock_release(&(file -> filet_lck));

    //return how many bytes we read
    return total;
}


/**
 * @brief Write data from the provided argument buffer into file attached to uio
 * @param uio The file to be written to
 * @param buf The buffer to be read from
 * @param len Number of bytes to write from the buffer to the file
 * @return Number of bytes written from the buffer to the file system if sucessful, negative error
 * code if error
 */
long ktfs_store(struct uio* uio, const void* buf, unsigned long len) {
    // FIXME

    struct ktfs_file * file = (struct ktfs_file *) uio; // Typecast uio into ktfs file * pointer

    lock_acquire(&((*(file)).filet_lck)); // Acquire the lock to prevent concurrency issues

    int open_status = (*(file)).opened;

    long total = 0; // The number of bytes we have written

    if (!open_status) { // Check if file has already been opened
        lock_release(&((*(file)).filet_lck));
        return -EBADFD;
    } else {
        // Get the inode block
        struct inodeblock *theInodeBlock;

        // Store whether it was successful in the status variable
        int status = get_inode_block(&(theInodeBlock));

        if(status < 0) { // Check if it worked correctly
            lock_release(&((*(file)).filet_lck));
            return status; // Release the same negative error code
        } else {
            // It passed

            int inodeNum = ((*file).dir_entry) -> inode;

            struct ktfs_inode * file_inode = &((*theInodeBlock).entrys[inodeNum]);

            int dirty = 0; // To track if inode is dirty or not

            // Write to the blocks

            // Writing to direct blocks (Bytes 0-2047)
            // If current position is in direct block range and nothing is written yet
            unsigned long file_pos = (*file).pos;
           
            if ((file_pos + total) < (KTFS_NUM_DIRECT_DATA_BLOCKS * 512) && (unsigned long) total < len) {
                file_pos = (*file).pos;

                // Calculate which direct block to start from
                unsigned long block_to_write_to = file_pos / 512;

                file_pos = (*file).pos;

                unsigned long byte_to_write_at = file_pos % 512;

                unsigned long writing_block_at = block_to_write_to;

                while (writing_block_at < KTFS_NUM_DIRECT_DATA_BLOCKS && total < len) {
                    if(!(*file_inode).block[writing_block_at]) {
                        // Allocate new block
                        int new_blk = allocate_data_block();
                        if(new_blk >= 0) { // If block was allocated properly

                            // Store the block index inside the inode
                            (*file_inode).block[writing_block_at] = new_blk - data_block_offset;                            

                            // Mark inode block as dirty
                            dirty = 1;
                        } else {
                            release_inode_block(theInodeBlock, 0);
                            lock_release(&((*file).filet_lck));
                            return new_blk;
                        }
                    }

                    int blockIdx = (*file_inode).block[writing_block_at];

                    // Calculagte how many bytes are left
                    unsigned long bytes_left_to_write = len - total;

                    long bytes_written = write_direct_block(blockIdx + data_block_offset, (char *) buf + total, bytes_left_to_write, byte_to_write_at);

                    if (bytes_written < 0) {
                        release_inode_block(theInodeBlock, dirty);
                        lock_release(&((*file).filet_lck));
                        return bytes_written;
                    } else {
                        total += bytes_written;
                        byte_to_write_at = 0;
                        writing_block_at++;
                    }

                }
            }

            file_pos = (*file).pos;

            // Writing to indirect blocks

            if ((file_pos + total) < ((KTFS_NUM_DIRECT_DATA_BLOCKS + 128) * 512) && (unsigned long) total < len) {
                file_pos = (*file).pos;
                unsigned long curr_pos = file_pos + total;
                unsigned long indirect_start = curr_pos - (KTFS_NUM_DIRECT_DATA_BLOCKS * 512);
                unsigned long bytes_left_to_write = len - total;

                if(!(file_inode -> indirect)) {
                    // Allocate indirect block

                    int new_indirect_blk = allocate_data_block();

                    if (new_indirect_blk < 0) {
                        release_inode_block(theInodeBlock, dirty);
                        lock_release(&((*file).filet_lck));
                        return new_indirect_blk;
                    } else {
                        // Store the block index in the inode

                        (*file_inode).indirect = new_indirect_blk - data_block_offset;

                        dirty = 1; // Mark inode block as dirty

                        // Init the indirect block by setting all entries to 0
                       
                        int * indirect_blk_idx;

                        int result = cache_get_block(theCache, (new_indirect_blk) * KTFS_BLKSZ, (void **) &indirect_blk_idx);

                        if (result >= 0) {
                            // Zero out the indirect block to make all pointers point to 0
                            memset(indirect_blk_idx, 0, KTFS_BLKSZ);

                            // Release them all and mark them dirty

                            cache_release_block(theCache, (void *) indirect_blk_idx, 1);
                        } else {
                            release_inode_block(theInodeBlock, 1); // It is dirty
                            lock_release(&((*file).filet_lck));
                            return result;
                        }
                    }
                }

                long bytes_written = write_indirect_block((*file_inode).indirect + data_block_offset, (char *)buf + total, bytes_left_to_write, indirect_start);

                if(bytes_written < 0) {
                    release_inode_block(theInodeBlock, dirty);
                    lock_release(&((*file).filet_lck));
                    return bytes_written;
                }

                total += bytes_written;
            }

            // Writing to doubly indirect blocks

            file_pos = (*file).pos;

            if (total < len) {
                file_pos = (*file).pos;

                unsigned long curr_pos = file_pos + total;
                unsigned long beginning = ((KTFS_NUM_DIRECT_DATA_BLOCKS + 128) * 512);
                unsigned long dindirect_start = curr_pos - beginning;
                unsigned long start_block = dindirect_start / (128*512);

                unsigned long block_num = start_block;

                while (block_num < KTFS_NUM_DINDIRECT_BLOCKS && total < len) {
                    if (!(*file_inode).dindirect[block_num]) {
                        // Allocate dindirect block
                        int new_dindirect_blk_idx = allocate_data_block();

                        if (new_dindirect_blk_idx >= 0) {
                            // Store the blk idx in inode
                            (*file_inode).dindirect[block_num] = new_dindirect_blk_idx - data_block_offset;

                            // Mark as dirty
                            dirty = 1;

                            // Init the dindirect blk

                            int * dindirect_blk;

                            int result = cache_get_block(theCache, (new_dindirect_blk_idx) * KTFS_BLKSZ, (void **) &(dindirect_blk));

                            if (result >= 0) {

                                // Zero out the dindirect block to initialize the pointers to 0
                                memset(dindirect_blk, 0, KTFS_BLKSZ);

                                // Release and mark as dirty

                                cache_release_block(theCache, (void *) dindirect_blk, 1);
                            } else {
                                release_inode_block(theInodeBlock, 1);
                                lock_release(&((*file).filet_lck));
                                return result;
                            }

                        } else {
                            release_inode_block(theInodeBlock, dirty);
                            lock_release(&((*file).filet_lck));
                            return new_dindirect_blk_idx;
                        }
                    }

                    int blockIdx = (*file_inode).dindirect[block_num];
                    unsigned long byte_to_write_at = 0;
                   
                    if (block_num == start_block) {
                        byte_to_write_at = dindirect_start % (128*512);
                    }

                    unsigned long bytes_left_to_write = len - total;

                    long bytes_written = write_dindirect_block(blockIdx + data_block_offset, (char *)buf + total, bytes_left_to_write, byte_to_write_at);

                    if (bytes_written < 0) {
                        release_inode_block(theInodeBlock, dirty);
                        lock_release(&((*file).filet_lck));
                        return bytes_written;
                    } else {
                        total += bytes_written;
                        block_num++;
                    }
                }
            }

            // Update file size if we wrote past the end
            // if ((*file).pos + total > (*file).file_size){
                
            //     (*file).file_size = (*file).pos + total;
            //     (*file_inode).size = (*file).file_size;
            //     // Mark inode block as dirty since we modified it
            //     dirty = 1;
            // }

            // Update file position
            (*file).pos += total;

            //setend functionality. 
            unsigned long long new_end = (*file).pos;
            if(new_end> (*file).file_size){
                int x = ktfs_cntl((struct uio*)file, FCNTL_SETEND, &new_end);
                if (x < 0) {
                    release_inode_block(theInodeBlock, dirty);
                    lock_release(&((*file).filet_lck));
                    return x;
                }
                dirty =1;
            }
            // Release the inode block
            release_inode_block(theInodeBlock, dirty);

            lock_release(&((*file).filet_lck));

            return total;
        }
    }
}

// Helper func to free data blk in bitmap
static void free_data_block(uint32_t blk_num) {
    int bits_per_block = KTFS_BLKSZ * 8;

    int first_data_block = 1 + theSuperBlock.inode_bitmap_block_count + theSuperBlock.bitmap_block_count + theSuperBlock.inode_block_count;

    if (blk_num < (uint32_t)first_data_block ||
        blk_num >= theSuperBlock.block_count) {
        return;
    }
    // compute relative index in bitmap
    uint32_t rel = blk_num - first_data_block;

    int bmp_start_blk = 1 + theSuperBlock.inode_bitmap_block_count;

    int bitmap_blk_idx = rel / bits_per_block;
    int bit_idx        = rel % bits_per_block;

    struct ktfs_bitmap *bmp;

    int status = cache_get_block(
        theCache,
        (bmp_start_blk + bitmap_blk_idx) * KTFS_BLKSZ,
        (void **)&bmp
    );

    if (status < 0)
        return;

    int byte_idx = bit_idx / 8;
    int bit_num  = bit_idx % 8;

    bmp->bytes[byte_idx] &= ~(1 << bit_num);

    cache_release_block(theCache, bmp, 1);
}

// Helper to free inode in inode bmp

static void free_inode(uint16_t inode_num) {
    // Find where inode bmp starts

    int inode_bmp_s_blk = 1;

    // Find the inode bmp blk that has inode num

    int bmp_blk_idx = inode_num;
    bmp_blk_idx /= (8 * KTFS_BLKSZ);

    int bit_idx = inode_num % (8 * KTFS_BLKSZ);

    struct ktfs_bitmap * bmp;

    int status = cache_get_block(theCache, (inode_bmp_s_blk + bmp_blk_idx) * (KTFS_BLKSZ), (void **) &(bmp));

    if (status < 0) {
        return;
    } else {
        // Block was retrieved
        int byte_idx = bit_idx;
        byte_idx /= 8;

        int bit_num = bit_idx % 8; // Get the block and bit offset
       
        (*bmp).bytes[byte_idx] &= ~(1 << bit_num); // Bitmask the appropriate bit

        cache_release_block(theCache, (void *) bmp, 1); // Release and mark as dirty since we modified it.
    }
}

static void free_inode_blocks(struct ktfs_inode * file_inode) {
   
    // Free the direct blocks

    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) {
        if(!(*file_inode).block[i]) {
            continue;            
        } else {
            free_data_block((*file_inode).block[i] + data_block_offset);
            (*file_inode).block[i] = 0;
        }
    }

    // Free indirect blocks

    if ((*file_inode).indirect) {
        uint32_t * indirBlk = NULL;

        int status = cache_get_block(theCache, ((*file_inode).indirect + data_block_offset) * KTFS_BLKSZ, (void **) &(indirBlk));

        if (status > -1) {
            // Free all the data blocks

            for (int i = 0; i < 128; i++) {
                if(indirBlk[i]) {
                    free_data_block(indirBlk[i] + data_block_offset);
                } else {
                    continue;
                }
            }

            cache_release_block(theCache, (void *) indirBlk, 0);
        }

        // Free the indirect block

        free_data_block((*file_inode).indirect + data_block_offset);
        (*file_inode).indirect = 0;
    }

    // Free dindirect blocks and all of the blocks that they point to

    for(int i = 0; i < KTFS_NUM_DINDIRECT_BLOCKS; i++) {
        if((*file_inode).dindirect[i]) {
            uint32_t * dindirect_blk;
            int status = cache_get_block(theCache, ((*file_inode).dindirect[i] + data_block_offset) * KTFS_BLKSZ, (void **) &(dindirect_blk));

            if(status > -1) {
                // Iterate through all the indirect blocks inside this dindirect block
                for (int j = 0; j < 128; j++) {
                    uint32_t * indirectBlk;
                    if(!dindirect_blk[j]) {
                        continue;
                    } else {
                        int stat = cache_get_block(theCache, (dindirect_blk[j] + data_block_offset) * KTFS_BLKSZ , (void **) &(indirectBlk));
                        // Free the data blocks
                        if (stat < 0) {
                            free_data_block(dindirect_blk[j] + data_block_offset);
                            continue;
                        } else {
                            for (int k = 0; k < 128; k++) {
                                if(!indirectBlk[k]) {
                                    continue;
                                }
                                if(indirectBlk[k]) {
                                    free_data_block(indirectBlk[k] + data_block_offset);
                                }
                            }

                            cache_release_block(theCache, (void *) indirectBlk, 0);
                        }

                        // Free the indirect blk
                        free_data_block(dindirect_blk[j] + data_block_offset);
                    }
                }

                // Release the cache block
                cache_release_block(theCache, (void *) dindirect_blk, 0);
            }

            // Free the dindirect block
            free_data_block((*file_inode).dindirect[i] + data_block_offset);

            (*file_inode).dindirect[i] = 0;
        }
    }
}

// Helper function to find dentry in a direct block and return its location
static struct ktfs_dir_entry* find_dir_block_for_del(int idx, const char * name, struct dentryblock ** out_block, int * out_entry_idx) {
    if (idx == 0) {
        return NULL;
    } else {
        struct dentryblock * dblk;

        int status = cache_get_block(theCache, (idx + data_block_offset) * KTFS_BLKSZ , (void **) &(dblk));

        if (status < 0) {
            return NULL;
        } else {
            for (int i = 0; i < dentrySize; i++) {
                // Name has matched
                if (!(strcmp(name, (*dblk).entrys[i].name))) {

                    // Found the block and now return the location via output parameters

                    *(out_block) = dblk;
                    *(out_entry_idx) = i;

                    // Returning pointer to the entry
                    return &((*dblk).entrys[i]);
                }
            }

            // Nothing found

            cache_release_block(theCache, (void *) dblk, 0);
           
            return NULL;
        }
    }
}

// Helper function to find dentry in an indirect block and return its location
static struct ktfs_dir_entry* find_indir_block_for_del(int idx, const char * name, struct dentryblock ** out_block, int * out_entry_idx) {
    if (idx * idx == 0) {
        return NULL;
    } else {
        uint32_t * indirectBlk;

        // Get the indirect blk from cache

        int status = cache_get_block(theCache, (idx + data_block_offset) * KTFS_BLKSZ, (void **) &(indirectBlk));

        if (status < 0) {
            return NULL;
        } else {
            for (int i = 0; i < 128; i++) {
                uint32_t blkIdx = indirectBlk[i];

                if (i != 0 && blkIdx == 0) {
                    continue;
                } else {
                    struct ktfs_dir_entry * found = find_dir_block_for_del(blkIdx + data_block_offset, name, out_block, out_entry_idx);

                    if(found) {
                        cache_release_block(theCache, (void *) indirectBlk, 0);
                        return found;
                    }
                }
            }
        }

        cache_release_block(theCache, (void *) indirectBlk, 0);

        return NULL;
    }
}

// Helper function to find dentry in a dindirect block and return its location
static struct ktfs_dir_entry* find_dindir_block_for_del(int idx, const char * name, struct dentryblock ** out_block, int * out_entry_idx) {
    if (!idx) {
        return NULL;
    } else {
        uint32_t * dindirectBlk;

        int status = cache_get_block(theCache, (idx + data_block_offset) * KTFS_BLKSZ, (void **) &(dindirectBlk));

        if (status < 0) {
            return NULL;
        } else {
            for (int i = 0; i < 128; i++) {
                uint32_t blkIdx = dindirectBlk[i];

                if(i != 0 && blkIdx == 0) {
                    continue;
                } else {
                    struct ktfs_dir_entry * found = find_indir_block_for_del(blkIdx + data_block_offset, name, out_block, out_entry_idx);

                    if (found) {
                        cache_release_block(theCache, (void *) dindirectBlk, 0);

                        return found;
                    }
                }
            }

            cache_release_block(theCache, (void *) dindirectBlk, 0);

            return NULL;
        }
    }
}

static struct ktfs_dir_entry * find_entry_for_del(struct ktfs_inode * root_inode, const char * name, struct dentryblock ** out_block, int * out_entry_idx) {
    struct ktfs_dir_entry * found = NULL;

    // Check the direct blocks

    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) {
        if ((*root_inode).block[i]) {
            found = find_dir_block_for_del((*root_inode).block[i] , name, out_block, out_entry_idx);

            if(found) {
                return found;
            } else {
                continue;
            }
        }
    }
   
    if ((*root_inode).indirect) { // Check indirect blocks
        found = find_indir_block_for_del((*root_inode).indirect , name, out_block, out_entry_idx);

        if(found) {
            return found;
        }
    }

    for (int i = 0; i < KTFS_NUM_DINDIRECT_BLOCKS; i++) {
        if ((*root_inode).dindirect[i]) {
            found = find_dindir_block_for_del((*root_inode).dindirect[i] , name, out_block, out_entry_idx);

            if(found) {
                return found;
            } else {
                continue;
            }
        } else {
            continue;
        }
    }

    // No entry found

    return NULL;
}


///HELPERS FOR THE CREATE FUNCTION UNDER HERE

static int allocate_inode(void){
     int inode_bitmap_start = 1;  
    struct ktfs_bitmap* bitmap;

    int bits_per_block = KTFS_BLKSZ * 8;  

    for (int block = 0; block < theSuperBlock.inode_bitmap_block_count; block++) {

        int status = cache_get_block( theCache, (inode_bitmap_start + block) * KTFS_BLKSZ, (void**)&bitmap);

        if (status < 0) return status;

        for (int i = 0; i < bits_per_block; i++) {

            int byte = i / 8;
            int bit  = i % 8;

            if ((bitmap->bytes[byte] & (1 << bit)) == 0) {

                bitmap->bytes[byte] |= (1 << bit);

                cache_release_block(theCache, bitmap, 1);

                return block * bits_per_block + i;
            }
        }

        cache_release_block(theCache, bitmap, 0);
    }
    return -ENOTSUP;
}
static int insert_to_dir(struct ktfs_inode* root, const char* name, uint16_t inodeNum){

    struct dentryblock* dblk;
    uint32_t* inodeptr;
    int dirtyInode = 0;

   
    for(int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++){
        if(root->block[i] == 0){
            
            if (!(i == 0 && root->size > 0)) {
                int new_block = allocate_data_block();
                if(new_block < 0) return new_block;

                root->block[i] = new_block - data_block_offset;
                dirtyInode = 1;

                int cacheBlk = cache_get_block(theCache, new_block, (void**)&dblk);
                if(cacheBlk < 0) return cacheBlk;
                memset(dblk, 0, sizeof(struct dentryblock));
                cache_release_block(theCache, dblk, 1);
            }
           
        }

        int cacheBlk = cache_get_block(theCache, (root->block[i] + data_block_offset) * KTFS_BLKSZ , (void**)&dblk);
        if(cacheBlk < 0){
            return cacheBlk;
        }

        for(int j = 0; j < KTFS_BLKSZ / sizeof(struct ktfs_dir_entry); j++){
            if(dblk->entrys[j].inode == 0){
                dblk->entrys[j].inode = inodeNum;
                strncpy(dblk->entrys[j].name, name, KTFS_MAX_FILENAME_LEN);
                dblk->entrys[j].name[KTFS_MAX_FILENAME_LEN] = '\0';

                root->size += sizeof(struct ktfs_dir_entry);
                dirtyInode = 1;


                cache_release_block(theCache, dblk, 1);
                return dirtyInode;
            }
        }

        cache_release_block(theCache, dblk, 0);
    }

   
    if(root->indirect == 0){

        int new_ind = allocate_data_block();
        if(new_ind < 0){
            return new_ind;
        }

        root->indirect = new_ind - data_block_offset;
        dirtyInode = 1;

        int cacheBlk2 = cache_get_block(theCache, new_ind * KTFS_BLKSZ, (void**)&inodeptr);
        if(cacheBlk2 < 0) return cacheBlk2;
        memset(inodeptr, 0, KTFS_BLKSZ);
        cache_release_block(theCache, inodeptr, 1);
    }

    int r = cache_get_block(theCache, (root->indirect + data_block_offset) * KTFS_BLKSZ, (void**)&inodeptr);
    if(r < 0) return r;

    for(int i = 0; i < KTFS_BLKSZ / sizeof(uint32_t); i++){
        if(inodeptr[i] == 0){
            int newblk = allocate_data_block();
            if(newblk < 0){
                cache_release_block(theCache, inodeptr, 0);
                return newblk;
            }

            inodeptr[i] = newblk - data_block_offset;
            dirtyInode = 1;

            int r2 = cache_get_block(theCache, newblk * KTFS_BLKSZ, (void**)&dblk);
            if(r2 < 0){
                cache_release_block(theCache, inodeptr, 1);
                return r2;
            }
            memset(dblk, 0, sizeof(struct dentryblock));
            cache_release_block(theCache, dblk, 1);
        }

        r = cache_get_block(theCache, (inodeptr[i] + data_block_offset) * KTFS_BLKSZ, (void**)&dblk);
        if(r < 0){
            cache_release_block(theCache, inodeptr, 0);
            return r;
        }

        for(int e = 0; e < KTFS_BLKSZ / sizeof(struct ktfs_dir_entry); e++){
            if(dblk->entrys[e].inode == 0){
                dblk->entrys[e].inode = inodeNum;
                strncpy(dblk->entrys[e].name, name, KTFS_MAX_FILENAME_LEN);
                dblk->entrys[e].name[KTFS_MAX_FILENAME_LEN] = '\0';
                

                 root->size += sizeof(struct ktfs_dir_entry);
                dirtyInode = 1;


                cache_release_block(theCache, dblk, 1);
                cache_release_block(theCache, inodeptr, 0);
                return dirtyInode;
            }
        }

        cache_release_block(theCache, dblk, 0);
    }

    cache_release_block(theCache, inodeptr, 0);

   
    for (int d = 0; d < KTFS_NUM_DINDIRECT_BLOCKS; d++) {

        if (root->dindirect[d] == 0) {
            int new_dind = allocate_data_block();

            if (new_dind < 0)
                return new_dind;

            root->dindirect[d] = new_dind - data_block_offset;
            dirtyInode = 1;

            int r3 = cache_get_block(theCache, new_dind * KTFS_BLKSZ, (void**)&inodeptr);
            if (r3 < 0)
                return r3;

            memset(inodeptr, 0, KTFS_BLKSZ);
            cache_release_block(theCache, inodeptr, 1);
        }

        int r4 = cache_get_block(theCache,(root->dindirect[d] + data_block_offset) * KTFS_BLKSZ,(void**)&inodeptr);
        if (r4 < 0)
            return r4;

        int num_ptrs = KTFS_BLKSZ / sizeof(uint32_t);

        for (int i = 0; i < num_ptrs; i++) {

            if (inodeptr[i] == 0) {
                int new_ind = allocate_data_block();
                if (new_ind < 0) {
                    cache_release_block(theCache, inodeptr, 0);
                    return new_ind;
                }

                inodeptr[i] = new_ind - data_block_offset;
                dirtyInode = 1;

                uint32_t* lvl2;
                int r5 = cache_get_block(theCache, new_ind * KTFS_BLKSZ, (void**)&lvl2);
                if (r5 < 0) {
                    cache_release_block(theCache, inodeptr, 1);
                    return r5;
                }

                memset(lvl2, 0, KTFS_BLKSZ);
                cache_release_block(theCache, lvl2, 1);
            }

            uint32_t* lvl2;
            int r6 = cache_get_block(theCache,(inodeptr[i] + data_block_offset) * KTFS_BLKSZ, (void**)&lvl2);
            if (r6 < 0) {
                cache_release_block(theCache, inodeptr, 0);
                return r6;
            }

            for (int j = 0; j < num_ptrs; j++) {

                if (lvl2[j] == 0) {
                    int new_blk = allocate_data_block();
                    if (new_blk < 0) {
                        cache_release_block(theCache, lvl2, 0);
                        cache_release_block(theCache, inodeptr, 0);
                        return new_blk;
                    }

                    lvl2[j] = new_blk - data_block_offset;
                    dirtyInode = 1;

                    struct dentryblock* new_dblk;
                    int r7 = cache_get_block(theCache, new_blk * KTFS_BLKSZ, (void**)&new_dblk);
                    if (r7 < 0) {
                        cache_release_block(theCache, lvl2, 1);
                        cache_release_block(theCache, inodeptr, 1);
                        return r7;
                    }

                    memset(new_dblk, 0, sizeof(struct dentryblock));
                    cache_release_block(theCache, new_dblk, 1);
                }

                struct dentryblock* scan_blk;
                int r8 = cache_get_block(theCache,
                                         (lvl2[j] + data_block_offset) * KTFS_BLKSZ,
                                         (void**)&scan_blk);
                if (r8 < 0) {
                    cache_release_block(theCache, lvl2, 0);
                    cache_release_block(theCache, inodeptr, 0);
                    return r8;
                }

                int nentries = KTFS_BLKSZ / sizeof(struct ktfs_dir_entry);
                for (int e = 0; e < nentries; e++) {
                    if (scan_blk->entrys[e].inode == 0) {
                        scan_blk->entrys[e].inode = inodeNum;
                        strncpy(scan_blk->entrys[e].name, name, KTFS_MAX_FILENAME_LEN);
                        scan_blk->entrys[e].name[KTFS_MAX_FILENAME_LEN] = '\0';

                        root->size += sizeof(struct ktfs_dir_entry);
                        dirtyInode = 1;

                        cache_release_block(theCache, scan_blk, 1);
                        cache_release_block(theCache, lvl2, 0);
                        cache_release_block(theCache, inodeptr, 0);
                        return dirtyInode;
                    }
                }

                cache_release_block(theCache, scan_blk, 0);
            }

            cache_release_block(theCache, lvl2, 0);
        }

        cache_release_block(theCache, inodeptr, 0);
    }

    return -ENOTSUP;
}

/**
 * @brief Create a new file in the file system
 * @param fs The file system in which to create the file
 * @param name The name of the file
 * @return 0 if successful, negative error code if error
 */
int ktfs_create(struct filesystem* fs, const char* name) {
    // FIXME

    uint16_t root_inode_num = theSuperBlock.root_directory_inode;
    int inodes_per_block = KTFS_BLKSZ / sizeof(struct ktfs_inode);  
    int block_index      = root_inode_num / inodes_per_block;        // which inode block
    int entry_index      = root_inode_num % inodes_per_block;        // which entry inside the block

   
    int first_inode_block = 1 + theSuperBlock.inode_bitmap_block_count + theSuperBlock.bitmap_block_count;
    int disk_block_num = first_inode_block + block_index;

   
    struct inodeblock* inodeBlk;
    int st = cache_get_block(theCache, disk_block_num* KTFS_BLKSZ, (void**)&inodeBlk);
    if (st < 0){
        cache_release_block(theCache, inodeBlk, 0);
        return st;
    }
   
    struct ktfs_inode* root = &inodeBlk->entrys[entry_index];

   
    struct ktfs_dir_entry* found = NULL;

    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS && !found; i++)
        found = search_direct_block(root->block[i] + data_block_offset, name);

    if (!found)
        found = search_indirect_block(root->indirect + data_block_offset, name);

    for (int i = 0; i < KTFS_NUM_DINDIRECT_BLOCKS && !found; i++)
        found = search_dindirect_block(root->dindirect[i] + data_block_offset, name);

    if (found != NULL) {
        kfree(found);                
        cache_release_block(theCache, inodeBlk, 0);  
        return -EEXIST;
    }

    int new_inode_num = allocate_inode();

    if (new_inode_num < 0) {
        cache_release_block(theCache, inodeBlk, 0);
        return new_inode_num;
    }

   
   
    struct ktfs_inode* new_inode = &inodeBlk->entrys[new_inode_num % inodes_per_block];
    memset(new_inode, 0, sizeof(struct ktfs_inode));


    int dirty = 1;  

    int inserted = insert_to_dir(root, name, (uint16_t)new_inode_num);
   
    if (inserted < 0) {
        cache_release_block(theCache, inodeBlk, dirty);
        return inserted;
    }

    dirty |= inserted;  

   

    // root->size += sizeof(struct ktfs_dir_entry);
    // dirty = 1;   // root inode changed

    cache_release_block(theCache, inodeBlk, dirty);

    return 0;
}

/**
 * @brief Deletes a certain file from the file system with the given name
 * @param fs The file system to delete the file from
 * @param name The name of the file to be deleted
 * @return 0 if successful, negative error code if error
 */
int ktfs_delete(struct filesystem* fs, const char* name) {
    // FIXME

    // Get the inode block

    struct inodeblock * theInodeBlock;
    int status = get_inode_block(&(theInodeBlock));
    if (!(status >= 0)) {
        return status;
    } else {

        // Get root inode
        struct ktfs_inode * root_inode = &((*theInodeBlock).entrys[0]);

        // Find the dentry

        struct dentryblock * dir_blk = NULL;

        int entry_idx = -1;

        struct ktfs_dir_entry * entry = find_entry_for_del(root_inode, name, &(dir_blk),&(entry_idx));

        if(!entry) {
            // File is not found

            release_inode_block(theInodeBlock, 0);

            return -ENOENT;
        } else {
            // Entry found

            // Get the inode number

            uint16_t file_inode_num = (*entry).inode;

            // Check if the file is open

            struct opened_file_list * list = theOpenFileList;

            while(list) {
                if((*list).file && ((*list).file) -> dir_entry) {
                    if ((*list).file->dir_entry->inode == file_inode_num) {
                        // File is open and I cannot delete it

                        cache_release_block(theCache, (void *) dir_blk, 0);

                        release_inode_block(theInodeBlock, 0);

                        return -EBUSY;
                    }
                }

                list = (*list).next;
            }

            // Get the files inode

            struct ktfs_inode * file_inode = &((*theInodeBlock).entrys[file_inode_num]);

            // Free all the data blocks used by the file

            free_inode_blocks(file_inode);

            // Clear the inode

            (*file_inode).size = 0;

            for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) {
                (*file_inode).block[i] = 0;
            }

            (*file_inode).indirect = 0;

            for (int i = 0; i < KTFS_NUM_DINDIRECT_BLOCKS; i++) {
                (*file_inode).dindirect[i] = 0;
            }

            // Free the node in the bitmap

            free_inode(file_inode_num);

            (*dir_blk).entrys[entry_idx].inode = 0;

            memset((*dir_blk).entrys[entry_idx].name, 0, KTFS_MAX_FILENAME_LEN + sizeof(uint8_t)); // File name + null character is set to 0

            // Release the block as dirty

            cache_release_block(theCache, (void *) dir_blk, 1);

            release_inode_block(theInodeBlock, 1);

            return 0;
        }
    }

    return -ENOTSUP;
}

static int ensure_block_for_inode(struct ktfs_inode *node, uint32_t blk_index){
    uint32_t ptrs_per_block = KTFS_BLKSZ / sizeof(uint32_t);

    if (blk_index < KTFS_NUM_DIRECT_DATA_BLOCKS) {
        if (node->block[blk_index] == 0) {
            int newblk = allocate_data_block();
            if (newblk < 0) {
                return newblk;
            }
            node->block[blk_index] = newblk - data_block_offset;
        }
        return 0;
    }

    blk_index -= KTFS_NUM_DIRECT_DATA_BLOCKS;
    if (blk_index < ptrs_per_block) {
        uint32_t *indptr;

        // allocate the indirect block itself if missing
        if (node->indirect == 0) {
            int tblblk = allocate_data_block();
            if (tblblk < 0) return tblblk;

            node->indirect = tblblk - data_block_offset;

            int r = cache_get_block(theCache, tblblk * KTFS_BLKSZ, (void**)&indptr);
            if (r < 0) return r;
            memset(indptr, 0, KTFS_BLKSZ);
            cache_release_block(theCache, indptr, 1);
        }

        int r = cache_get_block(theCache,(node->indirect + data_block_offset) * KTFS_BLKSZ,(void**)&indptr);
        if (r < 0) return r;

        if (indptr[blk_index] == 0) {
            int newblk = allocate_data_block();
            if (newblk < 0) {
                cache_release_block(theCache, indptr, 0);
                return newblk;
            }
            indptr[blk_index] = newblk - data_block_offset;
            cache_release_block(theCache, indptr, 1);
        } else {
            cache_release_block(theCache, indptr, 0);
        }
        return 0;
    }

    blk_index -= ptrs_per_block;   // now index into the dindirect region

    uint32_t blocks_per_dind = ptrs_per_block * ptrs_per_block;
    uint32_t dind_idx= blk_index / blocks_per_dind;
    if (dind_idx >= KTFS_NUM_DINDIRECT_BLOCKS) {
        return -ENOTSUP;
    }
    uint32_t rel= blk_index % blocks_per_dind;
    uint32_t lvl1_idx = rel / ptrs_per_block;
    uint32_t lvl2_idx = rel % ptrs_per_block;

    uint32_t *lvl1;
    if (node->dindirect[dind_idx] == 0) {
        int newtbl = allocate_data_block();
        if (newtbl < 0) return newtbl;
        node->dindirect[dind_idx] = newtbl - data_block_offset;

        int r = cache_get_block(theCache, newtbl * KTFS_BLKSZ, (void**)&lvl1);
        if (r < 0) return r;
        memset(lvl1, 0, KTFS_BLKSZ);
        cache_release_block(theCache, lvl1, 1);
    }

    int r1 = cache_get_block(theCache, (node->dindirect[dind_idx] + data_block_offset) * KTFS_BLKSZ, (void**)&lvl1);
    if (r1 < 0) return r1;

    uint32_t *lvl2;
    if (lvl1[lvl1_idx] == 0) {
        int newtbl2 = allocate_data_block();
        if (newtbl2 < 0) {
            cache_release_block(theCache, lvl1, 0);
            return newtbl2;
        }
        lvl1[lvl1_idx] = newtbl2 - data_block_offset;

        int r = cache_get_block(theCache, newtbl2 * KTFS_BLKSZ, (void**)&lvl2);
        if (r < 0) {
            cache_release_block(theCache, lvl1, 1);
            return r;
        }
        memset(lvl2, 0, KTFS_BLKSZ);
        cache_release_block(theCache, lvl2, 1);
        cache_release_block(theCache, lvl1, 1);
    } else {
        cache_release_block(theCache, lvl1, 0);
    }

    int r2 = cache_get_block(theCache,(lvl1[lvl1_idx] + data_block_offset) * KTFS_BLKSZ, (void**)&lvl2);
    if (r2 < 0) return r2;

    if (lvl2[lvl2_idx] == 0) {
        int newblk = allocate_data_block();
        if (newblk < 0) {
            cache_release_block(theCache, lvl2, 0);
            return newblk;
        }
        lvl2[lvl2_idx] = newblk - data_block_offset;
        cache_release_block(theCache, lvl2, 1);
    } else {
        cache_release_block(theCache, lvl2, 0);
    }

    return 0;
}
/**
 * @brief Given a file io object, a specific command, and possibly some arguments, execute the
 * corresponding functions
 * @details Any commands such as (FCNTL_GETEND, FCNTL_GETPOS, ...) should pass back through the arg
 * variable. Do not directly return the value.
 * @details FCNTL_GETEND should pass back the size of the file in bytes through the arg variable.
 * @details FCNTL_SETEND should set the size of the file to the value passed in through arg.
 * @details FCNTL_GETPOS should pass back the current position of the file pointer in bytes through
 * the arg variable.
 * @details FCNTL_SETPOS should set the current position of the file pointer to the value passed in
 * through arg.
 * @param uio the uio object of the file to perform the control function
 * @param cmd the operation to execute. KTFS should support FCNTL_GETEND, FCNTL_SETEND (CP2),
 * FCNTL_GETPOS, FCNTL_SETPOS.
 * @param arg the argument to pass in, may be different for different control functions
 * @return 0 if successful, negative error code if error
 */
int ktfs_cntl(struct uio* uio, int cmd, void* arg) {
    // FIXME
    //get the file object
    struct ktfs_file * file = (struct ktfs_file*)uio;

    lock_acquire(&(file -> filet_lck));

    if (cmd == FCNTL_GETEND){
        if (arg == NULL) {
            lock_release(&(file -> filet_lck));
            return -EINVAL;
        } else {
            //set arg size of file
            *(unsigned long long*)arg = file->file_size;
        }
    } else if (cmd == FCNTL_GETPOS){
        if (arg == NULL) {
            lock_release(&(file -> filet_lck));
            return -EINVAL;
        } else {
            //set arg as current pos
            *(unsigned long long*)arg = file->pos;
        }
        
    } else if (cmd == FCNTL_SETPOS){
        if (arg == NULL) {
            lock_release(&(file -> filet_lck));
            return -EINVAL;
        } else {
            file->pos = *(unsigned long long*)arg;
        }
    } else if (cmd == FCNTL_SETEND){
        
        if (!arg) {
            lock_release(&(file -> filet_lck));
            return -EINVAL;
        } else {
            // get the inode block
            struct inodeblock * theInodeBlock;
            int result = get_inode_block(&theInodeBlock);
            if (result < 0) {
                lock_release(&(file->filet_lck));
                return result;
            } else {
                struct ktfs_inode * node = &theInodeBlock->entrys[file->dir_entry->inode];

                unsigned long long currSize = file->file_size;
                unsigned long long newSize  = *(unsigned long long*)arg;

                if (newSize > currSize) {
                    uint32_t oldBlocks = (currSize + KTFS_BLKSZ - 1) / KTFS_BLKSZ;
                    uint32_t newBlocks = (newSize  + KTFS_BLKSZ - 1) / KTFS_BLKSZ;

                    for (uint32_t b = oldBlocks; b < newBlocks; b++) {
                        int r = ensure_block_for_inode(node, b);
                        if (r < 0) {
                            // inode contents may be partially updated 
                            release_inode_block(theInodeBlock, 1);
                            lock_release(&(file->filet_lck));
                            return r;
                        }
                    }
                }  else if (newSize == 0 && currSize > 0) {
                    free_inode_blocks(node);
                }
                
                // adjust file position if it lies past new end
                if (newSize < currSize && file->pos > newSize){
                    file->pos = newSize;
                }

                file->file_size = newSize;
                node->size = newSize;

                release_inode_block(theInodeBlock, 1);
            }
        }
    } else {
        lock_release(&(file -> filet_lck));
        return -ENOTSUP;
    }

    lock_release(&(file -> filet_lck));
    return 0;
}

/**
 * @brief Flushes the cache to the backing device
 * @return 0 if flush successful, negative error code if error
 */
void ktfs_flush(struct filesystem* fs) {
    // FIXME
    cache_flush(theCache);
}


/**
 * @brief Closes the listing device represented by the uio pointer
 * @param uio The uio pointer of ls
 * @return None
 */
void ktfs_listing_close(struct uio* uio) {
    // FIXME
    struct ktfs_listing* ls = (struct ktfs_listing*)uio;
    kfree(ls);
}

/**
 * @brief Reads all of the files names in the file system using ls and copies them into the
 * providied buffer
 * @param uio The uio pointer of ls
 * @param buf The buffer to copy the file names to
 * @param bufsz The size of the buffer
 * @return The size written to the buffer
 */
long ktfs_listing_read(struct uio* uio, void* buf, unsigned long bufsz) {
    // FIXME
    struct ktfs_listing* const ls = (struct ktfs_listing*)uio;

    if (bufsz == 0) {
        return 0;
    }

    // Fetch inode block that contains the root inode
    struct inodeblock* inodes;
    int r = get_inode_block(&inodes);
    if (r < 0) return r;

    // struct ktfs_inode* root = &inodes->entrys[theSuperBlock.root_directory_inode];
    struct ktfs_inode* root = &inodes->entrys[0];

    // Total number of dentries in the root directory
    uint32_t total_entries = root->size / sizeof(struct ktfs_dir_entry);

    // Iterate from where we left off last time
    for (uint32_t idx = ls->next; idx < total_entries; idx++) {
        uint32_t blk_idx = idx / DENTRIES_PER_BLOCK;   // which directory data block
        uint32_t ent_idx = idx % DENTRIES_PER_BLOCK;   // which dentry within that block
        uint32_t data_blk = 0;

        
        if (blk_idx < KTFS_NUM_DIRECT_DATA_BLOCKS) {
            // Direct data blocks
            data_blk = root->block[blk_idx];

        } else if (blk_idx < KTFS_NUM_DIRECT_DATA_BLOCKS + PTRS_PER_BLOCK) {
            // Indirect level
            if (root->indirect == 0) break;

            uint32_t* ind;
            r = cache_get_block(theCache,(root->indirect + data_block_offset) * KTFS_BLKSZ,(void**)&ind);
            if (r < 0) {
                release_inode_block(inodes, 0);
                return r;
            }

            uint32_t ind_idx = blk_idx - KTFS_NUM_DIRECT_DATA_BLOCKS;
            data_blk = ind[ind_idx]+ data_block_offset;

            cache_release_block(theCache, ind, 0);

        } else {
            // Doubly indirect 
            uint32_t rel = blk_idx - (KTFS_NUM_DIRECT_DATA_BLOCKS + PTRS_PER_BLOCK);
            uint32_t blocks_per_dind = PTRS_PER_BLOCK * PTRS_PER_BLOCK;

            uint32_t dind_idx= rel / blocks_per_dind;      // which dindirect slot (0 or 1)
            uint32_t within= rel % blocks_per_dind;      // index within that dindirect region
            uint32_t lvl1_idx= within / PTRS_PER_BLOCK;    // index in first-level table
            uint32_t lvl2_idx  = within % PTRS_PER_BLOCK;    // index in second-level table

            if (dind_idx >= KTFS_NUM_DINDIRECT_BLOCKS) break;
            if (root->dindirect[dind_idx] == 0) break;

            // Firstlevel table: holds pointers to second-level tables
            uint32_t* lvl1;
            r = cache_get_block(theCache,(root->dindirect[dind_idx] + data_block_offset) * KTFS_BLKSZ,(void**)&lvl1);
            if (r < 0) {
                release_inode_block(inodes, 0);
                return r;
            }

            if (lvl1[lvl1_idx] != 0) {
                // seconf table: holds pointers to actual data blocks
                uint32_t* lvl2;
                r = cache_get_block(theCache,(lvl1[lvl1_idx] + data_block_offset) * KTFS_BLKSZ,(void**)&lvl2);
                if (r < 0) {
                    cache_release_block(theCache, lvl1, 0);
                    release_inode_block(inodes, 0);
                    return r;
                }

                data_blk = lvl2[lvl2_idx];

                cache_release_block(theCache, lvl2, 0);
            }

            cache_release_block(theCache, lvl1, 0);
        }

        // If no valid data block we’re done 
        if (data_blk == 0) break;

        
        struct dentryblock* dblk;
        r = cache_get_block(theCache,(data_blk + data_block_offset) * KTFS_BLKSZ,(void**)&dblk);
        if (r < 0) {
            release_inode_block(inodes, 0);
            return r;
        }

        struct ktfs_dir_entry* de = &dblk->entrys[ent_idx];

        // Advance ls next so we don’t get stuck even if this dentry is empty
        ls->next = idx + 1;

        if (de->inode != 0) {
            // Non-empty dentry  return this filename
            // size_t len = strnlen(de->name, KTFS_MAX_FILENAME_LEN);
            size_t len = 0;
            while (len < KTFS_MAX_FILENAME_LEN && de->name[len] != '\0') {
                len++;
            }   

            // Copy at most bufsz bytes of the name into buf
            size_t to_copy = (len < bufsz) ? len : bufsz;
            memcpy(buf, de->name, to_copy);

            cache_release_block(theCache, dblk, 0);
            release_inode_block(inodes, 0);

            // Return the number of bytes we actually copied
            return (long)to_copy;
        
        }

        // Empty dentry
        cache_release_block(theCache, dblk, 0);
    }

    // No more entries
    ls->next = total_entries;
    release_inode_block(inodes, 0);
    return 0;
}
