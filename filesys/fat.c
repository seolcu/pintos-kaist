#include "filesys/fat.h"
#include "devices/disk.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include <stdio.h>
#include <string.h>

/* Should be less than DISK_SECTOR_SIZE */
struct fat_boot {
	unsigned int magic;
	unsigned int sectors_per_cluster; /* Fixed to 1 */
	unsigned int total_sectors;
	unsigned int fat_start;
	unsigned int fat_sectors; /* Size of FAT in sectors. */
	unsigned int root_dir_cluster;
};

/* FAT FS */
struct fat_fs {
	struct fat_boot bs;
	unsigned int *fat;
	unsigned int fat_length;
	disk_sector_t data_start;
	cluster_t last_clst;
	struct lock write_lock;
};

static struct fat_fs *fat_fs;

void fat_boot_create (void);
void fat_fs_init (void);

void
fat_init (void) {
	fat_fs = calloc (1, sizeof (struct fat_fs));
	if (fat_fs == NULL)
		PANIC ("FAT init failed");

	// Read boot sector from the disk
	unsigned int *bounce = malloc (DISK_SECTOR_SIZE);
	if (bounce == NULL)
		PANIC ("FAT init failed");
	disk_read (filesys_disk, FAT_BOOT_SECTOR, bounce);
	memcpy (&fat_fs->bs, bounce, sizeof (fat_fs->bs));
	free (bounce);

	// Extract FAT info
	if (fat_fs->bs.magic != FAT_MAGIC)
		fat_boot_create ();
	fat_fs_init ();
}

void
fat_open (void) {
	fat_fs->fat = calloc (fat_fs->fat_length, sizeof (cluster_t));
	if (fat_fs->fat == NULL)
		PANIC ("FAT load failed");

	// Load FAT directly from the disk
	uint8_t *buffer = (uint8_t *) fat_fs->fat;
	off_t bytes_read = 0;
	off_t bytes_left = sizeof (fat_fs->fat);
	const off_t fat_size_in_bytes = fat_fs->fat_length * sizeof (cluster_t);
	for (unsigned i = 0; i < fat_fs->bs.fat_sectors; i++) {
		bytes_left = fat_size_in_bytes - bytes_read;
		if (bytes_left >= DISK_SECTOR_SIZE) {
			disk_read (filesys_disk, fat_fs->bs.fat_start + i,
			           buffer + bytes_read);
			bytes_read += DISK_SECTOR_SIZE;
		} else {
			uint8_t *bounce = malloc (DISK_SECTOR_SIZE);
			if (bounce == NULL)
				PANIC ("FAT load failed");
			disk_read (filesys_disk, fat_fs->bs.fat_start + i, bounce);
			memcpy (buffer + bytes_read, bounce, bytes_left);
			bytes_read += bytes_left;
			free (bounce);
		}
	}
}

void
fat_close (void) {
	// Write FAT boot sector
	uint8_t *bounce = calloc (1, DISK_SECTOR_SIZE);
	if (bounce == NULL)
		PANIC ("FAT close failed");
	memcpy (bounce, &fat_fs->bs, sizeof (fat_fs->bs));
	disk_write (filesys_disk, FAT_BOOT_SECTOR, bounce);
	free (bounce);

	// Write FAT directly to the disk
	uint8_t *buffer = (uint8_t *) fat_fs->fat;
	off_t bytes_wrote = 0;
	off_t bytes_left = sizeof (fat_fs->fat);
	const off_t fat_size_in_bytes = fat_fs->fat_length * sizeof (cluster_t);
	for (unsigned i = 0; i < fat_fs->bs.fat_sectors; i++) {
		bytes_left = fat_size_in_bytes - bytes_wrote;
		if (bytes_left >= DISK_SECTOR_SIZE) {
			disk_write (filesys_disk, fat_fs->bs.fat_start + i,
			            buffer + bytes_wrote);
			bytes_wrote += DISK_SECTOR_SIZE;
		} else {
			bounce = calloc (1, DISK_SECTOR_SIZE);
			if (bounce == NULL)
				PANIC ("FAT close failed");
			memcpy (bounce, buffer + bytes_wrote, bytes_left);
			disk_write (filesys_disk, fat_fs->bs.fat_start + i, bounce);
			bytes_wrote += bytes_left;
			free (bounce);
		}
	}
}

void
fat_create (void) {
	// Create FAT boot
	fat_boot_create ();
	fat_fs_init ();

	// Create FAT table
	fat_fs->fat = calloc (fat_fs->fat_length, sizeof (cluster_t));
	if (fat_fs->fat == NULL)
		PANIC ("FAT creation failed");

	// Set up ROOT_DIR_CLST
	fat_put (ROOT_DIR_CLUSTER, EOChain);

	// Fill up ROOT_DIR_CLUSTER region with 0
	uint8_t *buf = calloc (1, DISK_SECTOR_SIZE);
	if (buf == NULL)
		PANIC ("FAT create failed due to OOM");
	disk_write (filesys_disk, cluster_to_sector (ROOT_DIR_CLUSTER), buf);
	free (buf);
}

