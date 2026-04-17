/*! @file syscall.c
    @brief system call handlers
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA
*/

#ifdef SYSCALL_TRACE
#define TRACE
#endif

#ifdef SYSCALL_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "console.h"
#include "device.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "intr.h"
#include "memory.h"
#include "misc.h"
#include "process.h"
#include "scnum.h"
#include "string.h"
#include "thread.h"
#include "timer.h"
#include "uio.h"

// EXPORTED FUNCTION DECLARATIONS
//

extern void handle_syscall(struct trap_frame *tfr);  // called from excp.c

// INTERNAL FUNCTION DECLARATIONS
//

static int64_t syscall(const struct trap_frame *tfr);

static int sysexit(void);
static int sysexec(int fd, int argc, char **argv);
static int sysfork(const struct trap_frame *tfr);
static int syswait(int tid);
static int sysprint(const char *msg);
static int sysusleep(unsigned long us);

static int sysfsdelete(const char *path);
static int sysfscreate(const char *path);

static int sysopen(int fd, const char *path);
static int sysclose(int fd);
static long sysread(int fd, void *buf, size_t bufsz);
static long syswrite(int fd, const void *buf, size_t len);
static int sysfcntl(int fd, int cmd, void *arg);
static int syspipe(int *wfdptr, int *rfdptr);
static int sysuiodup(int oldfd, int newfd);

// EXPORTED FUNCTION DEFINITIONS
//

/**
 * @brief Initiates syscall present in trap frame struct and stores the return address into the sepc
 * @details sepc will be used to return back to program execution after interrupt is handled and
 * sret is called
 * @param tfr pointer to trap frame struct
 * @return void
 */

void handle_syscall(struct trap_frame *tfr) {

    int returnval = syscall(tfr); // initiate the sycall and get the value that we want to return
    tfr->a0 = returnval;    // store the return value 
    tfr->sepc += 4; // go to the next instruction from when the trap was called 
    
}

// INTERNAL FUNCTION DEFINITIONS
//

/**
 * @brief Calls specified syscall and passes arguments
 * @details Function uses register a7 to determine syscall number and arguments are passed in from
 * a0-a5 depending on the function
 * @param tfr pointer to trap frame struct
 * @return result of syscall
 */

int64_t syscall(const struct trap_frame *tfr) { 
//just gfet the call number from a7 and check which one it is
    int64_t call_num = tfr->a7;

    if(call_num == SYSCALL_CLOSE){// just call the other functions in the file depending on what call it is with the proper arguments from a0-a5
        return sysclose(tfr->a0);
    }else if(call_num == SYSCALL_EXEC){
        return sysexec(tfr->a0, tfr->a1, (char **)tfr->a2);
    }else if(call_num == SYSCALL_EXIT){
        return sysexit();
    }else if(call_num == SYSCALL_FCNTL){
        return sysfcntl(tfr->a0, tfr->a1, (void *)tfr->a2);
    }else if(call_num == SYSCALL_FORK){
        return sysfork(tfr);
    }else if(call_num == SYSCALL_FSCREATE){
        return sysfscreate((const char *)tfr->a0);
    }else if(call_num == SYSCALL_FSDELETE){
        return sysfsdelete((const char *)tfr->a0);
    }else if(call_num == SYSCALL_OPEN){
        return sysopen(tfr->a0, (const char *)tfr->a1);
    }else if(call_num == SYSCALL_PIPE){
        return syspipe((int *)tfr->a0,(int *) tfr->a1);
    }else if(call_num == SYSCALL_PRINT){
        return sysprint((const char *)tfr->a0);
    }else if(call_num == SYSCALL_READ){
        return sysread(tfr->a0, (void *)tfr->a1, tfr->a2);
    }else if(call_num == SYSCALL_UIODUP){
        return sysuiodup(tfr->a0, tfr->a1);
    }else if(call_num == SYSCALL_USLEEP){
        return sysusleep(tfr->a0);
    }else if(call_num == SYSCALL_WAIT){
        return syswait(tfr->a0);
    }else if(call_num == SYSCALL_WRITE){
        return syswrite(tfr->a0, (const void *)tfr->a1, tfr->a2);
    }else{
        return -ENOTSUP;
    }

}

