/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include <string.h>
#include "threads/palloc.h"
#include "threads/mmu.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "filesys/file.h"
#include "vm/vm.h"
#include "vm/inspect.h"

static uint64_t page_hash(const struct hash_elem *e, void *aux UNUSED);
static bool page_less(const struct hash_elem *a, const struct hash_elem *b,
					  void *aux UNUSED);
static void spt_destroy_page(struct hash_elem *e, void *aux UNUSED);

static struct list frame_table;
static struct lock frame_lock;
static struct list_elem *clock_hand;

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void)
{
	list_init(&frame_table);
	lock_init(&frame_lock);
	clock_hand = NULL;

	vm_anon_init();
	vm_file_init();
#ifdef EFILESYS /* For project 4 */
	pagecache_init();
#endif
	register_inspect_intr();
	/* DO NOT MODIFY UPPER LINES. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type(struct page *page)
{
	int ty = VM_TYPE(page->operations->type);
	switch (ty)
	{
	case VM_UNINIT:
		return VM_TYPE(page->uninit.type);
	default:
		return ty;
	}
}

static uint64_t
page_hash(const struct hash_elem *e, void *aux UNUSED)
{
	const struct page *page = hash_entry(e, struct page, spt_elem);
	return hash_bytes(&page->va, sizeof page->va);
}

static bool
page_less(const struct hash_elem *a, const struct hash_elem *b,
		  void *aux UNUSED)
{
	const struct page *page_a = hash_entry(a, struct page, spt_elem);
	const struct page *page_b = hash_entry(b, struct page, spt_elem);

	return page_a->va < page_b->va;
}

static void
spt_destroy_page(struct hash_elem *e, void *aux UNUSED)
{
	struct page *page = hash_entry(e, struct page, spt_elem);
	vm_dealloc_page(page);
}

/* Helpers */
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
									vm_initializer *init, void *aux)
{

	ASSERT(VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current()->spt;
	bool (*page_initializer)(struct page *, enum vm_type, void *) = NULL;
	struct page *page = NULL;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page(spt, upage) == NULL)
	{
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */

		/* TODO: Insert the page into the spt. */
		page = malloc(sizeof *page);
		if (page == NULL)
			goto err;

		switch (VM_TYPE(type))
		{
		case VM_ANON:
			page_initializer = anon_initializer;
			break;
		case VM_FILE:
			page_initializer = file_backed_initializer;
			break;
		default:
			goto err;
		}

		uninit_new(page, upage, init, type, aux, page_initializer);
		page->owner = thread_current();
		page->writable = writable;

		if (!spt_insert_page(spt, page))
			goto err;
		return true;
	}
err:
	if (page != NULL)
		free(page);
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page(struct supplemental_page_table *spt UNUSED, void *va UNUSED)
{
	struct page *page = NULL;
	struct page temp;
	struct hash_elem *elem;
	void *rounded = pg_round_down(va);

	temp.va = rounded;
	elem = hash_find(&spt->page_map, &temp.spt_elem);
	if (elem != NULL)
		page = hash_entry(elem, struct page, spt_elem);

	return page;
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table *spt UNUSED,
					 struct page *page UNUSED)
{
	int succ = false;
	struct hash_elem *elem = hash_insert(&spt->page_map, &page->spt_elem);
	succ = (elem == NULL);

	return succ;
}

void spt_remove_page(struct supplemental_page_table *spt, struct page *page)
{
	hash_delete(&spt->page_map, &page->spt_elem);
	vm_dealloc_page(page);
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim(void)
{
	struct frame *victim = NULL;
	if (list_empty(&frame_table))
		return NULL;

	if (clock_hand == NULL || clock_hand == list_end(&frame_table))
		clock_hand = list_begin(&frame_table);

	size_t scanned = 0;
	while (scanned < 2 * list_size(&frame_table))
	{
		if (clock_hand == list_end(&frame_table))
			clock_hand = list_begin(&frame_table);
		struct frame *f = list_entry(clock_hand, struct frame, elem);
		clock_hand = list_next(clock_hand);
		scanned++;

		if (f->page == NULL)
			continue;
		struct page *p = f->page;
		uint64_t *pml4 = (p->owner != NULL) ? p->owner->pml4 : thread_current()->pml4;
		if (pml4_is_accessed(pml4, p->va))
		{
			pml4_set_accessed(pml4, p->va, false);
			continue;
		}
		victim = f;
		break;
	}

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame(void)
{
	struct frame *victim = vm_get_victim();
	if (victim == NULL || victim->page == NULL)
		return victim;

	struct page *page = victim->page;
	if (!swap_out(page))
		PANIC("vm_evict_frame: swap_out failed");

	ASSERT(victim->page == NULL);
	return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame(void)
{
	struct frame *frame = NULL;
	lock_acquire(&frame_lock);

	/* First, reuse a free frame if possible. */
	for (struct list_elem *e = list_begin(&frame_table);
		e != list_end(&frame_table); e = list_next(e))
	{
		struct frame *f = list_entry(e, struct frame, elem);
		if (f->page == NULL)
		{
			frame = f;
			break;
		}
	}

	if (frame == NULL)
	{
		void *kva = palloc_get_page(PAL_USER);
		if (kva != NULL)
		{
			frame = malloc(sizeof *frame);
			if (frame == NULL)
			{
				palloc_free_page(kva);
				PANIC("vm_get_frame: frame allocation failed");
			}
			frame->kva = kva;
			frame->page = NULL;
			list_push_back(&frame_table, &frame->elem);
		}
		else
		{
			frame = vm_evict_frame();
			if (frame == NULL)
				PANIC("vm_get_frame: cannot evict frame");
		}
	}

	lock_release(&frame_lock);

	ASSERT(frame != NULL);
	ASSERT(frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth(void *addr UNUSED)
{
	uint8_t *va = pg_round_down(addr);
	uint8_t *limit = (uint8_t *)USER_STACK - (1 << 20);

	if (va < limit)
		return;

	for (uint8_t *p = va; p < (uint8_t *)USER_STACK; p += PGSIZE)
	{
		if (spt_find_page(&thread_current()->spt, p) != NULL)
			break;

		if (!vm_alloc_page(VM_ANON, p, true))
			return;
		if (!vm_claim_page(p))
			return;
	}
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp(struct page *page UNUSED)
{
	return false;
}

/* Return true on success */
bool vm_try_handle_fault(struct intr_frame *f UNUSED, void *addr UNUSED,
						 bool user UNUSED, bool write UNUSED, bool not_present UNUSED)
{
	struct supplemental_page_table *spt UNUSED = &thread_current()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

	if (addr == NULL || !is_user_vaddr(addr))
		return false;

	uint8_t *rsp = (uint8_t *) f->rsp;
	uint8_t *uaddr = (uint8_t *) addr;

	bool is_stack =
		not_present &&
		uaddr < (uint8_t *) USER_STACK &&
		uaddr >= (uint8_t *) (USER_STACK - (1 << 20)) &&
		uaddr >= rsp - 8;

	page = spt_find_page(spt, addr);
	if (page == NULL && is_stack)
	{
		vm_stack_growth(addr);
		page = spt_find_page(spt, addr);
		if (page == NULL)
			return false;
	}
	if (page == NULL)
		return false;
	if (write && !page->writable)
		return false;
	if (!not_present)
		return false;

	return vm_do_claim_page(page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page)
{
	destroy(page);
	free(page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page(void *va UNUSED)
{
	struct page *page = NULL;
	/* TODO: Fill this function */
	page = spt_find_page(&thread_current()->spt, va);
	if (page == NULL)
		return false;

	return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page(struct page *page)
{
	struct frame *frame = vm_get_frame();
	bool success;

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	success = pml4_set_page(thread_current()->pml4, page->va, frame->kva,
							page->writable);
	if (!success)
	{
		palloc_free_page(frame->kva);
		free(frame);
		page->frame = NULL;
		return false;
	}

	return swap_in(page, frame->kva);
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table *spt UNUSED)
{
	hash_init(&spt->page_map, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table *dst UNUSED,
								  struct supplemental_page_table *src UNUSED)
{
	struct hash_iterator it;

	hash_first(&it, &src->page_map);
	while (hash_next(&it))
	{
		struct page *src_page = hash_entry(hash_cur(&it),
									   struct page, spt_elem);
		enum vm_type type = page_get_type(src_page);
		struct page *dst_page = NULL;

		/* Memory-mapped file pages are not inherited. */
		if (type == VM_FILE)
			continue;

		if (type == VM_UNINIT)
		{
			struct segment_aux *dst_aux = NULL;
			if (src_page->uninit.aux != NULL)
			{
				dst_aux = malloc(sizeof *dst_aux);
				if (dst_aux == NULL)
					return false;
				memcpy(dst_aux, src_page->uninit.aux, sizeof *dst_aux);
				if (dst_aux->file != NULL)
					dst_aux->file = file_reopen(dst_aux->file);
			}

			if (!vm_alloc_page_with_initializer(src_page->uninit.type,
												src_page->va, src_page->writable,
												src_page->uninit.init, dst_aux))
			{
				free(dst_aux);
				return false;
			}
			continue;
		}

		if (!vm_alloc_page(type, src_page->va, src_page->writable))
			return false;
		dst_page = spt_find_page(dst, src_page->va);
		if (dst_page == NULL || !vm_do_claim_page(dst_page))
			return false;

		if (src_page->frame != NULL)
			memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
	}
	return true;
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt UNUSED)
{
	/* Unmap all memory-mapped files. */
	while (!list_empty(&thread_current()->mmap_list))
	{
		struct mmap_region *mr = list_entry(list_front(&thread_current()->mmap_list),
									struct mmap_region, elem);
		do_munmap(mr->addr);
	}

	hash_destroy(&spt->page_map, spt_destroy_page);
}
