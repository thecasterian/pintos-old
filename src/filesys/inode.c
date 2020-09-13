#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

#include "filesys/cache.h"
#include "threads/synch.h"

#include "threads/thread.h"
#include <stdio.h>

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* Number of direct blocks in inode. */
#define DIRECT_NO 64

/* On-disk inode.
   Must be exactly DISK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    disk_sector_t direct[DIRECT_NO];    /* Sector numbers of direct blocks. */
    disk_sector_t db_indirect;          /* Sector number of doubly indirect block. */
    off_t length;                       /* File size in bytes. */

    uint32_t is_dir;                    /* Is this inode for directory? */
    disk_sector_t parent_sector;        /* Sector number of parent inode (if it's for dir). */

    unsigned magic;                     /* Magic number. */
    uint32_t unused[123 - DIRECT_NO];   /* Not used. */
  };

/* On-disk inode for indirect blocks. */
struct inode_disk_iblock
  {
    disk_sector_t indirect[128];
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, DISK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode
  {
    struct list_elem elem;              /* Element in inode list. */
    disk_sector_t sector;               /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
    struct lock lock;                   /* Lock for file extension. */
    struct lock dir_lock;               /* Lock for directory operation. */
  };

/* Returns the disk sector that contains byte offset POS within
   INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static disk_sector_t
byte_to_sector (const struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);

  /* Sector index.
       0 .. DIRECT_NO - 1              ->  direct blocks
       DIRECT_NO .. DIRECT_NO + 16383  ->  doubly indirect blocks */
  int idx = pos / DISK_SECTOR_SIZE;

  const struct inode_disk *disk_inode = &inode->data;

  if (pos >= inode->data.length)
    return -1;

  if (idx < DIRECT_NO)
      /* Direct blocks. */
      return disk_inode->direct[idx];

    /* Doubly indirect block. */
    idx -= DIRECT_NO;
    int idx1 = idx / (DISK_SECTOR_SIZE / 4);
    int idx2 = idx % (DISK_SECTOR_SIZE / 4);

    struct inode_disk_iblock *iblock1
        = calloc (1, sizeof (struct inode_disk_iblock));
    buffer_cache_disk_read (disk_inode->db_indirect, iblock1);

    struct inode_disk_iblock *iblock2
        = calloc (1, sizeof (struct inode_disk_iblock));
    buffer_cache_disk_read (iblock1->indirect[idx1], iblock2);

    int sec_no = iblock2->indirect[idx2];

    free (iblock1);
    free (iblock2);
    return sec_no;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

static bool inode_create_direct (struct inode_disk *);
static bool inode_create_db_indirect (struct inode_disk *);
static void inode_close_direct (struct inode_disk *);
static void inode_close_db_indirect (struct inode_disk *);
static void inode_extend (struct inode *, off_t);
static void inode_extend_direct (struct inode_disk *, off_t);
static void inode_extend_db_indirect (struct inode_disk *, off_t);

/* Initializes the inode module. */
void
inode_init (void)
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   disk.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (disk_sector_t sector, off_t length, uint32_t is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == DISK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_dir = is_dir;
      disk_inode->parent_sector = -1u;

      success = (inode_create_direct (disk_inode)
                 && inode_create_db_indirect (disk_inode));

      buffer_cache_disk_write (sector, disk_inode);

      free (disk_inode);
    }

  return success;
}

static bool
inode_create_direct (struct inode_disk *disk_inode)
{
  int max_idx = (disk_inode->length - 1) / DISK_SECTOR_SIZE;
  max_idx = (max_idx >= DIRECT_NO) ? DIRECT_NO - 1 : max_idx;

  if (disk_inode->length == 0)
    /* No neead to allocate. */
    return true;

  static char zeros[DISK_SECTOR_SIZE];

  int i;
  for (i = 0; i <= max_idx; i++)
    {
      if (!free_map_allocate (1, &disk_inode->direct[i]))
        return false;
      buffer_cache_disk_write (disk_inode->direct[i], zeros);
    }

  return true;
}

