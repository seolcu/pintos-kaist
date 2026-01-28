/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "lib/kernel/bitmap.h"
#include "threads/synch.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include <string.h>

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static struct bitmap *swap_table;
static struct lock swap_lock;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* Swap disk is attached as hd1:1 (qemu drive index 3). */
	swap_disk = disk_get (1, 1);
	if (swap_disk == NULL)
		PANIC ("swap disk not present (hd1:1)");

	lock_init (&swap_lock);

	size_t sectors_per_page = PGSIZE / DISK_SECTOR_SIZE;
	size_t slot_cnt = disk_size (swap_disk) / sectors_per_page;
	swap_table = bitmap_create (slot_cnt);
	if (swap_table == NULL)
		PANIC ("vm_anon_init: swap_table allocation failed");
	bitmap_set_all (swap_table, false);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;

	struct anon_page *anon_page = &page->anon;
	anon_page->slot = BITMAP_ERROR;
	memset (kva, 0, PGSIZE);
	(void) anon_page;
	(void) type;

	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	if (anon_page->slot == BITMAP_ERROR)
		return false;

	size_t sectors_per_page = PGSIZE / DISK_SECTOR_SIZE;
	disk_sector_t base = (disk_sector_t) (anon_page->slot * sectors_per_page);

	lock_acquire (&swap_lock);
	for (size_t i = 0; i < sectors_per_page; i++)
		disk_read (swap_disk, base + (disk_sector_t) i,
			(uint8_t *) kva + i * DISK_SECTOR_SIZE);
	bitmap_reset (swap_table, anon_page->slot);
	lock_release (&swap_lock);

	anon_page->slot = BITMAP_ERROR;
	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	if (page->frame == NULL)
		return false;

	lock_acquire (&swap_lock);
	size_t slot = bitmap_scan_and_flip (swap_table, 0, 1, false);
	if (slot == BITMAP_ERROR)
		PANIC ("swap disk is full");

	size_t sectors_per_page = PGSIZE / DISK_SECTOR_SIZE;
	disk_sector_t base = (disk_sector_t) (slot * sectors_per_page);
	for (size_t i = 0; i < sectors_per_page; i++)
		disk_write (swap_disk, base + (disk_sector_t) i,
			(uint8_t *) page->frame->kva + i * DISK_SECTOR_SIZE);
	lock_release (&swap_lock);

	anon_page->slot = slot;

	/* Unmap and detach from frame (frame is reused by eviction). */
	if (page->owner != NULL)
		pml4_clear_page (page->owner->pml4, page->va);
	page->frame->page = NULL;
	page->frame = NULL;

	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	if (anon_page->slot != BITMAP_ERROR) {
		lock_acquire (&swap_lock);
		bitmap_reset (swap_table, anon_page->slot);
		lock_release (&swap_lock);
		anon_page->slot = BITMAP_ERROR;
	}

	if (page->frame != NULL) {
		if (page->owner != NULL)
			pml4_clear_page (page->owner->pml4, page->va);
		page->frame->page = NULL;
		page->frame = NULL;
	}
}