/**
 * @brief Calls process exit
 * @return void
 */

int sysexit(void) { 
    
    // literally just calls the process function and then process.c handles the rest 
    process_exit();
    return 0; // dont really need this cause theoritically idt we ever reach it.
}

/**
 * @brief Executes new process given a executable and arguments
 * @details Valid fd checks, get current process struct, close fd being executed, finally calls
 * process_exec with arguments and executable io "file"
 * @param fd file descripter idx
 * @param argc number of arguments in argv
 * @param argv array of arguments for multiple args
 * @return result of process_exec, else -EBADFD on invalid file descriptors
 */

int sysexec(int fd, int argc, char **argv) { 

    if (argc > 0 && argv) {
        int rc = validate_vptr(argv, (argc + 1) * sizeof(char *), PTE_R);

        if (rc < 0) {
            return rc;
        } else {
            if (argv[0]) {
                rc = validate_vstr(argv[0], PTE_R);
                if (rc < 0) {
                    return rc;
                }
            }
        }
    }

    struct process *p = current_process(); // get which process we are on and the file pointer from the uiotable
    struct uio *exec = p->uiotab[fd];

    if(!p){ // null checks 
        return -EINVAL;
    }
    if(fd<0){
        return -EBADFD;
    }
    if(!exec){
        return -EBADFD;
    }
    //sysclose(fd); // close the current process's fd so that when we make a new process nothing carries over and we have a blank slate
    p->uiotab[fd] = NULL;
    int returnValue = process_exec(exec, argc, argv);// just surely call the process exec and let process.c do the work hehehehhehe

    return returnValue; 
}

/**
 * @brief Forks a new child process using process_fork
 * @param tfr pointer to the trap frame
 * @return result of process_fork
 */

int sysfork(const struct trap_frame *tfr) { 
    //surely just call the process_fork function and let process.c do the rest
    int child = process_fork(tfr);
    return child; 
}

/**
 * @brief Sleeps till a specified child process completes
 * @details Calls thread_join with the thread id the process wishes to wait for
 * @param tid thread_id
 * @return result of thread_join else invalid on invalid thread id
 */

int syswait(int tid) { 
    // condition wait on the condition that the tid's oprocess is complete and then just call thread join.
    //** we have tomake sure the thread join is fully correct in thread.c */
    
    //do not implement yet according to piazza******************
    int rc = thread_join(tid);
    return rc;

}

/**
 * @brief Prints to console via kprintf
 * @details Validates that msg string is valid via validate_vstr and pages are mapped, calls kprintf
 * on current running process
 * @param msg string msg in userspace
 * @return 0 on sucess else error from validate_vstr
 */

int sysprint(const char *msg) { 

    int validate = validate_vstr(msg, PTE_R);
    if(validate < 0){
        return validate; //for now just return the invalid pointer
    }
    kprintf("%s", msg); // just print out to the console
    return 0;

}

/**
 * @brief Sleeps process till specificed amount of time has passed
 * @details Creates alarm struct, inits struct with name usleep, which sets the current time via the
 * rd_time() function, taking values from the csr, makes frequency calcuation to determine us has
 * passed before waking process
 * @param us time in us for process to sleep
 * @return 0
 */

int sysusleep(unsigned long us) {

    if(us ==0){
        return 0;
    }
    unsigned long long tcnt = (us * TIMER_FREQ) / 1000000ULL; //getting the number of ticks instead of the microsecs that were passed cause that is the measure used for the alarm
    struct alarm ring;
    alarm_init(&ring, "usleep");

    // if(tcnt ==0){
    //     tcnt =1;
    // } idk if this makes sense but we should prolly have a case where tcnt is 0 so we dont know what to do. This can maybe fix that, just adding a delay.  


    alarm_sleep(&ring, tcnt); // sleep for tcnt number of ticks we calculated earlier with TIMER_FREQ

    return 0;
    
}