static bool
inode_create_db_indirect (struct inode_disk *disk_inode)
{
  int max_idx = (disk_inode->length - 1) / DISK_SECTOR_SIZE - DIRECT_NO;
  if (max_idx < 0)
    /* No neead to allocate. */
    return true;

  struct inode_disk_iblock *iblock1
      = calloc (1, sizeof (struct inode_disk_iblock));
  struct inode_disk_iblock *iblock2
      = calloc (1, sizeof (struct inode_disk_iblock));

  static char zeros[DISK_SECTOR_SIZE];

  /* Allocate first indirect block sector. */
  free_map_allocate (1, &disk_inode->db_indirect);

  int i, idx1, idx2;
  for (i = 0; i <= max_idx; i++)
    {
      idx1 = i / 128;
      idx2 = i % 128;

      if (idx2 == 0)
        /* Allocate second indirect block sector. */
        if (!free_map_allocate (1, &iblock1->indirect[idx1]))
          return false;

      /* Allocate data sector. */
      if (!free_map_allocate (1, &iblock2->indirect[idx2]))
        return false;
      buffer_cache_disk_write (iblock2->indirect[idx2], zeros);

      if (idx2 == 127 || i == max_idx)
        /* Write iblock2 into disk. */
        buffer_cache_disk_write (iblock1->indirect[idx1], iblock2);
    }

  buffer_cache_disk_write (disk_inode->db_indirect, iblock1);

  free (iblock1);
  free (iblock2);

  return true;
}

static void
inode_extend (struct inode *inode, off_t new_length)
{
  ASSERT (inode->data.length < new_length);
  ASSERT (lock_held_by_current_thread (&inode->lock));

  inode_extend_direct (&inode->data, new_length);
  inode_extend_db_indirect (&inode->data, new_length);
  inode->data.length = new_length;

  buffer_cache_disk_write (inode->sector, &inode->data);
}

static void
inode_extend_direct (struct inode_disk *disk_inode, off_t new_length)
{
  int max_idx = (disk_inode->length - 1) / DISK_SECTOR_SIZE;
  int new_max_idx = (new_length - 1) / DISK_SECTOR_SIZE;

  if (max_idx >= DIRECT_NO)
    return;
  if (new_max_idx >= DIRECT_NO)
    new_max_idx = DIRECT_NO - 1;

  static char zeros[DISK_SECTOR_SIZE];

  if (disk_inode->length == 0)
    {
      free_map_allocate (1, &disk_inode->direct[0]);
      buffer_cache_disk_write (disk_inode->direct[0], zeros);
    }

  int i;
  for (i = max_idx + 1; i <= new_max_idx; i++)
    {
      free_map_allocate (1, &disk_inode->direct[i]);
      buffer_cache_disk_write (disk_inode->direct[i], zeros);
    }
}

static void
inode_extend_db_indirect (struct inode_disk *disk_inode, off_t new_length)
{
  int max_idx = (disk_inode->length - 1) / DISK_SECTOR_SIZE - DIRECT_NO;
  int new_max_idx = (new_length - 1) / DISK_SECTOR_SIZE - DIRECT_NO;

  if (new_max_idx < 0)
    return;

  else if (max_idx < 0)
    {
      disk_inode->length = new_length;
      inode_create_db_indirect (disk_inode);
      return;
    }

  struct inode_disk_iblock *iblock1
      = calloc (1, sizeof (struct inode_disk_iblock));
  struct inode_disk_iblock *iblock2
      = calloc (1, sizeof (struct inode_disk_iblock));

  static char zeros[DISK_SECTOR_SIZE];

  /* Load first indirect block sector. */
  buffer_cache_disk_read (disk_inode->db_indirect, iblock1);

  int i, idx1, idx2;
  for (i = max_idx + 1; i <= new_max_idx; i++)
    {
      idx1 = i / 128;
      idx2 = i % 128;

      if (i == max_idx + 1 && idx2 != 0)
        /* Load second indirect block sector. */
        buffer_cache_disk_read (iblock1->indirect[idx1], iblock2);

      if (idx2 == 0)
        /* Allocate second indirect block sector. */
        free_map_allocate (1, &iblock1->indirect[idx1]);

      /* Allocate data sector. */
      free_map_allocate (1, &iblock2->indirect[idx2]);
      buffer_cache_disk_write (iblock2->indirect[idx2], zeros);

      if (idx2 == 127 || i == new_max_idx)
        /* Write iblock2 into disk. */
        buffer_cache_disk_write (iblock1->indirect[idx1], iblock2);
    }

  buffer_cache_disk_write (disk_inode->db_indirect, iblock1);

  free (iblock1);
  free (iblock2);
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (disk_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector)
        {
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
  lock_init (&inode->lock);
  lock_init (&inode->dir_lock);
  buffer_cache_disk_read (inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
disk_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);

      /* Deallocate blocks if removed. */
      if (inode->removed)
        {
          /* Deallocate direct blocks. */
          inode_close_direct (&inode->data);

          /* Deallocate doubly indirect blocks. */
          inode_close_db_indirect (&inode->data);

          free_map_release (inode->sector, 1);
        }

      free (inode);
    }
}

