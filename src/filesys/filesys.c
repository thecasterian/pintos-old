#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "devices/disk.h"

#include "filesys/cache.h"
#include "threads/thread.h"

/* The disk that contains the file system. */
struct disk *filesys_disk;

/* Root directory. */
static struct dir *root_dir;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format)
{
  filesys_disk = disk_get (0, 1);
  if (filesys_disk == NULL)
    PANIC ("hd0:1 (hdb) not present, file system initialization failed");

  inode_init ();
  free_map_init ();
  buffer_cache_init ();

  if (format)
    do_format ();

  free_map_open ();

  root_dir = dir_open_root ();
  thread_current ()->curr_dir = dir_open_root ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void)
{
  free_map_close ();
  dir_close (root_dir);
  buffer_cache_done ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const struct dir *dir, const char *name, off_t initial_size)
{
  if (dir == NULL)
    dir = root_dir;

  disk_sector_t inode_sector = 0;
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, 0)
                  && dir_add ((struct dir *)dir, name, inode_sector));
  if (!success && inode_sector == 0)
    free_map_release (inode_sector, 1);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const struct dir *dir, const char *name)
{
  if (dir == NULL)
    dir = root_dir;

  struct inode *inode = NULL;

  if (dir != NULL)
    dir_lookup (dir, name, &inode);

  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const struct dir *dir, const char *name)
{
  if (dir == NULL)
    dir = root_dir;

  return (dir != NULL && dir_remove ((struct dir *)dir, name));
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();

  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");

  struct inode *root_inode = inode_open (ROOT_DIR_SECTOR);
  inode_set_parent_sector (root_inode, ROOT_DIR_SECTOR);
  inode_close (root_inode);

  free_map_close ();
  printf ("done.\n");
}
