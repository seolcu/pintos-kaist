#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/fat.h"
#ifndef EFILESYS
#include "filesys/free-map.h"
#endif
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
 * Must be exactly DISK_SECTOR_SIZE bytes long. */
struct inode_disk {
	cluster_t start;                    /* First data cluster. */
	off_t length;                       /* File size in bytes. */
	unsigned magic;                     /* Magic number. */
	uint32_t type;                      /* enum inode_type */
	uint32_t unused[124];               /* Not used. */
};

/* Returns the number of sectors to allocate for an inode SIZE
 * bytes long. */
static inline size_t
bytes_to_sectors (off_t size) {
	return DIV_ROUND_UP (size, DISK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode {
	struct list_elem elem;              /* Element in inode list. */
	disk_sector_t sector;               /* Sector number of disk location. */
	int open_cnt;                       /* Number of openers. */
	bool removed;                       /* True if deleted, false otherwise. */
	int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
	struct inode_disk data;             /* Inode content. */
};

#ifdef EFILESYS
static void
inode_sync (const struct inode *inode) {
	disk_write (filesys_disk, cluster_to_sector (inode->sector),
	            (void *) &inode->data);
}

static cluster_t
inode_last_cluster (const struct inode *inode) {
	size_t sectors = bytes_to_sectors (inode->data.length);
	if (sectors == 0)
		return 0;

	cluster_t clst = inode->data.start;
	for (size_t i = 1; i < sectors; i++)
		clst = fat_get (clst);
	return clst;
}

static bool
inode_grow (struct inode *inode, off_t new_length) {
	ASSERT (new_length >= inode->data.length);

	const off_t old_length = inode->data.length;
	const size_t old_sectors = bytes_to_sectors (old_length);
	const size_t new_sectors = bytes_to_sectors (new_length);
	if (new_sectors == old_sectors) {
		inode->data.length = new_length;
		inode_sync (inode);
		return true;
	}

	cluster_t orig_start = inode->data.start;
	cluster_t prev_last = old_sectors == 0 ? 0 : inode_last_cluster (inode);
	cluster_t cur_last = prev_last;
	cluster_t first_new = 0;

	for (size_t i = old_sectors; i < new_sectors; i++) {
		cluster_t new_clst = fat_create_chain (cur_last);
		if (new_clst == 0) {
			if (first_new != 0)
				fat_remove_chain (first_new, prev_last);
			inode->data.start = orig_start;
			inode->data.length = old_length;
			return false;
		}
		if (orig_start == 0 && inode->data.start == 0)
			inode->data.start = new_clst;
		if (first_new == 0)
			first_new = new_clst;
		cur_last = new_clst;
	}

	inode->data.length = new_length;
	inode_sync (inode);
	return true;
}
#endif

/* Returns the disk sector that contains byte offset POS within
 * INODE.
 * Returns -1 if INODE does not contain data for a byte at offset
 * POS. */
static disk_sector_t
byte_to_sector (const struct inode *inode, off_t pos) {
	ASSERT (inode != NULL);
	if (pos >= inode->data.length)
		return -1;

#ifdef EFILESYS
	if (inode->data.start == 0)
		return -1;

	const size_t clst_idx = pos / DISK_SECTOR_SIZE;
	cluster_t clst = inode->data.start;
	for (size_t i = 0; i < clst_idx; i++) {
		clst = fat_get (clst);
		if (clst == EOChain)
			return -1;
	}
	return cluster_to_sector (clst);
#else
	return inode->data.start + pos / DISK_SECTOR_SIZE;
#endif
}

/* List of open inodes, so that opening a single inode twice
 * returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) {
	list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
 * writes the new inode to sector SECTOR on the file system
 * disk.
 * Returns true if successful.
 * Returns false if memory or disk allocation fails. */
static bool
inode_create_internal (disk_sector_t sector, off_t length, enum inode_type type) {
	struct inode_disk *disk_inode = NULL;
	bool success = false;

	ASSERT (length >= 0);

	/* If this assertion fails, the inode structure is not exactly
	 * one sector in size, and you should fix that. */
	ASSERT (sizeof *disk_inode == DISK_SECTOR_SIZE);

	disk_inode = calloc (1, sizeof *disk_inode);
	if (disk_inode != NULL) {
		size_t sectors = bytes_to_sectors (length);
		disk_inode->length = length;
		disk_inode->magic = INODE_MAGIC;
		disk_inode->type = (uint32_t) type;
		disk_inode->start = 0;

#ifdef EFILESYS
		cluster_t first = 0;
		cluster_t prev = 0;
		bool alloc_ok = true;
		for (size_t i = 0; i < sectors; i++) {
			cluster_t new_clst = fat_create_chain (prev);
			if (new_clst == 0) {
				alloc_ok = false;
				break;
			}
			if (first == 0)
				first = new_clst;
			prev = new_clst;
		}
		if (alloc_ok) {
			disk_inode->start = first;
			disk_write (filesys_disk, cluster_to_sector (sector), disk_inode);
			success = true;
		} else if (first != 0)
			fat_remove_chain (first, 0);
#else
		if (free_map_allocate (sectors, &disk_inode->start)) {
			disk_write (filesys_disk, sector, disk_inode);
			if (sectors > 0) {
				static char zeros[DISK_SECTOR_SIZE];
				size_t i;

				for (i = 0; i < sectors; i++)
					disk_write (filesys_disk, disk_inode->start + i, zeros);
			}
			success = true;
		}
#endif
		free (disk_inode);
	}
	return success;
}

bool
inode_create (disk_sector_t sector, off_t length) {
	return inode_create_internal (sector, length, INODE_FILE);
}

bool
inode_create_dir (disk_sector_t sector, off_t length) {
	return inode_create_internal (sector, length, INODE_DIR);
}

bool
inode_create_symlink (disk_sector_t sector, off_t length) {
	return inode_create_internal (sector, length, INODE_SYMLINK);
}

/* Reads an inode from SECTOR
 * and returns a `struct inode' that contains it.
 * Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (disk_sector_t sector) {
	struct list_elem *e;
	struct inode *inode;

	/* Check whether this inode is already open. */
	for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
			e = list_next (e)) {
		inode = list_entry (e, struct inode, elem);
		if (inode->sector == sector) {
			inode_reopen (inode);
			return inode; 
		}
	}

	/* Allocate memory. */
	inode = malloc (sizeof *inode);
	if (inode == NULL)
		return NULL;

	/* Initialize. */
	list_push_front (&open_inodes, &inode->elem);
	inode->sector = sector;
	inode->open_cnt = 1;
	inode->deny_write_cnt = 0;
	inode->removed = false;
	#ifdef EFILESYS
	disk_read (filesys_disk, cluster_to_sector (inode->sector), &inode->data);
	#else
	disk_read (filesys_disk, inode->sector, &inode->data);
	#endif
	return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode) {
	if (inode != NULL)
		inode->open_cnt++;
	return inode;
}

