#include "userprog/syscall.h"
#include "intrinsic.h"
#include "devices/input.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/init.h"
#include "threads/loader.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/gdt.h"
#include "userprog/process.h"
#ifdef VM
#include "vm/vm.h"
#include "vm/file.h"
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <syscall-nr.h>

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

struct lock filesys_lock;

static void sys_exit(int status) NO_RETURN;
static void sys_exit(int status) {
  struct thread *curr = thread_current();
  curr->exit_status = status;
  thread_exit();
}

static void validate_user_address(const void *uaddr) {
  struct thread *curr = thread_current();
  if (uaddr == NULL || !is_user_vaddr(uaddr))
    sys_exit(-1);
  if (curr->pml4 == NULL)
    sys_exit(-1);

  if (pml4_get_page(curr->pml4, uaddr) == NULL) {
#ifdef VM
    if (!vm_claim_page(pg_round_down(uaddr)))
      sys_exit(-1);
    if (pml4_get_page(curr->pml4, uaddr) == NULL)
      sys_exit(-1);
#else
    sys_exit(-1);
#endif
  }
}

static void validate_user_buffer(const void *buffer, size_t size) {
  if (size == 0)
    return;

  uintptr_t start = (uintptr_t)buffer;
  uintptr_t end = start + size - 1;
  if (end < start)
    sys_exit(-1);

  validate_user_address((const void *)start);
  validate_user_address((const void *)end);

  for (uintptr_t page = (uintptr_t)pg_round_down((const void *)start);
       page <= (uintptr_t)pg_round_down((const void *)end); page += PGSIZE) {
    validate_user_address((const void *)page);
  }
}

static void validate_user_writable_buffer(void *buffer, size_t size) {
  if (size == 0)
    return;

  validate_user_buffer(buffer, size);

  struct thread *curr = thread_current();
  uintptr_t start = (uintptr_t)buffer;
  uintptr_t end = start + size - 1;

  for (uintptr_t page = (uintptr_t)pg_round_down((const void *)start);
       page <= (uintptr_t)pg_round_down((const void *)end); page += PGSIZE) {
    /* Make sure the page is present (lazy/swap-in). */
    validate_user_address((const void *)page);
    uint64_t *pte = pml4e_walk(curr->pml4, page, 0);
    if (pte == NULL || ((*pte & PTE_P) == 0) || !is_user_pte(pte) ||
        !is_writable(pte))
      sys_exit(-1);
  }
}

static char *copy_in_string(const char *ustr) {
  if (ustr == NULL)
    sys_exit(-1);

  struct thread *curr = thread_current();
  char *kstr = palloc_get_page(0);
  if (kstr == NULL)
    sys_exit(-1);

  for (size_t i = 0; i < PGSIZE; i++) {
    const void *uaddr = (const void *)(ustr + i);
    if (uaddr == NULL || !is_user_vaddr(uaddr) || curr->pml4 == NULL) {
      palloc_free_page(kstr);
      sys_exit(-1);
    }
    const char *kaddr = pml4_get_page(curr->pml4, uaddr);
    if (kaddr == NULL) {
#ifdef VM
      if (!vm_claim_page(pg_round_down(uaddr))) {
        palloc_free_page(kstr);
        sys_exit(-1);
      }
      kaddr = pml4_get_page(curr->pml4, uaddr);
      if (kaddr == NULL) {
        palloc_free_page(kstr);
        sys_exit(-1);
      }
#else
      palloc_free_page(kstr);
      sys_exit(-1);
#endif
    }

    kstr[i] = *kaddr;
    if (kstr[i] == '\0')
      return kstr;
  }

  palloc_free_page(kstr);
  sys_exit(-1);
}

static struct file *get_file(int fd) {
  struct thread *curr = thread_current();
  if (fd < 2 || fd >= 128)
    return NULL;
  return curr->fd_table[fd];
}

