#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/fat.h"
#include "devices/disk.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* The disk that contains the file system. */
struct disk *filesys_disk;

static void do_format (void);

#define SYMLINK_MAX_DEPTH 16

static int
get_next_part (char part[NAME_MAX + 1], const char **srcp) {
	const char *src = *srcp;
	int len = 0;

	while (*src == '/')
		src++;
	if (*src == '\0')
		return 0;

	while (*src != '/' && *src != '\0') {
		if (len >= NAME_MAX)
			return -1;
		part[len++] = *src++;
	}
	part[len] = '\0';
	*srcp = src;
	return 1;
}

static struct dir *
get_start_dir (struct dir *base, const char *path) {
	if (path != NULL && path[0] == '/')
		return dir_open_root ();
	if (base != NULL)
		return dir_reopen (base);
	struct thread *t = thread_current ();
	if (t != NULL && t->cwd != NULL)
		return dir_reopen (t->cwd);
	return dir_open_root ();
}

static struct inode *
resolve_path_inode_impl (struct dir *base, const char *path, bool follow_final,
		int depth) {
	if (path == NULL)
		return NULL;
	if (depth > SYMLINK_MAX_DEPTH)
		return NULL;

	struct dir *dir = get_start_dir (base, path);
	if (dir == NULL)
		return NULL;

	const char *p = path;
	char part[NAME_MAX + 1];
	int status = get_next_part (part, &p);
	if (status == -1) {
		dir_close (dir);
		return NULL;
	}
	if (status == 0) {
		struct inode *inode = inode_reopen (dir_get_inode (dir));
		dir_close (dir);
		return inode;
	}

	for (;;) {
		const char *q = p;
		while (*q == '/')
			q++;
		bool last = *q == '\0';

		struct inode *inode = NULL;
		if (!dir_lookup (dir, part, &inode)) {
			dir_close (dir);
			return NULL;
		}

		if (inode_is_symlink (inode) && (!last || follow_final)) {
			off_t len = inode_length (inode);
			if (len <= 0 || len > 4096) {
				inode_close (inode);
				dir_close (dir);
				return NULL;
			}
			char *target = malloc ((size_t) len + 1);
			if (target == NULL) {
				inode_close (inode);
				dir_close (dir);
				return NULL;
			}
			off_t n = inode_read_at (inode, target, len, 0);
			inode_close (inode);
			if (n != len) {
				free (target);
				dir_close (dir);
				return NULL;
			}
			target[len] = '\0';

			inode = resolve_path_inode_impl (dir, target, true, depth + 1);
			free (target);
			if (inode == NULL) {
				dir_close (dir);
				return NULL;
			}

			if (last) {
				dir_close (dir);
				return inode;
			}
		}

		if (last) {
			dir_close (dir);
			return inode;
		}

		if (!inode_is_dir (inode)) {
			inode_close (inode);
			dir_close (dir);
			return NULL;
		}

		struct dir *next = dir_open (inode);
		dir_close (dir);
		dir = next;
		if (dir == NULL)
			return NULL;

		status = get_next_part (part, &p);
		if (status == -1) {
			dir_close (dir);
			return NULL;
		}
		if (status == 0) {
			struct inode *res = inode_reopen (dir_get_inode (dir));
			dir_close (dir);
			return res;
		}
	}
}

static struct inode *
resolve_path_inode (const char *path, bool follow_final) {
	return resolve_path_inode_impl (NULL, path, follow_final, 0);
}

