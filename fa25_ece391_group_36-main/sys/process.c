/*! @file process.c
    @brief user process
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

/*!
 * @brief Enables trace messages for process.c
 */
 #ifdef PROCESS_TRACE
 #define TRACE
 #endif
 
 /*!
  * @brief Enables debug messages for process.c
  */
 #ifdef PROCESS_DEBUG
 #define DEBUG
 #endif
 
#include "process.h"

#include "conf.h"
#include "console.h"
#include "elf.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "riscv.h"
#include "string.h"
#include "thread.h"
#include "trap.h"
#include "uio.h"
 
 // COMPILE-TIME PARAMETERS
 //
 
 /*!
  * @brief Maximum number of processes
  */
 #ifndef NPROC
 #define NPROC 16
 #endif
 
 // INTERNAL FUNCTION DECLARATIONS
 //
 
 static int build_stack(void* stack, int argc, char** argv);
 
 static void fork_func(struct condition* forked, struct trap_frame* tfr);
 
 // INTERNAL GLOBAL VARIABLES
 //
 
 /*!
  * @brief The main user process struct
  */
 static struct process main_proc;
 
 static struct process* proctab[NPROC] = {&main_proc};
 
 // EXPORTED GLOBAL VARIABLES
 //
 
 char procmgr_initialized = 0;
 
 // EXPORTED FUNCTION DEFINITIONS
 //
 
 void procmgr_init(void) {
     assert(memory_initialized && heap_initialized);
     assert(!procmgr_initialized);
 
     main_proc.tid = running_thread();
     main_proc.mtag = active_mspace();
     thread_set_process(main_proc.tid, &main_proc);
     procmgr_initialized = 1;
 }

 static void free_argv_copy(char ** argv_copy, int argc) {
    if (argv_copy) {
        for (int i = 0; i <= argc; i++) {
            if (i == argc) {
                kfree(argv_copy);
            } else {
                kfree(argv_copy[i]);
            }
        }
    }    
 }
 
 int process_exec(struct uio* exefile, int argc, char** argv) {
    //kprintf("DEBUG: process_exec called with exefile=%p, argc=%d, argv=%p\n", exefile, argc, argv);

    // FIXME

    // When your coming from U mode you need to assume everything is malicious and need to verify the security

    // Only require a max of 16 concurrent processes
    // a. First any virtual memory mappings belonging to any other proceses should be unmapped
    // b. The executable should be loaded in from the UIO interface provided as an argument into the mapped pages. After an executable is loaded into the memory space the file should be closed.
    // c. The thread the thread associated with the process needs to be started in user-mode. (Use an thread startup from thrasm.s here)

    // Need to context switch from S mode to User mode
    // Need to use CSRs

    // Psuedocode

    // Copy program arguments argc and argv to kernel for this process temporarily

    // Free all user pages that were allocated for the old program but keep kernel / IO mappings.
        // Call reset_active_mspace
        // All user space memory of old program is gone.

    if (!exefile) {
        return -EINVAL;
    } else if (argc < 0) {
        return -EINVAL;
    // } else if (!argv) {
    //     return -EINVAL;
    }

    // Copy argv strings into kernel memory

    char ** argv_copy = NULL;

    if (argv && argc >= 1) {
        argv_copy = kmalloc((1+argc) * sizeof(char *)); // 1 + argc for null terminator

        if (!argv_copy) {
            return -ENOMEM;
        } else {
            int argc_idx;
            argc_idx = 0;

            while(argc_idx < argc) {
                if (argv[argc_idx]) {
                    size_t length = strlen(argv[argc_idx]);
                    length++; // For null term

                    argv_copy[argc_idx] = kmalloc(length);

                    if (argv_copy[argc_idx]) {
                        memcpy(argv_copy[argc_idx], argv[argc_idx], length);
                        argc_idx++;
                    } else {
                        // Alloc error

                        free_argv_copy(argv_copy, argc_idx);

                        return -ENOMEM; // Alloc failed
                    }
                } else {
                    argv_copy[argc_idx] = argv[argc_idx];
                    argc_idx++;
                }
            }

            argv_copy[argc] = NULL; // Null term
        }
    }
    
    reset_active_mspace();

    struct process * proc = running_thread_process();

    if(!proc) {

        if(argv_copy) {
            free_argv_copy(argv_copy, argc);
            return -EINVAL;
        } else {
            return -EINVAL;
        }
    }

    (*proc).tid = running_thread();
    (*proc).mtag = active_mspace();


    // Declare a void pointer to point to the entry point of the new program

    void (*entry_pt)(void) = NULL;

    // Call elf load to load new exec image into memory
    // Need to allocate physical pages and update page table for new program

    int ret_val = elf_load(exefile, &entry_pt);

    if (ret_val < 0) {
        // Error occured need to return immediately
        free_argv_copy(argv_copy, argc);
        return ret_val;
    } else {
        
        // After executable is loaded into the memory space, the file should be closed. 

        uio_close(exefile);
        
        int r_w_u_permission_flag = PTE_R;
        r_w_u_permission_flag |= PTE_W;
        r_w_u_permission_flag |= PTE_U; // Set the permision flags using bitwise OR for reading, writing and user mode

        void * allocated_page_for_stack = alloc_and_map_range(UMEM_END_VMA - PAGE_SIZE, PAGE_SIZE, r_w_u_permission_flag);

        if (!allocated_page_for_stack) { // NULL Check to see if it was allocated
            free_argv_copy(argv_copy, argc);
            return -ENOMEM;
        } else {
            //kprintf("Allocated Page for Stack");

            // Building stack in kernel memory of PAGE_SIZE because user stack is one full page
            //char stack_buf[PAGE_SIZE];

            // int stack_size_debug = build_stack(allocated_page_for_stack, argc, argv);
            int stack_size_debug = build_stack(allocated_page_for_stack, argc, argv_copy);

            // if (stack_size_debug % 16 != 0) {
            //     stack_size_debug += (stack_size_debug % 16);
            // }

            if (stack_size_debug < 0) {
                free_argv_copy(argv_copy, argc);
                return stack_size_debug; // Return the same error code if build_stack threw an error
            } else {
                //kprintf("Build stack worked");

                // Copy the stack buffer into the user page
                //memcpy(allocated_page_for_stack, allocated_page_for_stack, stack_size_debug);

                struct trap_frame trap_fr = {0}; // Initialize the trap frame tp 0

                trap_fr.sepc = (void *) entry_pt; // Set the entry point to go to after going to the trap handler

                trap_fr.sp = (void *)(UMEM_END_VMA - stack_size_debug); // Set the stack pointer to point right below the arguments

                //trap_fr.sstatus &= ~(RISCV_SSTATUS_SPP | RISCV_SSTATUS_SPIE); // SPP cleared to sret is returned to U mode

                trap_fr.a0 = argc;
                trap_fr.a1 = (long)(UMEM_END_VMA - stack_size_debug);

                trap_fr.sstatus = csrr_sstatus();

                trap_fr.sstatus &= ~RISCV_SSTATUS_SPP;

                trap_fr.sstatus |= RISCV_SSTATUS_SPIE; // Enable Interrupts

                //void * stack_anchor = running_thread_stack_base(); // Get the threads stack anchor

                char * addr = (char *) running_thread_stack_base(); // Get the threads stack anchor once again and cast it to char * for pointer arithmetic
    

                addr -= sizeof(struct trap_frame); // Subtract by the size of the trap frame to ensure that it points to the address after the trap frame in kernel stack

                //void * sscratch = NULL;

                //write_csr(sscratch, addr);

                // void * sscratch = addr; // Set sscratch CSR to the calculated addr


                trap_frame_jump(&(trap_fr), addr); // Perform the Jump from S mode to U mode

                //kprintf("Should have jumped already. Should not be printing this");
            }
        }
        
    }

    //kprintf("THIS IS BAD. GO TO OH NOW");

    return -ENOTSUP;

    //return 0;
 }

 static struct uio * duplicate(struct uio * uio) {
    // if (!uio) {
    //     uio_addref(uio);
    //     return uio;
    // } else {
    //     return uio;
    // }

    if (!uio) {
        return NULL;
    }

    uio_addref(uio);
    return uio;
 }
 
 int process_fork(const struct trap_frame* tfr) {
     // FIXME

     // Parent process

     struct process * papa = running_thread_process();

     if (!papa) {
        return -EINVAL;
     } else {
        struct process * chld;

        int idx = -1;

        int curr_idx = 0;

        while (curr_idx < NPROC) {
            if(proctab[curr_idx]) {
                curr_idx++;
                continue;
            } else {
                chld = kmalloc(sizeof(struct process));

                if(!chld) {
                    return -ENOMEM;
                } else {
                    idx = curr_idx;
                    proctab[curr_idx] = chld;
                    break;
                }
            }
        }

        if(!chld) {
            return -EINVAL;
        } else {
            memset(chld, 0, sizeof(struct process));

            for (int i = 0; i < PROCESS_UIOMAX; i++) {
                if((*papa).uiotab[i]) {
                    (*chld).uiotab[i] = duplicate((*papa).uiotab[i]);
                } else {
                    continue;
                }
            }

            mtag_t chld_t = clone_active_mspace();

            if(!chld_t) {
                for (int i = 0; i < PROCESS_UIOMAX; i++) {
                    if((*chld).uiotab[i]) {
                        uio_close((*chld).uiotab[i]);
                    }
                }
                proctab[idx] = NULL;
                kfree(chld);
                return -ENOMEM;
            } else {
                (*chld).mtag = chld_t;

                struct trap_frame * tfr_cpy = kmalloc(sizeof(struct trap_frame));

                if(!tfr_cpy) {
                    for (int i = 0; i < PROCESS_UIOMAX; i++) {
                        if((*chld).uiotab[i]) {
                            uio_close((*chld).uiotab[i]);
                        }
                    }

                    proctab[idx] = NULL;
                    kfree(chld);
                    return -ENOMEM;
                } else {
                    memcpy(tfr_cpy, tfr, sizeof(struct trap_frame));

                    struct condition fked;

                    condition_init(&fked, "I am done fking");

                    void (* entry_ptr) (void) = (void *) fork_func;

                    int chld_tid = spawn_thread("chld_proc", entry_ptr, &fked, tfr_cpy);

                    if (chld_tid * 1 < 0) {
                        for (int i = 0; i < PROCESS_UIOMAX; i++) {
                            if((*chld).uiotab[i]) {
                                uio_close((*chld).uiotab[i]);
                            }
                        }
                        proctab[idx] = NULL;
                        kfree(chld);
                        kfree(tfr_cpy);

                        return chld_tid;
                    } else {
                        (*chld).tid = chld_tid;

                        thread_set_process(chld_tid, chld);

                        mtag_t ole_tag = switch_mspace(chld_t);

                        switch_mspace(ole_tag);

                        condition_wait(&fked);

                        kfree(tfr_cpy);

                        return chld_tid;
                    }
                }
            }
        }
     }

     return 0;
 }
 
