/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "vm/file.h"

#include <string.h>

#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;
	(void) file_page;
	(void) type;
	(void) kva;
	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page = &page->file;
	if (file_page->file == NULL)
		return false;

	if (file_read_at (file_page->file, kva, file_page->read_bytes,
			file_page->ofs) != (int) file_page->read_bytes)
		return false;
	memset ((uint8_t *) kva + file_page->read_bytes, 0, file_page->zero_bytes);
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page = &page->file;
	struct thread *t = (page->owner != NULL) ? page->owner : thread_current ();

	if (page->frame != NULL && file_page->file != NULL && page->writable &&
		pml4_is_dirty (t->pml4, page->va)) {
		file_write_at (file_page->file, page->frame->kva,
			file_page->read_bytes, file_page->ofs);
		pml4_set_dirty (t->pml4, page->va, false);
	}

	if (page->frame != NULL) {
		pml4_clear_page (t->pml4, page->va);
		page->frame->page = NULL;
		page->frame = NULL;
	}

	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	(void) file_backed_swap_out (page);
}

struct mmap_page_aux {
	struct file *file;
	off_t ofs;
	size_t read_bytes;
	size_t zero_bytes;
};

static bool
lazy_load_mmap (struct page *page, void *aux_) {
	struct mmap_page_aux *aux = aux_;
	struct file_page *file_page = &page->file;

	file_page->file = aux->file;
	file_page->ofs = aux->ofs;
	file_page->read_bytes = aux->read_bytes;
	file_page->zero_bytes = aux->zero_bytes;

	free (aux);

	if (file_page->file == NULL)
		return false;

	if (file_read_at (file_page->file, page->frame->kva, file_page->read_bytes,
			file_page->ofs) != (int) file_page->read_bytes)
		return false;
	memset ((uint8_t *) page->frame->kva + file_page->read_bytes, 0,
			file_page->zero_bytes);
	return true;
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	struct thread *t = thread_current ();
	struct supplemental_page_table *spt = &t->spt;

	if (addr == NULL)
		return NULL;
	if (pg_ofs (addr) != 0)
		return NULL;
	if (length == 0)
		return NULL;
	if (file == NULL)
		return NULL;
	if (offset < 0 || pg_ofs (offset) != 0)
		return NULL;

	/* Refuse to map over kernel address space. */
	uintptr_t start = (uintptr_t) addr;
	uintptr_t end = start + length - 1;
	if (end < start)
		return NULL;
	if (!is_user_vaddr ((void *) start) || !is_user_vaddr ((void *) end))
		return NULL;

	int file_len = file_length (file);
	if (file_len <= 0)
		return NULL;

	size_t page_cnt = (length + PGSIZE - 1) / PGSIZE;
	if (page_cnt == 0)
		return NULL;

	/* Check overlap with existing mappings. */
	for (size_t i = 0; i < page_cnt; i++) {
		void *va = (uint8_t *) addr + i * PGSIZE;
		if (spt_find_page (spt, va) != NULL)
			return NULL;
		if (pml4_get_page (t->pml4, va) != NULL)
			return NULL;
	}

	struct mmap_region *mr = malloc (sizeof *mr);
	if (mr == NULL)
		return NULL;
	*mr = (struct mmap_region) {
		.addr = addr,
		.length = length,
		.page_cnt = page_cnt,
		.file = file_reopen (file),
		.offset = offset,
		.writable = writable != 0,
	};
	if (mr->file == NULL) {
		free (mr);
		return NULL;
	}
	list_push_back (&t->mmap_list, &mr->elem);

	for (size_t i = 0; i < page_cnt; i++) {
		void *va = (uint8_t *) addr + i * PGSIZE;
		off_t ofs = offset + (off_t) (i * PGSIZE);

		size_t file_left = 0;
		if ((int64_t) ofs < file_len)
			file_left = (size_t) file_len - (size_t) ofs;

		size_t read_bytes = file_left < PGSIZE ? file_left : PGSIZE;
		size_t zero_bytes = PGSIZE - read_bytes;

		struct mmap_page_aux *aux = malloc (sizeof *aux);
		if (aux == NULL)
			goto fail;
		*aux = (struct mmap_page_aux) {
			.file = mr->file,
			.ofs = ofs,
			.read_bytes = read_bytes,
			.zero_bytes = zero_bytes,
		};

		if (!vm_alloc_page_with_initializer (VM_FILE, va, writable != 0,
				lazy_load_mmap, aux)) {
			free (aux);
			goto fail;
		}
	}

	return addr;

fail:
	do_munmap (addr);
	return NULL;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	struct thread *t = thread_current ();
	struct supplemental_page_table *spt = &t->spt;

	struct mmap_region *mr = NULL;
	for (struct list_elem *e = list_begin (&t->mmap_list);
		e != list_end (&t->mmap_list); e = list_next (e)) {
		struct mmap_region *cand = list_entry (e, struct mmap_region, elem);
		if (cand->addr == addr) {
			mr = cand;
			break;
		}
	}
	if (mr == NULL)
		return;

	for (size_t i = 0; i < mr->page_cnt; i++) {
		void *va = (uint8_t *) mr->addr + i * PGSIZE;
		struct page *page = spt_find_page (spt, va);
		if (page != NULL)
			spt_remove_page (spt, page);
	}

	list_remove (&mr->elem);
	file_close (mr->file);
	free (mr);
}