static struct dir *
resolve_parent_dir (const char *path, char file_name[NAME_MAX + 1]) {
	file_name[0] = '\0';
	if (path == NULL)
		return NULL;

	struct dir *dir = get_start_dir (NULL, path);
	if (dir == NULL)
		return NULL;

	const char *p = path;
	char part[NAME_MAX + 1];
	int status = get_next_part (part, &p);
	if (status == -1) {
		dir_close (dir);
		return NULL;
	}
	if (status == 0)
		return dir;

	for (;;) {
		const char *q = p;
		while (*q == '/')
			q++;
		bool last = *q == '\0';
		if (last) {
			strlcpy (file_name, part, NAME_MAX + 1);
			return dir;
		}

		struct inode *inode = NULL;
		if (!dir_lookup (dir, part, &inode)) {
			dir_close (dir);
			return NULL;
		}

		if (inode_is_symlink (inode)) {
			off_t len = inode_length (inode);
			if (len <= 0 || len > 4096) {
				inode_close (inode);
				dir_close (dir);
				return NULL;
			}
			char *target = malloc ((size_t) len + 1);
			if (target == NULL) {
				inode_close (inode);
				dir_close (dir);
				return NULL;
			}
			off_t n = inode_read_at (inode, target, len, 0);
			inode_close (inode);
			if (n != len) {
				free (target);
				dir_close (dir);
				return NULL;
			}
			target[len] = '\0';
			inode = resolve_path_inode_impl (dir, target, true, 1);
			free (target);
			if (inode == NULL) {
				dir_close (dir);
				return NULL;
			}
		}

		if (!inode_is_dir (inode)) {
			inode_close (inode);
			dir_close (dir);
			return NULL;
		}
		struct dir *next = dir_open (inode);
		dir_close (dir);
		dir = next;
		if (dir == NULL)
			return NULL;

		status = get_next_part (part, &p);
		if (status == -1) {
			dir_close (dir);
			return NULL;
		}
		if (status == 0)
			return dir;
	}
}

/* Initializes the file system module.
 * If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) {
	filesys_disk = disk_get (0, 1);
	if (filesys_disk == NULL)
		PANIC ("hd0:1 (hdb) not present, file system initialization failed");

	inode_init ();

#ifdef EFILESYS
	fat_init ();

	if (format)
		do_format ();

	fat_open ();
#else
	/* Original FS */
	free_map_init ();

	if (format)
		do_format ();

	free_map_open ();
#endif
}

/* Shuts down the file system module, writing any unwritten data
 * to disk. */
void
filesys_done (void) {
	/* Original FS */
#ifdef EFILESYS
	fat_close ();
#else
	free_map_close ();
#endif
}