/** \brief
*
*
*  Discard memory space, close your associated uio, free the memory you're supposed to free.
*
*
*/
void process_exit(void) {
    // FIXME

    // Need to clean up resources used by processes
    // Includes process memory space
    // Open UIO interfaces
    // Associated Kernel Thread
    
    // Pseudocode
    // Get current thread
    // Close all the open UIO interface
    // Clean up process memory space
    // Exit the thread

    // Get the current process of the current thread because in this MP one process can only have one thread
    struct process * current_process_to_clean = running_thread_process();

    // Null check for this in case this is kernel thread which does not have a parent
    if (!current_process_to_clean) {
        running_thread_exit();
    }

    // Close all the UIO Interfaces

    for (int i = 0; i < PROCESS_UIOMAX; i++) {
        if(!(*(current_process_to_clean)).uiotab[i]) {
            continue;
        } else {
            uio_close((*(current_process_to_clean)).uiotab[i]);
            (*(current_process_to_clean)).uiotab[i] = NULL;
        }
    }

    for (int i = 0; i < NPROC; i++) {
        if (proctab[i] == current_process_to_clean) {

            // if (proctab[i] == &(main_proc)) {
            //     panic("Bad");
            // }

            proctab[i] = NULL;
        }
    }
    

    discard_active_mspace(); // Clean up the process memory space

    running_thread_exit(); // Exit the thread
}
 
 // INTERNAL FUNCTION DEFINITIONS
 //
 
 /**
  * \brief Builds the initial user stack for a new process.
  *
  * Builds the stack for a new process, including the argument vector (\p argv)
  * and the strings it points to. Note that \p argv must contain \p argc + 1
  * elements (the last one is a NULL pointer).
  *
  * Remember to round the final stack size up to a multiple of 16 bytes
  * (RISC-V ABI requirement).
  *
  * \param[in,out] stack  Pointer to the stack page (destination buffer).
  * \param[in]     argc   Number of arguments in \p argv.
  * \param[in]     argv   Array of argument pointers; length is \p argc+1 and
  *                       \p argv[argc] must be NULL.
  *
  * \return Size of the stack page on success; negative error code on failure.
  */
 int build_stack(void* stack, int argc, char** argv) {
     size_t stksz, argsz;
     uintptr_t* newargv;
     char* p;
     int i;
 
     // We need to be able to fit argv[] on the initial stack page, so _argc_
     // cannot be too large. Note that argv[] contains argc+1 elements (last one
     // is a NULL pointer).
 
     if (PAGE_SIZE / sizeof(char*) - 1 < argc) return -ENOMEM;
 
     stksz = (argc + 1) * sizeof(char*);
 
     // Add the sizes of the null-terminated strings that argv[] points to.
 
     for (i = 0; i < argc; i++) {
         argsz = strlen(argv[i]) + 1;
         if (PAGE_SIZE - stksz < argsz) return -ENOMEM;
         stksz += argsz;
     }
 
     // Round up stksz to a multiple of 16 (RISC-V ABI requirement).
 
     stksz = ROUND_UP(stksz, 16);
     assert(stksz <= PAGE_SIZE);
 
     // Set _newargv_ to point to the location of the argument vector on the new
     // stack and set _p_ to point to the stack space after it to which we will
     // copy the strings. Note that the string pointers we write to the new
     // argument vector must point to where the user process will see the stack.
     // The user stack will be at the highest page in user memory, the address of
     // which is `(UMEM_END_VMA - PAGE_SIZE)`. The offset of the _p_ within the
     // stack is given by `p - newargv'.
 
     newargv = stack + PAGE_SIZE - stksz;
     p = (char*)(newargv + argc + 1);
 
     for (i = 0; i < argc; i++) {
         newargv[i] = (UMEM_END_VMA - PAGE_SIZE) + ((void*)p - (void*)stack);
         argsz = strlen(argv[i]) + 1;
         memcpy(p, argv[i], argsz);
         p += argsz;
     }
 
     newargv[argc] = 0;
     return stksz;
 }
 
 /**
  * \brief Function to be executed by the child process after fork.
  * This is a very beautiful function.
  * Tell the parent process that it is done with the trap frame, then jumps to user space (hint:
  * which function should we use?)
  *
  * \param[in] done  Pointer to a condition variable to signal parent
  * \param[in] tfr   Pointer to a trap frame
  *
  * \return NONE (very important, this is a hint)
  */
 void fork_func(struct condition* done, struct trap_frame* tfr) {
     // FIXME

     // Signal parent that we are done

     condition_broadcast(done);

     // Return value is 0 for child process

     (*tfr).a0 = 0;

     (*tfr).sepc += 4;

     // Go to user space

     char * stack_base = (char *) running_thread_stack_base();

     stack_base -= sizeof(struct trap_frame);

     // Make the jump
     trap_frame_jump(tfr, stack_base);
 }