/**
 * @brief Creates a new file in the filesystem specified by the path.
 * @details Validates and parses the user provided path for mountpoint name, file name and calls
 * create_file.
 * @param path User provided path string.
 * @return 0 on success, negative error code if error on error.
 */

int sysfscreate(const char *path) { 
    //validate
    int rc = validate_vstr(path, PTE_R);

    if(rc <0){
        return rc;
    }

    char *mountPoint = NULL;
    char *filename = NULL;
    rc = parse_path((char*)path, &mountPoint, &filename); //parse the path

    if(rc <0){
        return rc;
    }

    rc = create_file(mountPoint, filename); //create the file

    kfree
(mountPoint);
    kfree
(filename);


    return rc; 


}

/**
 * @brief Deletes a file in the filesystem specified by the path.
 * @details Validates and parses the user provided path for mountpoint name, file name and calls
 * delete_file.
 * @param path User provided path string.
 * @return 0 on success, negative error code if error on error.
 */

int sysfsdelete(const char *path) {  // literally the same as create but instead if create file just call delete file surely
    int rc = validate_vstr(path, PTE_R); // for reach only the flag is 1
    if(rc <0){
        return rc;
    }
    char *mountPoint = NULL;
    char *filename = NULL;
    rc = parse_path((char*)path, &mountPoint, &filename); //parse the path

    if(rc <0){
        return rc;
    }

    rc = delete_file(mountPoint, filename); //delete the file

    kfree(mountPoint);
    kfree(filename);

    return rc; 

    //return 0;
}

/**
 * @brief Opens a file or device of specified fd for given process
 * @details gets current process, allocates file descriptor (if fd = -1) or uses valid file
 * descriptor given, validates and parses user provided path, calls open_file
 * @param fd file descriptor number
 * @param path User provided path string
 * @return fd number if sucessful else return error that occured -EMFILE or -EBADFD
 */

int sysopen(int fd, const char *path) {
    int rc = validate_vstr(path, PTE_R);
    if(rc<0){
        return rc;
    }
    
    struct process *p = current_process();
        // struct process *p = current_process();

    if(!p){ 
        return -EINVAL;
    }

    char *mount_point = NULL;
    char *filename = NULL;
    rc = parse_path((char*)path, &mount_point, &filename);
     if (rc < 0) {
        return rc;
    }

    if (fd < 0) { // just check if the file descriptor is -1 and if so allocate to availbale table slot
        for (int i = 0; i < PROCESS_UIOMAX; i++) {
            if (p->uiotab[i] == NULL) {
                fd = i;
                break;
            }
        }
    }
    if (fd < 0 || fd >= PROCESS_UIOMAX){
        kfree(mount_point);
        kfree(filename);
        return -EMFILE;
    }
    if (p->uiotab[fd] != NULL){
        kfree(mount_point);
        kfree(filename);
        return -EBUSY;
    }
    struct uio* x = NULL;
    rc = open_file(mount_point,filename,&x );

    kfree(mount_point);
    kfree
(filename);
    if (rc < 0) {
        return rc;
    }

    p->uiotab[fd] = x;

    return fd;
}

/**
 * @brief Closes file or device of specified fd for given process
 * @details gets current process, calls close function of the io, deallocates the file descriptor
 * @param fd file descriptor
 * @return 0 on success, error on invalid file descriptor or empty file descriptor
 */

int sysclose(int fd) {

    struct process *p = current_process();
    if (!p)
        return -EINVAL;

    if (fd < 0 || fd >= PROCESS_UIOMAX)
        return -EBADFD;

    struct uio *x = p->uiotab[fd]; 

    if (x == NULL)
        return -EBADFD;

    uio_close(x);

    p->uiotab[fd] = NULL;

    return 0;
}

/**
 * @brief Calls read function of file io on given buffer
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * validate buffer, call ioread with given buffer
 * @param fd file descriptor number
 * @param buf pointer to buffer
 * @param bufsz number of bytes to be read
 * @return number of bytes read
 */

