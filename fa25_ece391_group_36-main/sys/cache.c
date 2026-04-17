/*! @file cache.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌⁠‍‍‌‌‍‌​⁠⁠‌
    @brief Block cache for a storage device.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef CACHE_TRACE
#define TRACE
#endif

#ifdef CACHE_DEBUG
#define DEBUG
#endif

#include "cache.h"

#include "conf.h"
#include "console.h"
#include "device.h"
#include "devimpl.h"
#include "error.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "thread.h"

// INTERNAL TYPE DEFINITIONS
//

//create a struct that will store each individual block

#define KTFS_BLKSZ 512

struct datablock {
    void * data;
    int block_id;
    int in_use;
    int dirty;
};

//this is the actual cache array
//make this a struct
//have a lock on the cache
//only one thread should use the cache at once 
struct cache{
    struct datablock cacheArray[CACHE_CAPACITY];
    struct storage * cacheBackingDevice;
    int currSize;
    struct lock cacheLock; // fix this to proper lock 
};

static struct cache theCache;


/**
 * @brief Creates/initializes a cache with the passed backing storage device (disk) and makes it
 * available through cptr.
 * @param disk Pointer to the backing storage device.
 * @param cptr Pointer to the cache to create.
 * @return 0 on success, negative error code if error
 */
int create_cache(struct storage* disk, struct cache** cptr) {
    // FIXME
    //create a global cache object
    *cptr = &theCache;
    theCache.cacheBackingDevice = disk;
    theCache.currSize = 0;

    //init lock
    lock_init(&theCache.cacheLock);
    
    for (int i = 0; i < CACHE_CAPACITY; i++) {
        theCache.cacheArray[i].data = NULL;
        theCache.cacheArray[i].block_id = -1;
        theCache.cacheArray[i].in_use = 0;
        theCache.cacheArray[i].dirty = 0;
    }
    
    return 0;
}

// static int evict(struct cache* cache){
//     int removeIdx = cache->currSize;
//     while (removeIdx >= 0){
//         //check if its in use, if so dont remove that one
//         if (cache->cacheArray[removeIdx].in_use == 1){
//             removeIdx--;
//         }
//         else{
//             //set as not in use
//             cache->cacheArray[removeIdx].in_use = 0;

//             //write to disk if dirty
//             if (cache->cacheArray[removeIdx].dirty == 1){
//                 storage_store(cache->cacheBackingDevice, cache->cacheArray[removeIdx].block_id * KTFS_BLKSZ, cache->cacheArray[removeIdx].data, KTFS_BLKSZ);

//                 //reset dirty
//                 cache->cacheArray[removeIdx].dirty = 0;
//             }
//             return removeIdx;
//         }
//     }
//     return 0;
// }

static int evict(struct cache* cache){
    for (int removeIdx = cache->currSize - 1; removeIdx > -1; removeIdx--) {
        //check if its in use and if so dont remove that one
        if (cache->cacheArray[removeIdx].in_use == 0){
            //write to disk if dirty
            if (cache->cacheArray[removeIdx].dirty == 1){
                storage_store(cache->cacheBackingDevice, cache->cacheArray[removeIdx].block_id * KTFS_BLKSZ, cache->cacheArray[removeIdx].data, KTFS_BLKSZ);
                //reset dirty
                cache->cacheArray[removeIdx].dirty = 0;
            }
            return removeIdx;
        }
    }
    
    // All blocks in use so return an error because this is not expected. 
    return -EBUSY;  // Indicate failure
}

/**
 * @brief Reads a CACHE_BLKSZ sized block from the backing interface into the cache.
 * @param cache Pointer to the cache.
 * @param pos Position in the backing storage device. Must be aligned to a multiple of the block
 * size of the backing interface.
 * @param pptr Pointer to the block pointer read from the cache. Assume that CACHE_BLKSZ will always
 * be equal to the block size of the storage disk. Any replacement policy is permitted, as long as
 * your design meets the above specifications.
 * @return 0 on success, negative error code if error
 */