void
fat_boot_create (void) {
	unsigned int fat_sectors =
	    (disk_size (filesys_disk) - 1)
	    / (DISK_SECTOR_SIZE / sizeof (cluster_t) * SECTORS_PER_CLUSTER + 1) + 1;
	fat_fs->bs = (struct fat_boot){
	    .magic = FAT_MAGIC,
	    .sectors_per_cluster = SECTORS_PER_CLUSTER,
	    .total_sectors = disk_size (filesys_disk),
	    .fat_start = 1,
	    .fat_sectors = fat_sectors,
	    .root_dir_cluster = ROOT_DIR_CLUSTER,
	};
}

void
fat_fs_init (void) {
	ASSERT (fat_fs != NULL);
	ASSERT (fat_fs->bs.sectors_per_cluster == SECTORS_PER_CLUSTER);

	fat_fs->data_start = fat_fs->bs.fat_start + fat_fs->bs.fat_sectors;

	/* Cluster numbers are 1-based (0 is reserved).
	 * fat_length includes the unused entry 0.
	 *
	 * Usable clusters: [ROOT_DIR_CLUSTER, fat_length).
	 * ROOT_DIR_CLUSTER itself is reserved for the root directory.
	 */
	const unsigned int data_sectors =
	    fat_fs->bs.total_sectors > fat_fs->data_start
	        ? fat_fs->bs.total_sectors - fat_fs->data_start
	        : 0;
	fat_fs->fat_length = data_sectors / fat_fs->bs.sectors_per_cluster + 1;
	fat_fs->last_clst = fat_fs->bs.root_dir_cluster;
	lock_init (&fat_fs->write_lock);
}

/*----------------------------------------------------------------------------*/
/* FAT handling                                                               */
/*----------------------------------------------------------------------------*/

/* Add a cluster to the chain.
 * If CLST is 0, start a new chain.
 * Returns 0 if fails to allocate a new cluster. */
cluster_t
fat_create_chain (cluster_t clst) {
	ASSERT (fat_fs != NULL);
	ASSERT (fat_fs->fat != NULL);

	lock_acquire (&fat_fs->write_lock);

	cluster_t new_clst = 0;
	cluster_t start = fat_fs->last_clst + 1;
	if (start < ROOT_DIR_CLUSTER + 1)
		start = ROOT_DIR_CLUSTER + 1;

	for (cluster_t i = start; i < fat_fs->fat_length; i++)
		if (fat_fs->fat[i] == 0) {
			new_clst = i;
			break;
		}
	if (new_clst == 0)
		for (cluster_t i = ROOT_DIR_CLUSTER + 1; i < start && i < fat_fs->fat_length; i++)
			if (fat_fs->fat[i] == 0) {
				new_clst = i;
				break;
			}

	if (new_clst != 0) {
		static char zeros[DISK_SECTOR_SIZE];

		/* Mark allocated and terminate chain. */
		fat_fs->fat[new_clst] = EOChain;
		if (clst != 0) {
			ASSERT (clst < fat_fs->fat_length);
			fat_fs->fat[clst] = new_clst;
		}
		fat_fs->last_clst = new_clst;
		disk_write (filesys_disk, cluster_to_sector (new_clst), zeros);
	}

	lock_release (&fat_fs->write_lock);
	return new_clst;
}

/* Remove the chain of clusters starting from CLST.
 * If PCLST is 0, assume CLST as the start of the chain. */
void
fat_remove_chain (cluster_t clst, cluster_t pclst) {
	ASSERT (fat_fs != NULL);
	ASSERT (fat_fs->fat != NULL);

	if (clst == 0)
		return;

	lock_acquire (&fat_fs->write_lock);

	if (pclst != 0) {
		ASSERT (pclst < fat_fs->fat_length);
		fat_fs->fat[pclst] = EOChain;
	}

	cluster_t cur = clst;
	while (cur != 0 && cur != EOChain) {
		ASSERT (cur < fat_fs->fat_length);
		cluster_t next = fat_fs->fat[cur];
		fat_fs->fat[cur] = 0;
		cur = next;
	}

	lock_release (&fat_fs->write_lock);
}

/* Update a value in the FAT table. */
void
fat_put (cluster_t clst, cluster_t val) {
	ASSERT (fat_fs != NULL);
	ASSERT (fat_fs->fat != NULL);
	ASSERT (clst > 0);
	ASSERT (clst < fat_fs->fat_length);
	fat_fs->fat[clst] = val;
}

/* Fetch a value in the FAT table. */
cluster_t
fat_get (cluster_t clst) {
	ASSERT (fat_fs != NULL);
	ASSERT (fat_fs->fat != NULL);
	ASSERT (clst > 0);
	ASSERT (clst < fat_fs->fat_length);
	return fat_fs->fat[clst];
}

/* Covert a cluster # to a sector number. */
disk_sector_t
cluster_to_sector (cluster_t clst) {
	ASSERT (fat_fs != NULL);
	ASSERT (clst >= ROOT_DIR_CLUSTER);
	ASSERT (clst < fat_fs->fat_length);
	return fat_fs->data_start + (clst - ROOT_DIR_CLUSTER) * fat_fs->bs.sectors_per_cluster;
}
