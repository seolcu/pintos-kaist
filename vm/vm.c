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

static void frame_attach_page(struct frame *frame, struct page *page);
static void frame_detach_page(struct page *page);
static size_t frame_page_cnt(struct frame *frame);
static uint64_t *page_pml4(struct page *page);

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
		page->cow = false;

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

		if (list_empty(&f->pages))
			continue;

		bool accessed = false;
		for (struct list_elem *e = list_begin(&f->pages);
			 e != list_end(&f->pages); e = list_next(e)) {
			struct page *p = list_entry(e, struct page, frame_elem);
			uint64_t *pml4 = page_pml4(p);
			if (pml4_is_accessed(pml4, p->va)) {
				pml4_set_accessed(pml4, p->va, false);
				accessed = true;
			}
		}
		if (accessed)
			continue;

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
	if (victim == NULL || list_empty(&victim->pages))
		return victim;

	while (!list_empty(&victim->pages)) {
		struct page *page = list_entry(list_front(&victim->pages),
							struct page, frame_elem);
		if (!swap_out(page))
			PANIC("vm_evict_frame: swap_out failed");
	}

	ASSERT(list_empty(&victim->pages));
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
		if (list_empty(&f->pages))
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
			list_init(&frame->pages);
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
	ASSERT(list_empty(&frame->pages));
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
vm_handle_wp(struct page *page)
{
	struct thread *curr = thread_current();
	if (page == NULL || page->frame == NULL)
		return false;
	if (!page->writable || !page->cow)
		return false;

	lock_acquire(&frame_lock);
	struct frame *old = page->frame;
	size_t refcnt = frame_page_cnt(old);
	lock_release(&frame_lock);

	if (refcnt <= 1) {
		/* Sole owner now: just make it writable again. */
		uint64_t *pte = pml4e_walk(curr->pml4, (uint64_t)page->va, 0);
		if (pte == NULL)
			return false;
		*pte |= PTE_W;
		page->cow = false;
		return true;
	}

	struct frame *newf = vm_get_frame();
	memcpy(newf->kva, old->kva, PGSIZE);

	lock_acquire(&frame_lock);

	/* Switch this process's mapping to the new frame. */
	pml4_clear_page(curr->pml4, page->va);
	frame_detach_page(page);
	frame_attach_page(newf, page);
	page->cow = false;

	if (!pml4_set_page(curr->pml4, page->va, newf->kva, true)) {
		frame_detach_page(page);
		frame_attach_page(old, page);
		lock_release(&frame_lock);
		return false;
	}

	lock_release(&frame_lock);
	return true;
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

	uint8_t *rsp = NULL;
	if (user)
		rsp = (uint8_t *) f->rsp;
	else if (thread_current()->pml4 != NULL && thread_current()->user_rsp != 0)
		rsp = (uint8_t *) thread_current()->user_rsp;
	else
		rsp = (uint8_t *) f->rsp;
	uint8_t *uaddr = (uint8_t *) addr;

	/* Heuristic: treat it as stack growth if the fault is close enough to RSP.
	 * Use a small slack (32 bytes) to cover push/call/prologue behaviors. */
	bool is_stack =
		not_present &&
		uaddr < (uint8_t *) USER_STACK &&
		uaddr >= (uint8_t *) (USER_STACK - (1 << 20)) &&
		uaddr >= rsp - 32;

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

	/* Not-present fault: demand paging / stack growth. */
	if (not_present) {
		if (write && !page->writable)
			return false;
		if (page->frame != NULL)
			return true;
		return vm_do_claim_page(page);
	}

	/* Rights-violation fault: handle COW write fault. */
	if (write)
		return vm_handle_wp(page);
	return false;
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
	if (page->frame != NULL)
		return true;

	return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page(struct page *page)
{
	struct frame *frame = vm_get_frame();
	bool success;

	/* Set links */
	lock_acquire(&frame_lock);
	frame_attach_page(frame, page);
	lock_release(&frame_lock);

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	success = pml4_set_page(thread_current()->pml4, page->va, frame->kva,
							page->writable);
	if (!success)
	{
		lock_acquire(&frame_lock);
		frame_detach_page(page);
		lock_release(&frame_lock);
		return false;
	}

	if (!swap_in(page, frame->kva)) {
		pml4_clear_page(thread_current()->pml4, page->va);
		lock_acquire(&frame_lock);
		frame_detach_page(page);
		lock_release(&frame_lock);
		return false;
	}
	return true;
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
	while (hash_next(&it)) {
		struct page *src_page = hash_entry(hash_cur(&it), struct page, spt_elem);
		enum vm_type type = page_get_type(src_page);
		struct page *dst_page = NULL;

		/* Memory-mapped file pages are not inherited. */
		if (type == VM_FILE)
			continue;

		/* If the page is still uninitialized, preserve its lazy initializer. */
		if (VM_TYPE(src_page->operations->type) == VM_UNINIT) {
			struct segment_aux *dst_aux = NULL;
			if (src_page->uninit.aux != NULL) {
				dst_aux = malloc(sizeof *dst_aux);
				if (dst_aux == NULL)
					return false;
				memcpy(dst_aux, src_page->uninit.aux, sizeof *dst_aux);
				if (dst_aux->file != NULL)
					dst_aux->file = file_reopen(dst_aux->file);
			}

			if (!vm_alloc_page_with_initializer(src_page->uninit.type, src_page->va,
											  src_page->writable, src_page->uninit.init,
											  dst_aux)) {
				free(dst_aux);
				return false;
			}
			continue;
		}

		if (!vm_alloc_page(type, src_page->va, src_page->writable))
			return false;
		dst_page = spt_find_page(dst, src_page->va);
		if (dst_page == NULL)
			return false;

		/* If source is already in memory, share the frame and mark as COW. */
		if (src_page->frame != NULL) {
			/* Convert dst_page into the same concrete type as src_page.
			 * (dst_page was created as an uninit page by vm_alloc_page().) */
			dst_page->operations = src_page->operations;
			switch (VM_TYPE(src_page->operations->type)) {
			case VM_ANON:
				dst_page->anon = src_page->anon;
				break;
			case VM_FILE:
				dst_page->file = src_page->file;
				break;
			default:
				break;
			}

			struct frame *f = src_page->frame;
			lock_acquire(&frame_lock);
			frame_attach_page(f, dst_page);
			lock_release(&frame_lock);

			bool cow = src_page->writable;
			src_page->cow = cow;
			dst_page->cow = cow;

			if (!pml4_set_page(thread_current()->pml4, dst_page->va, f->kva, false))
				return false;

			if (cow) {
				uint64_t *ppte = pml4e_walk(src_page->owner->pml4,
									 (uint64_t)src_page->va, 0);
				if (ppte != NULL)
					*ppte &= ~PTE_W;
			}
			continue;
		}

		/* Fallback: eager copy for non-resident pages. */
		if (!vm_do_claim_page(dst_page))
			return false;
		if (src_page->frame != NULL)
			memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
	}
	return true;
}

static void
frame_attach_page(struct frame *frame, struct page *page) {
	ASSERT(frame != NULL);
	ASSERT(page != NULL);
	ASSERT(page->frame == NULL);
	list_push_back(&frame->pages, &page->frame_elem);
	page->frame = frame;
}

static void
frame_detach_page(struct page *page) {
	ASSERT(page != NULL);
	if (page->frame == NULL)
		return;
	list_remove(&page->frame_elem);
	page->frame = NULL;
}

static size_t
frame_page_cnt(struct frame *frame) {
	return list_size(&frame->pages);
}

static uint64_t *
page_pml4(struct page *page) {
	struct thread *t = (page->owner != NULL) ? page->owner : thread_current();
	return t->pml4;
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