int cache_get_block(struct cache* cache, unsigned long long pos, void** pptr) {
    // FIXME

    //acquire lock
    lock_acquire(&cache->cacheLock);

    //check for errors
    if (cache == NULL){
        lock_release(&cache->cacheLock);
        return -ENOTSUP;
    }
    
    //check if block exists in cache
    for (int i=0; i<cache->currSize; i++){
        if (pos/KTFS_BLKSZ == cache->cacheArray[i].block_id){
            //if so set pointer
            //return the data from the cache
            *pptr = cache->cacheArray[i].data;

            cache->cacheArray[i].in_use = 1;
            //lock_release(&cache->cacheLock);
            return 0;
        }
    }

    //evict if full
    //make sure to check if the block is in use before evicting 
    int evicted = 0;
    int evictedIndex = 0;
    if (cache->currSize == CACHE_CAPACITY){
        evictedIndex = evict(cache);
        if (evictedIndex < 0) {
            lock_release(&(cache -> cacheLock));
            return evictedIndex;
        }
        evicted = 1;
    }

    //check if we write to a new block or an evicted one and set index
    int indexToWrite = 0;
    if (evicted==1){
        indexToWrite = evictedIndex;
    }
    else{
        indexToWrite = cache->currSize;
    }

    //allocate space for data if needed
    if (cache->cacheArray[indexToWrite].data == NULL){
        cache->cacheArray[indexToWrite].data = kcalloc(KTFS_BLKSZ, 1);
        if (cache->cacheArray[indexToWrite].data == NULL) {
            lock_release(&cache->cacheLock);
            return -ENOMEM;
        }
    }
   
    //get the data from the disk and store in cache
    int bytesReturned = storage_fetch(cache->cacheBackingDevice, pos, cache->cacheArray[indexToWrite].data, KTFS_BLKSZ);
    if (bytesReturned < 0){
        lock_release(&cache->cacheLock);
        return -ENOTSUP;
    }

    //increment currSize to make room for next element if next is not max
    if (evicted == 0 && cache->currSize < CACHE_CAPACITY){
        cache->currSize++;
    }

    //set pointer
    *pptr = cache->cacheArray[indexToWrite].data;

    cache->cacheArray[indexToWrite].block_id = pos/KTFS_BLKSZ;

    //mark as in use
    cache->cacheArray[indexToWrite].in_use = 1;

    // mark as clean

    cache->cacheArray[indexToWrite].dirty = 0;

    //lock_release(&cache->cacheLock);
    

    return 0;

}

/**
 * @brief Releases a block previously obtained from cache_get_block().
 * @param cache Pointer to the cache.
 * @param pblk Pointer to a block that was made available in cache_get_block() (which means that
 * pblk == *pptr for some pptr).
 * @param dirty Indicates whether the block has been modified (1) or not (0). If dirty == 1, the
 * block has been written to. If dirty == 0, the block has not been written to.
 * @return 0 on success, negative error code if error
 */
void cache_release_block(struct cache* cache, void* pblk, int dirty) {
    // FIXME
   //lock_acquire(&cache->cacheLock);

    //find which node the data corresponds to
    int blockFound = 0;
    int i;
    for (i=0; i<CACHE_CAPACITY; i++){
        if (cache->cacheArray[i].data == pblk){
            blockFound = 1;
            break;
        }
    }
   
    //return if not found a block 
    if (blockFound == 0){
        //lock_release(&cache->cacheLock);
        return;
    }

    //else mark as block as not in use anymore
    cache->cacheArray[i].in_use = 0;

    //mark it dirty if needed
    if (dirty) {
        cache->cacheArray[i].dirty = 1;
    }


    lock_release(&cache->cacheLock);
    return;
}

/**
 * @brief Flushes the cache to the backing device
 * @param cache Pointer to the cache to flush
 * @return 0 on success, error code if error
 */
int cache_flush(struct cache* cache) {
    // FIXME
    //acquire a lock
    lock_acquire(&cache->cacheLock);

    //iterate through every block
    for (int i=0; i<cache->currSize; i++){
        

        //check if dirty
        if (cache->cacheArray[i].dirty == 1){
            //write to disk
            int returnedVal = storage_store(cache->cacheBackingDevice, cache->cacheArray[i].block_id * KTFS_BLKSZ, cache->cacheArray[i].data, KTFS_BLKSZ);
            if (returnedVal < 0){
                lock_release(&cache->cacheLock);
                return -ENOTSUP;
            }
            cache->cacheArray[i].dirty = 0;
        }

        //mark as not in use (do i need this)
        // cache->cacheArray[i].in_use = 0;
    }

    //release the lock
    lock_release(&cache->cacheLock);
    return 0;
}