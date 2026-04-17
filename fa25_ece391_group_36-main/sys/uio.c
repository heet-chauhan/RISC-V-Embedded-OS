/*! @file uio.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌⁠‍‍‌‌‍‌​⁠⁠‌
    @brief Uniform I/O interface
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef UIO_DEBUG
#define DEBUG
#endif

#ifdef UIO_TRACE
#define TRACE
#endif

#include "uio.h"

#include <stddef.h>  // for NULL and offsetof

#include "error.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uioimpl.h"

// Used for null uio devices and are supposed to return everything for 0

static void nulluio_close(struct uio* uio);

static long nulluio_read(struct uio* uio, void* buf, unsigned long bufsz);

static long nulluio_write(struct uio* uio, const void* buf, unsigned long buflen);

// INTERNAL GLOBAL VARIABLES AND CONSTANTS
//

void uio_close(struct uio* uio) {
    debug("uio_close: refcnt=%d, has_close=%d", uio->refcnt, (uio->intf->close != NULL));

    // Decrement reference count if it's greater than 0
    if (uio->refcnt > 0) {
        uio->refcnt--;
        debug("uio_close: decremented refcnt to %d", uio->refcnt);
    }

    // Only call the actual close method when refcnt reaches 0
    if (uio->refcnt == 0 && uio->intf->close != NULL) {
        debug("uio_close: calling close method");
        uio->intf->close(uio);
    } else if (uio->refcnt > 0) {
        debug("uio_close: NOT calling close (refcnt=%d still has references)", uio->refcnt);
    }
}

// refcnt exists because different parts can refer to the same UIO. 

// UIO is Uniform I/O. Astracted interface for different I/O devices. 

long uio_read(struct uio* uio, void* buf, unsigned long bufsz) {
    if (uio->intf->read != NULL) {
        if (0 <= (long)bufsz)
            return uio->intf->read(uio, buf, bufsz);
        else
            return -EINVAL;
    } else
        return -ENOTSUP;
}

long uio_write(struct uio* uio, const void* buf, unsigned long buflen) {
    if (uio->intf->write != NULL) {
        if (0 <= (long)buflen)
            return uio->intf->write(uio, buf, buflen);
        else
            return -EINVAL;
    } else
        return -ENOTSUP;
}

int uio_cntl(struct uio* uio, int op, void* arg) {
    if (uio->intf->cntl != NULL)
        return uio->intf->cntl(uio, op, arg);
    else
        return -ENOTSUP;
}

unsigned long uio_refcnt(const struct uio* uio) {
    assert(uio != NULL);
    return uio->refcnt;
}

int uio_addref(struct uio* uio) { return ++uio->refcnt; }

struct uio* create_null_uio(void) {
    static const struct uio_intf nulluio_intf = {
        .close = &nulluio_close, .read = &nulluio_read, .write = &nulluio_write};

    static struct uio nulluio = {.intf = &nulluio_intf, .refcnt = 0};

    return &nulluio;
}

// Placeholder UIO which does nothing 

static void nulluio_close(struct uio* uio) {
    // ...
}

static long nulluio_read(struct uio* uio, void* buf, unsigned long bufsz) {
    // ...
    return -ENOTSUP;
}

static long nulluio_write(struct uio* uio, const void* buf, unsigned long buflen) {
    // ...
    return -ENOTSUP;
}

// Implementing the pipe

// Pipe is a one sided comm channel between two procs or threads. 

// Buffer is a chunk of memory which holds the data for the read and write. 

struct the_buffer {
    char * buffer; // The circular buffer of page size
    int h; // Head pointer
    int t; // Tail pointer
    int r_cnt; // How many readers
    int w_cnt; // How many writers
    struct condition r_cond; // Read cond var
    struct condition w_cond; // Write cond var. 
};


// Adding pipes to uio

struct reader_uio {
    struct uio b;
    struct the_buffer * r_buf; // Pointer to pipe buffer
};

// uio interface so the standard IO functions can be used

struct writer_uio {
    struct uio b;
    struct the_buffer * w_buf;
};

// Pipe Helper functions

// Avail data in pipe
static unsigned int data_avail(struct the_buffer * pipe_buf) {
    if ((*pipe_buf).t < (*pipe_buf).h) {
        return PAGE_SIZE - ((*pipe_buf).h - (*pipe_buf).t);
    } else {
        return ((*pipe_buf).t - (*pipe_buf).h);
    }
}

// Avail space in pipe

static unsigned int space_avail(struct the_buffer * pipe_buf) {
    unsigned int space = 0;
    space += PAGE_SIZE;
    space--;
    space -= data_avail(pipe_buf);
    return space;
}

// reads data from pipes buffer to user provided buffer

static long read(struct uio * uio, void * buffer, unsigned long buffer_size) {


    struct reader_uio * the_reader = (struct reader_uio *) uio;

    char * the_out = (char *) buffer; // Enables byte to byte copying
    
    struct the_buffer * pipe_buffer = (*the_reader).r_buf;
    
    unsigned long num_bytes_read = 0;

    while (num_bytes_read < buffer_size) {
        int avail = data_avail(pipe_buffer);
        
        if (!avail) {
            if (!(*pipe_buffer).w_cnt) {
                return num_bytes_read;
            } else {
                //while ((*pipe_buffer).w_cnt) {
                condition_wait(&((*pipe_buffer).r_cond));
                //}
                continue;
            }
        } else {
            int left_to_read = avail;

            int diff = buffer_size - num_bytes_read;

            if (diff < left_to_read) {
                left_to_read = diff;
            }

            //struct the_buffer pipe_buffer_deref = (*pipe_buffer);

            int idx = 0;
            while (idx < left_to_read) {
                the_out[num_bytes_read] = ((*pipe_buffer)).buffer[((*pipe_buffer)).h];
                num_bytes_read++;
                (*pipe_buffer).h = ((*pipe_buffer).h + 1);
                (*pipe_buffer).h = (*pipe_buffer).h % PAGE_SIZE;
                idx++;
            }

            condition_broadcast(&((*pipe_buffer).w_cond)); // signal that space is now avail

            if (num_bytes_read > 0) {
                return num_bytes_read;
            }
        }
    }

    return num_bytes_read;
}

// When reader closes its pipe

static void close_the_reader(struct uio * uio) {
    struct the_buffer * pipe_buffer = (*((struct reader_uio *) uio)).r_buf;

    // Gets the shared pipe buffer from the reader structure

    if (pipe_buffer) {
        (*pipe_buffer).r_cnt--;

        condition_broadcast(&(*pipe_buffer).w_cond); // Wake up since writers are waiting on reader but no point on waking up forever

        if (!(*pipe_buffer).r_cnt && !(*pipe_buffer).w_cnt) { // If both are zero, free. Do not free if not
            free_phys_page((*pipe_buffer).buffer);
            kfree(pipe_buffer);
        }
    }

    kfree(uio);
}

static void close_the_writer(struct uio * uio) {
    struct the_buffer * pipe_buffer = (*((struct writer_uio *) uio)).w_buf;

    if (pipe_buffer) {
        (*pipe_buffer).w_cnt--;

        condition_broadcast(&(*pipe_buffer).r_cond);

        if (!(*pipe_buffer).r_cnt && !(*pipe_buffer).w_cnt) {
            free_phys_page((*pipe_buffer).buffer);
            kfree(pipe_buffer);
        }
    }

    kfree(uio);
}

static long write(struct uio * uio, const void * buffer, unsigned long length_of_buffer) {
    struct the_buffer * pipe_buffer = (*((struct writer_uio *) uio)).w_buf;
    
    long num_bytes_written = 0;

    const char * input_buffer = (const char *) buffer;

    if(!(*pipe_buffer).r_cnt) {
        return -EPIPE; // Got this from error.h
    } else {
        while (num_bytes_written < length_of_buffer) {
            int avail_spc = space_avail(pipe_buffer);

            if (!avail_spc) {
                if(!(*pipe_buffer).r_cnt) {
                    return -EPIPE; // Got this from error.h
                } else {
                    condition_wait(&(*pipe_buffer).w_cond);
                    continue;;
                }
            }

            int left_to_write = avail_spc;

            int diff = length_of_buffer - num_bytes_written;

            if (diff < left_to_write) {
                left_to_write = diff;
            }

            int idx = 0;

            while (idx < left_to_write) {
                (*pipe_buffer).buffer[(*pipe_buffer).t] = input_buffer[num_bytes_written];
                (*pipe_buffer).t++;
                (*pipe_buffer).t = (*pipe_buffer).t % PAGE_SIZE;
                idx++;
                num_bytes_written++;
            }

            condition_broadcast(&(*pipe_buffer).r_cond);
        }
    }

    return num_bytes_written;
}

// Define the interfaces

static const struct uio_intf writer_intf = {
    .close = close_the_writer,
    .read = NULL,
    .write = write,
    .cntl = NULL
};

static const struct uio_intf reader_intf = {
    .close = close_the_reader,
    .read = read,
    .write = NULL,
    .cntl = NULL
};

// Create the pipe

void create_pipe(struct uio ** wptr, struct uio ** rptr) {
    struct the_buffer * pipe_buffer = kmalloc(sizeof(struct the_buffer));

    if (!pipe_buffer) {
        *wptr = NULL;
        *rptr = *wptr;
        return;
    } else {
        (*pipe_buffer).buffer = alloc_phys_page();

        if (!(*pipe_buffer).buffer) {
            kfree(pipe_buffer);
            *wptr = NULL;
            *rptr = *wptr;
            return;
        } else {
            // Init pipe buf
            (*pipe_buffer).h = 0;
            (*pipe_buffer).t = (*pipe_buffer).h;
            (*pipe_buffer).w_cnt = 1;
            (*pipe_buffer).r_cnt = (*pipe_buffer).w_cnt;
            
            condition_init(&((*pipe_buffer)).r_cond, "Pipe Read");
            condition_init(&((*pipe_buffer)).w_cond, "Pipe Write");

            // Create reader uio
            struct reader_uio * reader = kmalloc(sizeof(struct reader_uio));
            
            if (!reader) {
                free_phys_page((*pipe_buffer).buffer);
                *wptr = NULL;
                *rptr = *wptr;
                kfree(pipe_buffer);
                return;
            } else {
                uio_init1(&(*reader).b, &reader_intf);
                (*reader).r_buf = pipe_buffer;

                struct writer_uio * writer = kmalloc(sizeof(struct writer_uio));

                if (!writer) {
                    free_phys_page((*pipe_buffer).buffer);
                    *wptr = NULL;
                    *rptr = *wptr;
                    kfree(pipe_buffer);
                    kfree(reader);
                    return;
                } else {
                    uio_init1(&(*writer).b, &writer_intf);

                    (*writer).w_buf = pipe_buffer;

                    // Return the UIO pointers
                    *rptr = &(*reader).b;
                    *wptr = &(*writer).b;
                }                
            }
        }
    }
}