static int allocate_fd(struct file *file) {
  struct thread *curr = thread_current();
  for (int fd = curr->next_fd; fd < 128; fd++) {
    if (curr->fd_table[fd] == NULL) {
      curr->fd_table[fd] = file;
      curr->next_fd = fd + 1;
      return fd;
    }
  }
  for (int fd = 2; fd < curr->next_fd; fd++) {
    if (curr->fd_table[fd] == NULL) {
      curr->fd_table[fd] = file;
      return fd;
    }
  }
  curr->next_fd = 128;
  return -1;
}

static void close_fd(int fd) {
  struct thread *curr = thread_current();
  if (fd < 2 || fd >= 128)
    return;
  if (curr->fd_table[fd] == NULL)
    return;
  file_close(curr->fd_table[fd]);
  curr->fd_table[fd] = NULL;
  if (fd < curr->next_fd)
    curr->next_fd = fd;
}

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void) {
  lock_init(&filesys_lock);

  write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG)
                                                               << 32);
  write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

  /* The interrupt service rountine should not serve any interrupts
   * until the syscall_entry swaps the userland stack to the kernel
   * mode stack. Therefore, we masked the FLAG_FL. */
  write_msr(MSR_SYSCALL_MASK,
            FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f) {
  int syscall_num = f->R.rax;

  switch (syscall_num) {
  case SYS_HALT:
    power_off();

  case SYS_EXIT:
    sys_exit((int)f->R.rdi);

  case SYS_FORK: {
    char *thread_name = copy_in_string((const char *)f->R.rdi);
    tid_t tid = process_fork(thread_name, f);
    palloc_free_page(thread_name);
    f->R.rax = tid;
    return;
  }

  case SYS_EXEC: {
    char *cmd_line = copy_in_string((const char *)f->R.rdi);
    int result = process_exec(cmd_line);
    f->R.rax = result;
    return;
  }

  case SYS_WAIT: {
    tid_t tid = (tid_t)f->R.rdi;
    f->R.rax = process_wait(tid);
    return;
  }

  case SYS_CREATE: {
    char *file = copy_in_string((const char *)f->R.rdi);
    unsigned initial_size = (unsigned)f->R.rsi;

    lock_acquire(&filesys_lock);
    bool ok = filesys_create(file, initial_size);
    lock_release(&filesys_lock);

    palloc_free_page(file);
    f->R.rax = ok;
    return;
  }

  case SYS_REMOVE: {
    char *file = copy_in_string((const char *)f->R.rdi);

    lock_acquire(&filesys_lock);
    bool ok = filesys_remove(file);
    lock_release(&filesys_lock);

    palloc_free_page(file);
    f->R.rax = ok;
    return;
  }

  case SYS_OPEN: {
    char *file_name = copy_in_string((const char *)f->R.rdi);

    lock_acquire(&filesys_lock);
    struct file *file = filesys_open(file_name);
    lock_release(&filesys_lock);

    palloc_free_page(file_name);
    if (file == NULL) {
      f->R.rax = -1;
      return;
    }

    int fd = allocate_fd(file);
    if (fd < 0) {
      file_close(file);
      f->R.rax = -1;
      return;
    }
    f->R.rax = fd;
    return;
  }

  case SYS_FILESIZE: {
    int fd = (int)f->R.rdi;
    struct file *file = get_file(fd);
    if (file == NULL) {
      f->R.rax = -1;
      return;
    }

    lock_acquire(&filesys_lock);
    int length = (int)file_length(file);
    lock_release(&filesys_lock);

    f->R.rax = length;
    return;
  }

  case SYS_READ: {
    int fd = (int)f->R.rdi;
    void *buffer = (void *)f->R.rsi;
    unsigned size = (unsigned)f->R.rdx;

    validate_user_writable_buffer(buffer, size);

    if (fd == 0) {
      for (unsigned i = 0; i < size; i++) {
        uint8_t *dst = pml4_get_page(thread_current()->pml4,
                                     (const void *)((uint8_t *)buffer + i));
        *dst = input_getc();
      }
      f->R.rax = (int)size;
      return;
    }
    if (fd == 1) {
      f->R.rax = -1;
      return;
    }

    struct file *file = get_file(fd);
    if (file == NULL) {
      f->R.rax = -1;
      return;
    }

    off_t bytes_read = 0;
    lock_acquire(&filesys_lock);
    while (size > 0) {
      size_t page_left = PGSIZE - pg_ofs(buffer);
      size_t chunk = size < page_left ? size : page_left;
      void *kaddr = pml4_get_page(thread_current()->pml4, buffer);
      if (kaddr == NULL) {
        lock_release(&filesys_lock);
        sys_exit(-1);
      }

      off_t n = file_read(file, kaddr, (off_t)chunk);
      if (n <= 0)
        break;
      bytes_read += n;
      buffer = (uint8_t *)buffer + n;
      size -= (unsigned)n;
      if ((size_t)n < chunk)
        break;
    }
    lock_release(&filesys_lock);
    f->R.rax = (int)bytes_read;
    return;
  }

  case SYS_WRITE: {
    int fd = (int)f->R.rdi;
    const void *buffer = (const void *)f->R.rsi;
    unsigned size = (unsigned)f->R.rdx;

    validate_user_buffer(buffer, size);

    if (fd == 1) {
      putbuf(buffer, size);
      f->R.rax = (int)size;
      return;
    }
    if (fd == 0) {
      f->R.rax = -1;
      return;
    }

    struct file *file = get_file(fd);
    if (file == NULL) {
      f->R.rax = -1;
      return;
    }

    off_t bytes_written = 0;
    lock_acquire(&filesys_lock);
    while (size > 0) {
      size_t page_left = PGSIZE - pg_ofs(buffer);
      size_t chunk = size < page_left ? size : page_left;
      const void *kaddr = pml4_get_page(thread_current()->pml4, buffer);
      if (kaddr == NULL) {
        lock_release(&filesys_lock);
        sys_exit(-1);
      }

      off_t n = file_write(file, kaddr, (off_t)chunk);
      if (n <= 0)
        break;
      bytes_written += n;
      buffer = (const uint8_t *)buffer + n;
      size -= (unsigned)n;
      if ((size_t)n < chunk)
        break;
    }
    lock_release(&filesys_lock);
    f->R.rax = (int)bytes_written;
    return;
  }

  case SYS_SEEK: {
    int fd = (int)f->R.rdi;
    unsigned position = (unsigned)f->R.rsi;
    struct file *file = get_file(fd);
    if (file == NULL)
      return;

    lock_acquire(&filesys_lock);
    file_seek(file, (off_t)position);
    lock_release(&filesys_lock);
    return;
  }

  case SYS_TELL: {
    int fd = (int)f->R.rdi;
    struct file *file = get_file(fd);
    if (file == NULL) {
      f->R.rax = -1;
      return;
    }

    lock_acquire(&filesys_lock);
    off_t pos = file_tell(file);
    lock_release(&filesys_lock);
    f->R.rax = (int)pos;
    return;
  }

  case SYS_CLOSE: {
    int fd = (int)f->R.rdi;
    lock_acquire(&filesys_lock);
    close_fd(fd);
    lock_release(&filesys_lock);
    return;
  }

#ifdef VM
  case SYS_MMAP: {
    void *addr = (void *)f->R.rdi;
    size_t length = (size_t)f->R.rsi;
    int writable = (int)f->R.rdx;
    int fd = (int)f->R.r10;
    off_t offset = (off_t)f->R.r8;

    struct file *file = get_file(fd);
    if (file == NULL) {
      f->R.rax = (uint64_t)NULL;
      return;
    }

    f->R.rax = (uint64_t)do_mmap(addr, length, writable, file, offset);
    return;
  }

  case SYS_MUNMAP: {
    void *addr = (void *)f->R.rdi;
    do_munmap(addr);
    return;
  }
#endif

  default:
    sys_exit(-1);
  }
}
