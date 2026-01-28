#ifndef VM_ANON_H
#define VM_ANON_H

#include <stddef.h>
#include <stdbool.h>

struct page;
enum vm_type;

struct anon_page {
	/* Swap slot index when swapped out, BITMAP_ERROR when in memory. */
	size_t slot;
};

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);

#endif /* VM_ANON_H */