/* Creates a file named NAME with the given INITIAL_SIZE.
 * Returns true if successful, false otherwise.
 * Fails if a file named NAME already exists,
 * or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) {
	disk_sector_t inode_sector = 0;
	char file_name[NAME_MAX + 1];
	struct dir *dir = resolve_parent_dir (name, file_name);
	if (dir == NULL)
		return false;

	if (file_name[0] == '\0' || !strcmp (file_name, ".")
			|| !strcmp (file_name, "..")) {
		dir_close (dir);
		return false;
	}

	struct inode *tmp = NULL;
	if (dir_lookup (dir, file_name, &tmp)) {
		inode_close (tmp);
		dir_close (dir);
		return false;
	}

	bool success = false;

#ifdef EFILESYS
	inode_sector = fat_create_chain (0);
	if (inode_sector != 0
			&& inode_create (inode_sector, initial_size)
			&& dir_add (dir, file_name, inode_sector))
		success = true;
	else if (inode_sector != 0) {
		struct inode *inode = inode_open (inode_sector);
		if (inode != NULL) {
			inode_remove (inode);
			inode_close (inode);
		} else
			fat_remove_chain (inode_sector, 0);
	}
#else
	success = (free_map_allocate (1, &inode_sector)
			&& inode_create (inode_sector, initial_size)
			&& dir_add (dir, file_name, inode_sector));
	if (!success && inode_sector != 0)
		free_map_release (inode_sector, 1);
#endif

	dir_close (dir);
	return success;
}

/* Opens the file with the given NAME.
 * Returns the new file if successful or a null pointer
 * otherwise.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name) {
	if (name == NULL || name[0] == '\0')
		return NULL;
	struct inode *inode = resolve_path_inode (name, true);
	return file_open (inode);
}

/* Deletes the file named NAME.
 * Returns true if successful, false on failure.
 * Fails if no file named NAME exists,
 * or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) {
	char file_name[NAME_MAX + 1];
	struct dir *dir = resolve_parent_dir (name, file_name);
	if (dir == NULL)
		return false;
	if (file_name[0] == '\0') {
		dir_close (dir);
		return false;
	}
	bool success = dir_remove (dir, file_name);
	dir_close (dir);
	return success;
}

bool
filesys_chdir (const char *path) {
	struct inode *inode = resolve_path_inode (path, true);
	if (inode == NULL)
		return false;
	if (!inode_is_dir (inode)) {
		inode_close (inode);
		return false;
	}
	struct dir *dir = dir_open (inode);
	if (dir == NULL)
		return false;
	struct thread *t = thread_current ();
	struct dir *old = t->cwd;
	t->cwd = dir;
	dir_close (old);
	return true;
}

bool
filesys_mkdir (const char *path) {
	disk_sector_t inode_sector = 0;
	char dir_name[NAME_MAX + 1];
	struct dir *parent = resolve_parent_dir (path, dir_name);
	if (parent == NULL)
		return false;
	if (dir_name[0] == '\0' || !strcmp (dir_name, ".")
			|| !strcmp (dir_name, "..")) {
		dir_close (parent);
		return false;
	}

	struct inode *tmp = NULL;
	if (dir_lookup (parent, dir_name, &tmp)) {
		inode_close (tmp);
		dir_close (parent);
		return false;
	}

	bool success = false;
#ifdef EFILESYS
	inode_sector = fat_create_chain (0);
	if (inode_sector != 0
			&& dir_create (inode_sector,
					inode_get_inumber (dir_get_inode (parent)), 16)
			&& dir_add (parent, dir_name, inode_sector))
		success = true;
	else if (inode_sector != 0) {
		struct inode *inode = inode_open (inode_sector);
		if (inode != NULL) {
			inode_remove (inode);
			inode_close (inode);
		} else
			fat_remove_chain (inode_sector, 0);
	}
#else
	success = (free_map_allocate (1, &inode_sector)
			&& dir_create (inode_sector,
					inode_get_inumber (dir_get_inode (parent)), 16)
			&& dir_add (parent, dir_name, inode_sector));
	if (!success && inode_sector != 0)
		free_map_release (inode_sector, 1);
#endif

	dir_close (parent);
	return success;
}

int
filesys_symlink (const char *target, const char *linkpath) {
	disk_sector_t inode_sector = 0;
	char link_name[NAME_MAX + 1];
	struct dir *parent = resolve_parent_dir (linkpath, link_name);
	if (parent == NULL)
		return -1;
	if (link_name[0] == '\0' || !strcmp (link_name, ".")
			|| !strcmp (link_name, "..")) {
		dir_close (parent);
		return -1;
	}

	struct inode *tmp = NULL;
	if (dir_lookup (parent, link_name, &tmp)) {
		inode_close (tmp);
		dir_close (parent);
		return -1;
	}

	bool success = false;
	off_t len = (off_t) strlen (target) + 1;

#ifdef EFILESYS
	inode_sector = fat_create_chain (0);
	if (inode_sector != 0
			&& inode_create_symlink (inode_sector, len)) {
		struct inode *inode = inode_open (inode_sector);
		if (inode != NULL) {
			inode_write_at (inode, target, len, 0);
			inode_close (inode);
			if (dir_add (parent, link_name, inode_sector))
				success = true;
		}
	}
	if (!success && inode_sector != 0) {
		struct inode *inode = inode_open (inode_sector);
		if (inode != NULL) {
			inode_remove (inode);
			inode_close (inode);
		} else
			fat_remove_chain (inode_sector, 0);
	}
#else
	if (free_map_allocate (1, &inode_sector)
			&& inode_create_symlink (inode_sector, len)) {
		struct inode *inode = inode_open (inode_sector);
		if (inode != NULL) {
			inode_write_at (inode, target, len, 0);
			inode_close (inode);
			if (dir_add (parent, link_name, inode_sector))
				success = true;
		}
	}
	if (!success && inode_sector != 0)
		free_map_release (inode_sector, 1);
#endif

	dir_close (parent);
	return success ? 0 : -1;
}

/* Formats the file system. */
static void
do_format (void) {
	printf ("Formatting file system...");

#ifdef EFILESYS
	/* Create FAT and save it to the disk. */
	fat_create ();
	if (!dir_create (ROOT_DIR_SECTOR, ROOT_DIR_SECTOR, 16))
		PANIC ("root directory creation failed");
	fat_close ();
#else
	free_map_create ();
	if (!dir_create (ROOT_DIR_SECTOR, ROOT_DIR_SECTOR, 16))
		PANIC ("root directory creation failed");
	free_map_close ();
#endif

	printf ("done.\n");
}
