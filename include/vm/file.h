#ifndef VM_FILE_H
#define VM_FILE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "filesys/file.h"
#include <list.h>

struct page;
enum vm_type;

struct mmap_region {
	void *addr;           /* Mapped base address (page-aligned). */
	size_t length;        /* Requested length in bytes. */
	size_t page_cnt;      /* Number of pages mapped. */
	struct file *file;    /* Reopened file for this mapping. */
	off_t offset;         /* File offset for the first page. */
	bool writable;
	struct list_elem elem;
};

struct file_page {
	struct file *file;
	off_t ofs;
	size_t read_bytes;
	size_t zero_bytes;
};

void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);

void *do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset);
void do_munmap (void *addr);

#endif /* VM_FILE_H */