static void
inode_close_direct (struct inode_disk *disk_inode)
{
  if (disk_inode->length == 0)
    return;

  int max_idx = (disk_inode->length - 1) / DISK_SECTOR_SIZE;
  max_idx = (max_idx >= DIRECT_NO) ? DIRECT_NO - 1 : max_idx;

  int i;
  for (i = 0; i <= max_idx; i++)
    free_map_release (disk_inode->direct[i], 1);
}

static void
inode_close_db_indirect (struct inode_disk *disk_inode)
{
  int max_idx = (disk_inode->length - 1) / DISK_SECTOR_SIZE - DIRECT_NO;
  if (max_idx < 0)
    return;

  struct inode_disk_iblock *iblock1
      = calloc (1, sizeof (struct inode_disk_iblock));
  struct inode_disk_iblock *iblock2
      = calloc (1, sizeof (struct inode_disk_iblock));

  /* Load first indirect block sector. */
  buffer_cache_disk_read (disk_inode->db_indirect, iblock1);

  int i, idx1, idx2;
  for (i = 0; i <= max_idx; i++)
    {
      idx1 = i / 128;
      idx2 = i % 128;

      if (idx2 == 0)
        /* Load second indirect block sector. */
        buffer_cache_disk_read (iblock1->indirect[idx1], iblock2);

      /* Free data sector. */
      free_map_release (iblock2->indirect[idx2], 1);

      if (idx2 == 127 || i == max_idx)
        /* Free second indirect block sector. */
        free_map_release (iblock1->indirect[idx1], 1);
    }

  /* Free first indirect block sector. */
  free_map_release (disk_inode->db_indirect, 1);

  free (iblock1);
  free (iblock2);
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0)
    {
      /* Acquire lock before calculate sector index because
         some process can perform file extension. */
      lock_acquire (&inode->lock);

      /* Disk sector to read, starting byte offset within sector. */
      disk_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % DISK_SECTOR_SIZE;

      /* Next sector is calculated for read-ahead. */
      disk_sector_t next_sector_idx = byte_to_sector (inode, offset + DISK_SECTOR_SIZE);

      /* Release lock. Disk writing can be done without lock.
         (PintOS manual requires this) */
      lock_release (&inode->lock);

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          buffer_cache_disk_read (sector_idx, buffer + bytes_read);
        }
      else
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (DISK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          buffer_cache_disk_read (sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }

      /* Read-ahead. */
      if (next_sector_idx != -1u)
        buffer_cache_disk_fetch (next_sector_idx);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;
  bool extended = false;

  if (inode->deny_write_cnt)
    return 0;

  off_t new_length = offset + size;
  if (new_length > inode->data.length)
    {
      /* Acquire lock before file extension so that several
         proecesses cannot extend file simultaneously. */
      lock_acquire (&inode->lock);

      inode_extend (inode, new_length);

      /* If file is extended, lock must be released after
         writing is done. */
      extended = true;
    }

  while (size > 0)
    {
      /* Acquire lock before calculate sector index because
         some process can perform file extension. */
      if (!extended)
        lock_acquire (&inode->lock);

      /* Sector to write, starting byte offset within sector. */
      disk_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % DISK_SECTOR_SIZE;

      /* Release lock. Disk writing can be done without lock.
         (PintOS manual requires this) */
      if (!extended)
        lock_release (&inode->lock);

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          buffer_cache_disk_write (sector_idx, buffer + bytes_written);
        }
      else
        {
          /* We need a bounce buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (DISK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left)
            buffer_cache_disk_read (sector_idx, bounce);
          else
            memset (bounce, 0, DISK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          buffer_cache_disk_write (sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  /* File extended & writing done. */
  if (extended)
    lock_release (&inode->lock);

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
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode)
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}

uint32_t
inode_is_dir (const struct inode *inode)
{
  return inode->data.is_dir;
}

disk_sector_t
inode_get_parent_sector (const struct inode *inode)
{
  return inode->data.parent_sector;
}

void
inode_set_parent_sector (struct inode *inode, disk_sector_t parent_sector)
{
  inode->data.parent_sector = parent_sector;
  buffer_cache_disk_write (inode->sector, &inode->data);
}

bool
inode_is_removed (struct inode *inode)
{
  return inode->removed;
}

void
inode_dir_lock_acquire (struct inode *inode)
{
  lock_acquire (&inode->dir_lock);
}

void
inode_dir_lock_release (struct inode *inode)
{
  lock_release (&inode->dir_lock);
}