long sysread(int fd, void *buf, size_t bufsz) {
    struct process *p = current_process();
    if (!p)
        return -EINVAL;

    if (fd < 0 || fd >= PROCESS_UIOMAX)
        return -EBADFD;

    struct uio *x = p->uiotab[fd];
    if (x == NULL)
        return -EBADFD;

    int rc = validate_vptr(buf, bufsz, PTE_W);
    if (rc < 0)
        return rc;

    return uio_read(x, buf, bufsz);
    
}

/**
 * @brief Calls write function of file io on given buffer
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * validate buffer, call iowrite with given buffer
 * @param fd file descriptor number
 * @param buf pointer to buffer
 * @param len number of bytes to be written
 * @return number of bytes written
 */

long syswrite(int fd, const void *buf, size_t len) {
    struct process *p = current_process();
    if (!p)
        return -EINVAL;

    if (fd < 0 || fd >= PROCESS_UIOMAX)
        return -EBADFD;

    struct uio *x = p->uiotab[fd];
    if (x == NULL)
        return -EBADFD;

    int rc = validate_vptr(buf, len, PTE_R);
    if (rc < 0)
        return rc;

    return uio_write(x, buf, len);
    
}

/**
 * @brief Calls device input output commands for a given device instance
 * @details get current process, valid file descriptor checks, find io struct via file descriptor,
 * ensure that fcntl type exists, validate argument pointer, issue fcntl
 * @param fd file descriptor number
 * @param cmd selection of fcntl
 * @param arg pointer to arguments
 * @return number of bytes written
 */

int sysfcntl(int fd, int cmd, void *arg) {
    struct process *p = current_process();
    if (!p)
        return -EINVAL;

    if (fd < 0 || fd >= PROCESS_UIOMAX)
        return -EBADFD;

    struct uio *x = p->uiotab[fd];

    if (x == NULL)
        return -EBADFD;
    if (arg != NULL) {
        int rc;

        switch (cmd) {
        case FCNTL_GETPOS:
        case FCNTL_GETEND:
            rc = validate_vptr(arg, sizeof(unsigned long long), PTE_W);
            if (rc < 0){
                return rc;
            }
            break;

        case FCNTL_SETPOS:
        case FCNTL_SETEND:
            rc = validate_vptr(arg, sizeof(unsigned long long), PTE_R);
            if (rc < 0){
                return rc;
            }
            break;

        case FCNTL_MMAP:
            rc = validate_vptr(arg, sizeof(void *), PTE_W);
            if (rc < 0){
                return rc;
            }
            break;

        default:
            rc = validate_vptr(arg, sizeof(uintptr_t), PTE_R);
            if (rc < 0){
                return rc;
            }
            break;
        }
    }

    return uio_cntl(x, cmd, arg);
}



/**
 * @brief Creates a pipe for the current process
 * @details The function retrieves the current process. If either the write or read descriptor
 * pointer stores a negative value, an unused descriptor is assigned. If both file descriptors are
 * unused and valid, the function connects them via create_pipe function.
 * @param wfdptr pointer to write file descriptor
 * @param rfdptr pointer to read file descriptor
 * @return 0 on success. Else, negative error code on invalid file descriptor, or if a file
 * descriptor is already in use, or if no descriptors are found available.
 */
