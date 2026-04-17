/*! @file elf.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌⁠‍‍‌‌‍‌​⁠⁠‌
   @brief ELF file loader
   @copyright Copyright (c) 2024-2025 University of Illinois
   @license SPDX-License-identifier: NCSA


*/


#ifdef ELF_TRACE
#define TRACE
#endif


#ifdef ELF_DEBUG
#define DEBUG
#endif


#include "elf.h"


#include <stdint.h>


#include "conf.h"
#include "error.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "uio.h"


#include "heap.h"

// Offsets into e_ident


#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define EI_OSABI 7
#define EI_ABIVERSION 8
#define EI_PAD 9


// ELF header e_ident[EI_CLASS] values


#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2


// ELF header e_ident[EI_DATA] values


#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2


// ELF header e_ident[EI_VERSION] values


#define EV_NONE 0
#define EV_CURRENT 1


// ELF header e_type values


enum elf_et { ET_NONE = 0, ET_REL, ET_EXEC, ET_DYN, ET_CORE };


/*! @struct elf64_ehdr
   @brief ELF header struct
*/
struct elf64_ehdr {
   unsigned char e_ident[16];
   uint16_t e_type;
   uint16_t e_machine;
   uint32_t e_version;
   uint64_t e_entry;
   uint64_t e_phoff;
   uint64_t e_shoff;
   uint32_t e_flags;
   uint16_t e_ehsize;
   uint16_t e_phentsize;
   uint16_t e_phnum;
   uint16_t e_shentsize;
   uint16_t e_shnum;
   uint16_t e_shstrndx;
};


/*! @enum elf_pt
   @brief Program header p_type values
*/
enum elf_pt { PT_NULL = 0, PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_NOTE, PT_SHLIB, PT_PHDR, PT_TLS };


// Program header p_flags bits


#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4


/*! @struct elf64_phdr
   @brief Program header struct
*/
struct elf64_phdr {
   uint32_t p_type;
   uint32_t p_flags;
   uint64_t p_offset;
   uint64_t p_vaddr;
   uint64_t p_paddr;
   uint64_t p_filesz;
   uint64_t p_memsz;
   uint64_t p_align;
};


// ELF header e_machine values (short list)


