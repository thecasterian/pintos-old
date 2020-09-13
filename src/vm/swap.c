#include "vm/swap.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "devices/disk.h"
#include <string.h>
#include <hash.h>
#include <bitmap.h>
#include <stdio.h>

/* The number of sector required for one page */
#define SEC_PG (PGSIZE / DISK_SECTOR_SIZE)

/* Swap disk. It can btained by using disk_get (1, 1). */
static struct disk *swap_disk;

/* Swap table. It manages which sectors are allocated to evicted
   pages. For example, if index 2 of swap table is true then some
   page can be swapped to sector 16 - 23 of the swap disk. (when
   page size is 8 times sector size) */
static struct bitmap *swap_table;

static size_t alloc_pg_idx (void);

static void to_swap_disk (struct fte *);
static void to_file (struct fte *);

static void from_swap_disk (struct spte *);
static void from_file (struct spte *);

/* Create and initialize the swap table. */
void
swap_table_init (void)
{
  swap_disk = disk_get (1, 1);
	if (swap_disk == NULL)
    PANIC ("Can't load swap disk");

	swap_table = bitmap_create (disk_size (swap_disk) / SEC_PG);
	bitmap_set_all (swap_table, true);
}

/* Set IDX of the swap table to AVAILABLE, */
void
swap_table_set_available (size_t idx, bool available)
{
  bitmap_set (swap_table, idx, available);
}

void
swap_out (struct fte *victim)
{
  ASSERT (victim != NULL);

  struct thread *t = victim->process;

  pagedir_clear_page (t->pagedir, victim->upage);

  struct spte *p = suppl_page_table_find (t->suppl_page_table, victim->upage);

  if (p->file == NULL)
    to_swap_disk (victim);
  else
    to_file (victim);
}

void
swap_in (struct spte *p)
{
  ASSERT (p != NULL);

  if (p->file == NULL)
    from_swap_disk (p);
  else
    from_file (p);
}



/* ===== Helpers ===== */

/* Allocate number of disk sector which is empty */
static size_t
alloc_pg_idx (void)
{
  return bitmap_scan (swap_table, 0, 1, true);
}

static void
to_swap_disk (struct fte *victim)
{
  pgl_acquire ();

  /* Allocate new swap slot. */
  size_t pg_idx = alloc_pg_idx ();

	if (pg_idx == BITMAP_ERROR)
    PANIC ("Swap disk capacity insufficient");

  struct hash *spt = victim->process->suppl_page_table;
  struct spte *p = suppl_page_table_find (spt, victim->upage);

  if (p != NULL)
    p->pg_idx = pg_idx;

	/* Swap out victim from memory to the swap disk. */
	size_t sec_no = pg_idx * SEC_PG;
	int i;

	pgl_release ();
	for (i = 0; i < PGSIZE / DISK_SECTOR_SIZE; i++)
    disk_write (swap_disk, sec_no + i,
                (uint8_t *)victim->kpage + DISK_SECTOR_SIZE*i);
  pgl_acquire ();

  /* Discard victim's kpage from the frame table */
	frame_free (victim->kpage);

  /* Mark that pg_idx is occupied (not available) in the swap disk. */
	swap_table_set_available (pg_idx, false);

	pgl_release ();
}

static void
to_file (struct fte *victim)
{
  pgl_acquire ();

  uint32_t *pd = victim->process->pagedir;

	if (pagedir_is_dirty (pd, victim->upage))
    {
      struct hash *spt = victim->process->suppl_page_table;
      struct spte *p = suppl_page_table_find (spt, victim->upage);

      pgl_release ();
      if (file_write_at (p->file, p->kpage, p->page_read_bytes, p->ofs)
          != (int32_t) p->page_read_bytes)
        PANIC ("Cannot swap out to file");
      file_seek (p->file, p->ofs);
      pgl_acquire ();
    }

  frame_free (victim->kpage);

  pgl_release ();
}

static void
from_swap_disk (struct spte *p)
{
  pgl_acquire ();

  /* Allocate new frame to load evicted page from the swap disk to
     memory. */
  void *kpage = frame_alloc (p->upage, PAL_USER, true);

  /* Swap in the page from the swap disk to memory */
  size_t sec_no = p->pg_idx * SEC_PG;
  int i;

  pgl_release ();
  for (i = 0; i < PGSIZE / DISK_SECTOR_SIZE; i++)
    disk_read (swap_disk, sec_no + i,
               (uint8_t *)kpage + DISK_SECTOR_SIZE*i);
  pgl_acquire ();

  swap_table_set_available (p->pg_idx, true);

  pgl_release ();
}

static void
from_file (struct spte *p)
{
  pgl_acquire ();

  void *kpage = frame_alloc (p->upage, PAL_USER | PAL_ZERO, p->writable);

  ASSERT (p->page_read_bytes + p->page_zero_bytes == PGSIZE);

  pgl_release ();
  if (file_read_at (p->file, kpage, p->page_read_bytes, p->ofs)
      != (int32_t) p->page_read_bytes)
    PANIC ("Cannot load from file");
  file_seek (p->file, p->ofs);
  pgl_acquire ();

  memset (kpage + p->page_read_bytes, 0, p->page_zero_bytes);

  if (p->writable == true && p->mapped == false)
    /* This page will be evicted to swap disk from now. */
    p->file = NULL;

  pgl_release ();
}