int syspipe(int *wfdptr, int *rfdptr) {
    struct process *proc = current_process();
    if(!proc){
        return -EINVAL;
    } else {
        int status;
        status = validate_vptr(wfdptr, sizeof(int), PTE_W);
        if (status < 0) {
            return status;
        } else {
            status = validate_vptr(rfdptr, sizeof(int), PTE_W);
            if (status < 0) {
                return status;
            } else {
                int wfd = (*wfdptr);
                int rfd = (*rfdptr);

                // Find available descriptor
                if ( wfd < 0) {
                    wfd = -1;
                    int idx = 0;
                    while (idx < PROCESS_UIOMAX) {
                        if(!(*proc).uiotab[idx]) {
                            wfd = idx;
                            break;
                        } else {
                            idx++;
                        }                       
                    }
                    if (wfd < 0) {
                        return -EMFILE; // Got from error.h again
                    }
                }

                if (wfd < 0) {
                    return -EBADFD; // This is just an extra precaution
                } else if (wfd >= PROCESS_UIOMAX) {
                    return -EBADFD; // Invalid file descriptor
                } else if ((*proc).uiotab[wfd]) {
                    return -EBUSY;
                } else {
                    // Find available descriptor for rfd

                    if (rfd < 0) {
                        rfd = -1;
                        int idx = 0;
                        while (idx < PROCESS_UIOMAX) {
                            if (!(*proc).uiotab[idx]) {
                                if (idx == wfd) {
                                    idx++;
                                    continue;
                                } else {
                                    rfd = idx;
                                    idx++;
                                    break;
                                }
                            } else {
                                idx++;
                            }
                        }

                        if (rfd < 0) {
                            return -EMFILE; // No descriptors are available
                        }
                    }

                    if (rfd < 0) {
                        return -EBADFD; // Once again an extra precaution
                    } else if (rfd >= PROCESS_UIOMAX) {
                        return -EBADFMT;
                    } else if ((*proc).uiotab[rfd]) {
                        return -EBUSY;
                    } else if (wfd - rfd == 0) {
                        return -EINVAL; // Invalid argument
                    } else {
                        // Create the actual pipe now

                        struct uio * r_uio;
                        struct uio * w_uio;

                        create_pipe(&w_uio, &r_uio);

                        if (!r_uio) {
                            return -ENOMEM;
                        } else if (!w_uio) {
                            return -ENOMEM;
                        } else {
                            (*proc).uiotab[rfd] = r_uio;
                            (*proc).uiotab[wfd] = w_uio;

                            // Descriptors to file space
                            *rfdptr = rfd;
                            *wfdptr = wfd;

                            return 0;
                        }
                    }
                }
            }
        }
    }



    return 0; 
}

/**
 * @brief Duplicates a file description
 * @details Allocates a new file descriptor that refers to the same open _uio_ as the descriptor
 * _oldfd_. Increments the _refcnt_ if successful.
 * @param oldfd old file descriptor number
 * @param newfd new file descriptor number
 * @return fd number if sucessful else return error on invalid file descriptor or empty file
 * descriptor
 */

int sysuiodup(int oldfd, int newfd) {
    // struct process *p = current_process();
    // struct uio* prev = p->uiotab[oldfd]; 
    // if(!p){
    //     return -EINVAL;
    // }
    // if(oldfd <0 || oldfd >= PROCESS_UIOMAX || prev == NULL){
    //     return -EBADFD;
    // }
    // if (newfd < 0) {
    //     for (int i = 0; i < PROCESS_UIOMAX; i++) {
    //         if (p->uiotab[i] == NULL) {
    //             newfd = i;
    //             break;
    //         }
    //     }
    // }
    // if(newfd <0 || newfd >= PROCESS_UIOMAX){
    //     return -EBADFD;
    // }
    // if (p->uiotab[newfd] != NULL){
    //     return -EBUSY;
    // }
    // int rc = uio_addref(prev);
    // p->uiotab[newfd] = prev;
    // return newfd;
    struct process *p = current_process();
    if (!p)
        return -EINVAL;

    if (oldfd < 0 || oldfd >= PROCESS_UIOMAX)
        return -EBADFD;

    struct uio *source = p->uiotab[oldfd];
    if (source == NULL)
        return -EBADFD;

    if (newfd == oldfd)
        return oldfd;

    if (newfd < 0) {
        for (int i = 0; i < PROCESS_UIOMAX; i++) {
            if (p->uiotab[i] == NULL) {
                newfd = i;
                break;
            }
        }
        if (newfd < 0)
            return -EMFILE;
    }

    if (newfd < 0 || newfd >= PROCESS_UIOMAX)
        return -EBADFD;
    if (p->uiotab[newfd] != NULL)
        return -EBUSY;

    int rc = uio_addref(source);
    if (rc < 0)
        return rc;

    p->uiotab[newfd] = source;
    return newfd;
    
}