/* Returns INODE's inode number. */
disk_sector_t
inode_get_inumber (const struct inode *inode) {
	return inode->sector;
}

int
inode_get_open_cnt (const struct inode *inode) {
	return inode != NULL ? inode->open_cnt : 0;
}

bool
inode_is_dir (const struct inode *inode) {
	return inode != NULL && inode->data.type == INODE_DIR;
}

bool
inode_is_symlink (const struct inode *inode) {
	return inode != NULL && inode->data.type == INODE_SYMLINK;
}

/* Closes INODE and writes it to disk.
 * If this was the last reference to INODE, frees its memory.
 * If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) {
	/* Ignore null pointer. */
	if (inode == NULL)
		return;

	/* Release resources if this was the last opener. */
	if (--inode->open_cnt == 0) {
		/* Remove from inode list and release lock. */
		list_remove (&inode->elem);

		/* Deallocate blocks if removed. */
		if (inode->removed) {
			#ifdef EFILESYS
			if (inode->data.start != 0)
				fat_remove_chain (inode->data.start, 0);
			if (inode->sector != ROOT_DIR_SECTOR)
				fat_remove_chain (inode->sector, 0);
			#else
			free_map_release (inode->sector, 1);
			free_map_release (inode->data.start,
					bytes_to_sectors (inode->data.length));
			#endif
		}

		free (inode); 
	}
}

/* Marks INODE to be deleted when it is closed by the last caller who
 * has it open. */
void
inode_remove (struct inode *inode) {
	ASSERT (inode != NULL);
	inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
 * Returns the number of bytes actually read, which may be less
 * than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) {
	uint8_t *buffer = buffer_;
	off_t bytes_read = 0;
	uint8_t *bounce = NULL;

	while (size > 0) {
		/* Disk sector to read, starting byte offset within sector. */
		disk_sector_t sector_idx = byte_to_sector (inode, offset);
		int sector_ofs = offset % DISK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		off_t inode_left = inode_length (inode) - offset;
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;
		int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually copy out of this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
			/* Read full sector directly into caller's buffer. */
			disk_read (filesys_disk, sector_idx, buffer + bytes_read); 
		} else {
			/* Read sector into bounce buffer, then partially copy
			 * into caller's buffer. */
			if (bounce == NULL) {
				bounce = malloc (DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}
			disk_read (filesys_disk, sector_idx, bounce);
			memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_read += chunk_size;
	}
	free (bounce);

	return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
 * Returns the number of bytes actually written, which may be
 * less than SIZE if end of file is reached or an error occurs.
 * (Normally a write at end of file would extend the inode, but
 * growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
		off_t offset) {
	const uint8_t *buffer = buffer_;
	off_t bytes_written = 0;
	uint8_t *bounce = NULL;

	if (inode->deny_write_cnt)
		return 0;

#ifdef EFILESYS
	off_t end_pos = offset + size;
	if (end_pos > inode->data.length)
		if (!inode_grow (inode, end_pos))
			return 0;
#endif

	while (size > 0) {
		/* Sector to write, starting byte offset within sector. */
		disk_sector_t sector_idx = byte_to_sector (inode, offset);
		int sector_ofs = offset % DISK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		off_t inode_left = inode_length (inode) - offset;
		int sector_left = DISK_SECTOR_SIZE - sector_ofs;
		int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually write into this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) {
			/* Write full sector directly to disk. */
			disk_write (filesys_disk, sector_idx, buffer + bytes_written); 
		} else {
			/* We need a bounce buffer. */
			if (bounce == NULL) {
				bounce = malloc (DISK_SECTOR_SIZE);
				if (bounce == NULL)
					break;
			}

			/* If the sector contains data before or after the chunk
			   we're writing, then we need to read in the sector
			   first.  Otherwise we start with a sector of all zeros. */
			if (sector_ofs > 0 || chunk_size < sector_left) 
				disk_read (filesys_disk, sector_idx, bounce);
			else
				memset (bounce, 0, DISK_SECTOR_SIZE);
			memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
			disk_write (filesys_disk, sector_idx, bounce); 
		}

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_written += chunk_size;
	}
	free (bounce);

	return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
	void
inode_deny_write (struct inode *inode) 
{
	inode->deny_write_cnt++;
	ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
 * Must be called once by each inode opener who has called
 * inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) {
	ASSERT (inode->deny_write_cnt > 0);
	ASSERT (inode->deny_write_cnt <= inode->open_cnt);
	inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode) {
	return inode->data.length;
}
