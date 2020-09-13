#include "filesys/cache.h"
#include "filesys/filesys.h"
#include <debug.h>
#include <string.h>

#include <stdio.h>
#include "threads/thread.h"

/* Buffer cache size. */
#define CACHE_SIZE 64

extern struct disk *filesys_disk;

static struct bce buffer_cache[CACHE_SIZE];
static struct lock bc_lock;

static int buffer_cache_find (disk_sector_t);
static int buffer_cache_alloc (void);

void
buffer_cache_init (void)
{
  int i;
  for (i = 0; i < CACHE_SIZE; i++)
    {
      buffer_cache[i].empty = true;
      lock_init (&buffer_cache[i].lock);
    }

  lock_init (&bc_lock);
}

void
buffer_cache_done (void)
{
  int i;
  for (i = 0; i < CACHE_SIZE; i++)
    if (buffer_cache[i].empty == false
        && buffer_cache[i].dirty == true)
      {
        lock_acquire (&buffer_cache[i].lock);
        disk_write (filesys_disk, buffer_cache[i].sec_no,
                    buffer_cache[i].buffer);
        lock_release (&buffer_cache[i].lock);
      }
}

/* Fetch the disk sector SEC_NO into the buffer cache. */
void
buffer_cache_disk_fetch (disk_sector_t sec_no)
{
  int idx = buffer_cache_find (sec_no);
  if (idx == -1)
    {
      /* Cache miss. */
      idx = buffer_cache_alloc ();
      buffer_cache[idx].empty = false;
      buffer_cache[idx].sec_no = sec_no;
      buffer_cache[idx].dirty = false;
      lock_release (&bc_lock);

      /* Disk --> buffer cache. */
      disk_read (filesys_disk, sec_no, buffer_cache[idx].buffer);
    }
  lock_release (&buffer_cache[idx].lock);
}


/* Disk --> memory. */
void
buffer_cache_disk_read (disk_sector_t sec_no, void *buffer)
{
  int idx = buffer_cache_find (sec_no);
  if (idx == -1)
    {
      /* Cache miss. */
      idx = buffer_cache_alloc ();
      buffer_cache[idx].empty = false;
      buffer_cache[idx].sec_no = sec_no;
      buffer_cache[idx].dirty = false;
      lock_release (&bc_lock);

      /* Disk --> buffer cache. */
      disk_read (filesys_disk, sec_no, buffer_cache[idx].buffer);
    }

  /* Buffer cache --> memory. */
  memcpy (buffer, buffer_cache[idx].buffer, DISK_SECTOR_SIZE);

  buffer_cache[idx].accessed = true;

  lock_release (&buffer_cache[idx].lock);
}

/* Memory --> buffer cache. */
void
buffer_cache_disk_write (disk_sector_t sec_no, const void *buffer)
{
  int idx = buffer_cache_find (sec_no);
  if (idx == -1)
    {
      /* Cache miss. */
      idx = buffer_cache_alloc ();
      buffer_cache[idx].empty = false;
      buffer_cache[idx].sec_no = sec_no;
      lock_release (&bc_lock);
    }

  /* Memory --> buffer cache. */
  memcpy (buffer_cache[idx].buffer, buffer, DISK_SECTOR_SIZE);

  buffer_cache[idx].accessed = true;
  buffer_cache[idx].dirty = true;

  lock_release (&buffer_cache[idx].lock);
}



/* ===== Helpers ===== */
static int
buffer_cache_find (disk_sector_t sec_no)
{
  /* Prevent change of whole buffer cache during finding. */
  lock_acquire (&bc_lock);

  int i;
  for (i = 0; i < CACHE_SIZE; i++)
    if (buffer_cache[i].empty == false
        && buffer_cache[i].sec_no == sec_no)
      {
        /* Cache hit. Allow change of whole buffer cache and
           prevent change of the block found. */
        lock_acquire (&buffer_cache[i].lock);
        lock_release (&bc_lock);
        return i;
      }
  return -1;
}

static int
buffer_cache_alloc (void)
{
  ASSERT (lock_held_by_current_thread (&bc_lock));

  int i;
  for (i = 0; i < CACHE_SIZE; i++)
    {
      if (buffer_cache[i].empty == true)
        {
          /* Empty cache; return immediately. */
          lock_acquire (&buffer_cache[i].lock);
          return i;
        }
    }

  /* All cache slots are occupied. Select a victim using
     clock algorithm. */
  static int s = 0;
  while (true)
    {
      if (buffer_cache[s].accessed == true)
        buffer_cache[s].accessed = false;
      else
        break;

      s++;
      s %= CACHE_SIZE;
    }

  lock_acquire (&buffer_cache[s].lock);

  /* The victim is selected; evict it. */
  if (buffer_cache[s].dirty == true)
    disk_write (filesys_disk, buffer_cache[s].sec_no,
                buffer_cache[s].buffer);
  buffer_cache[s].empty = true;

  return s;
}