#define EM_RISCV 243
/**
* \brief Validates and loads an ELF file into memory.
*
* This function validates an ELF file, then loads its contents into memory,
* returning the start of the entry point through \p eptr.
*
* The loader processes only program header entries of type `PT_LOAD`. The layouts
* of structures and magic values can be found in the Linux ELF header file
* `<uapi/linux/elf.h>`
* The implementation should ensure that all loaded sections of the program are
* mapped within the memory range `0x80100000` to `0x81000000`.
*
* Let's do some reading! The following documentation will be very helpful!
* [Helpful doc](https://linux.die.net/man/5/elf)
* Good luck!
* [Educational video](https://www.youtube.com/watch?v=dQw4w9WgXcQ)
*
* \param[in]  uio  Pointer to an user I/O corresponding to the ELF file.
* \param[out] eptr   Double pointer used to return the ELF file's entry point.
*
* \return 0 on success, or a negative error code on failure.
*/
int elf_load(struct uio* uio, void (**eptr)(void)) {
   // FIXME
   struct elf64_ehdr e_hdr;


   unsigned long long pos = 0;


   //const void * buf = kmalloc(1, sizeof(struct elf64_ehdr));


   int success = uio_cntl(uio, FCNTL_SETPOS, &pos);


   if (success) {
       return -EINVAL;
   }


   int num_bytes_read = uio_read(uio, &(e_hdr), sizeof(struct elf64_ehdr));


   if (num_bytes_read != sizeof(struct elf64_ehdr)) {
       return -ENOTSUP;
   }


   // Check for the magic number


   if (e_hdr.e_ident[0] != 0x7f) {
       return -ENOTSUP;
   } else if (e_hdr.e_ident[1] != 'E') {
       return -ENOTSUP;
   } else if (e_hdr.e_ident[2] != 'L') {
       return -ENOTSUP;
   } else if (e_hdr.e_ident[3] != 'F') {
       return -ENOTSUP;
   } else if (e_hdr.e_ident[EI_CLASS] != ELFCLASS64) {
       return -ENOTSUP;
   } else if (e_hdr.e_ident[EI_DATA] != ELFDATA2LSB) {
       return -ENOTSUP;
   } else if (e_hdr.e_type != ET_EXEC) {
       return -ENOTSUP;
   } else if (e_hdr.e_machine != EM_RISCV) {
       return -ENOTSUP;
   } else {
       struct elf64_phdr * phdr_array = kcalloc(e_hdr.e_phnum, sizeof(struct elf64_phdr));
       if (!phdr_array) {
           return -ENOTSUP;
       } else {
           int success = uio_cntl(uio, FCNTL_SETPOS, &(e_hdr.e_phoff));
           if (success) {
               return -ENOTSUP;
           } else {
               num_bytes_read = uio_read(uio, phdr_array, (e_hdr.e_phnum * e_hdr.e_phentsize));
               if (num_bytes_read != (e_hdr.e_phnum * e_hdr.e_phentsize)) {
                   return -ENOTSUP;
               } else {
                   for (int i = 0; i < e_hdr.e_phnum; i++) {
                       if (!(phdr_array[i].p_type == PT_LOAD)) {
                           continue;
                       } else {

                            // 1) Write KTFS tests

                            // 2) Run the KTFS

                            // 3) Use unmkfs_ktfs to make sure it matches what we expect.

                           if (phdr_array[i].p_vaddr < UMEM_START_VMA || phdr_array[i].p_vaddr + phdr_array[i].p_memsz > UMEM_END_VMA || (phdr_array[i].p_filesz > phdr_array[i].p_memsz)) {
                               kfree(phdr_array);
                               return -ENOTSUP;
                           } else {
                               int pte_flags = PTE_U; // Set to user mode

                                pte_flags |= PTE_R;

                                pte_flags |= PTE_W;


                               void * ptr = alloc_and_map_range(phdr_array[i].p_vaddr, ((phdr_array[i].p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)), pte_flags);

                               if (!ptr) {
                                   kfree(phdr_array);
                                   return -ENOMEM;
                               }


                               int success = uio_cntl(uio, FCNTL_SETPOS, &(phdr_array[i].p_offset));
                               if (success) {
                                   kfree(phdr_array);
                                   return -ENOTSUP;
                               }
                               // num_bytes_read = uio_read(uio, (void *) phdr_array[i].p_vaddr, phdr_array[i].p_filesz);


                               num_bytes_read = uio_read(uio, ptr, phdr_array[i].p_filesz);


                               if (num_bytes_read != phdr_array[i].p_filesz) {
                                   kfree(phdr_array);
                                   return -ENOTSUP;
                               } else {
                                   if (phdr_array[i].p_filesz < phdr_array[i].p_memsz) {
                                       //uint64_t start_addr = phdr_array[i].p_vaddr + phdr_array[i].p_filesz;


                                        size_t size_to_zero = phdr_array[i].p_memsz - phdr_array[i].p_filesz;


                                        memset((char *) ptr + phdr_array[i].p_filesz, 0, size_to_zero);
                                    }

                                    pte_flags = PTE_U; // Set to user mode


                                    if (phdr_array[i].p_flags & PF_R) {
                                        pte_flags |= PTE_R;
                                    }


                                    if (phdr_array[i].p_flags & PF_W) {
                                            pte_flags |= PTE_W;
                                    }


                                    if (phdr_array[i].p_flags & PF_X) {
                                        pte_flags |= PTE_X;
                                    }

                                    set_range_flags(ptr, ((phdr_array[i].p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)), pte_flags);
                               }
                           }
                       }
                   }


                   if (e_hdr.e_entry < (unsigned long long) UMEM_START_VMA || e_hdr.e_entry >= (unsigned long long) UMEM_END_VMA) {
                       kfree(phdr_array);
                       return -ENOTSUP;
                   }


                   *eptr = (void (*)(void)) e_hdr.e_entry;


                   kfree(phdr_array);


                   return 0;
               }
           }
       }
   }


   return -ENOTSUP;
}

