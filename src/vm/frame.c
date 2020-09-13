#include "vm/frame.h"
#include "devices/timer.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/swap.h"
#include <stdio.h>

static struct hash frame_table;
static struct list victim_selector;

/* Lock used for paging system */
static struct lock paging_lock;
static int pgl_nested = 0;

static bool frame_table_insert (struct fte *);
static void frame_table_del_frame (struct fte *);

static unsigned fte_hash (const struct hash_elem *, void *);
static bool fte_cmp_kpage (const struct hash_elem *,
                           const struct hash_elem *, void *);
static struct fte *select_victim (void);

/* Initialize the frame table. Called by init.c */
void
frame_table_init (void)
{
  hash_init (&frame_table, fte_hash, fte_cmp_kpage, NULL);
  list_init (&victim_selector);
  lock_init (&paging_lock);
}

/* Insert fte to the frame table */
static bool
frame_table_insert (struct fte *f)
{
  ASSERT (f != NULL);

  bool success = (hash_insert (&frame_table, &f->elem) == NULL);
  list_push_back (&victim_selector, &f->lelem);

  return success;
}

/* Find fte from address. If fte does not exist, return NULL. */
struct fte *
frame_table_find (void *kpage)
{
  pgl_acquire ();

  struct fte f;
  f.kpage = kpage;

  struct hash_elem *e = hash_find (&frame_table, &f.elem);

  if (e != NULL)
    {
      struct fte *f_found = hash_entry (e, struct fte, elem);

      pgl_release ();
      return f_found;
    }

  pgl_release ();
  return NULL;
}

/* Delete fte from the frame table */
static void
frame_table_del_frame (struct fte *f)
{
  ASSERT (f != NULL);

  hash_delete (&frame_table, &f->elem);
  list_remove (&f->lelem);
}

/* Allocate a new frame */
void *
frame_alloc (void *upage, enum palloc_flags flags, bool writable)
{
  ASSERT (upage != NULL);

  pgl_acquire ();

  struct thread *t = thread_current ();

  /* First attempt to allocate new frame. */
  void *kpage = palloc_get_page (flags);
  if (kpage  == NULL)
    {
      /* Physical memory insufficient. Eviction is required. */
      /* Select victim which is to be evicted. */
      struct fte *victim = select_victim ();

      swap_out (victim);

      /* Retry frame allocation once more */
      kpage = palloc_get_page (flags);
    }

  /* If retry is also failed, eviction performed incorrectly. */
  if (kpage == NULL)
    PANIC ("Eviction failed");

  /* upage - kpage mapping on pagedir */
  bool success = (pagedir_get_page (t->pagedir, upage) == NULL)
                 && pagedir_set_page (t->pagedir, upage, kpage, writable);
  if (!success)
    goto update_error;

  /* No eviction. Allocate new frame table entry (fte) */
  struct fte *fte_new = malloc (sizeof (struct fte));
  ASSERT (fte_new != NULL);

  /* Initialize fte */
  fte_new->kpage = kpage;
  fte_new->upage = upage;
  fte_new->process = t;

  /* Insert fte to the frame table */
  success = frame_table_insert (fte_new);
  if (!success)
    {
      free (fte_new);
      goto update_error;
    }

  /* Update supplemental page table. */
  struct spte *p = suppl_page_table_find (t->suppl_page_table, upage);
  if (p == NULL)
    {
      /* Current upage is not in supplemental page table. I.e., frame
         allocation for this UPAGE is first time. (If this UPAGE have
         been evicted, then frame is reallocated to swap in.)
         Allocate new spte. */
      p = malloc (sizeof (struct spte));
      ASSERT (p != NULL);

      p->kpage = kpage;
      p->upage = upage;
      p->file = NULL;

      success = suppl_page_table_insert (t->suppl_page_table, p);

      if (!success)
        {
          free (fte_new);
          free (p);
          goto update_error;
        }
    }
  else
    /* This page is swapped in. Just update kpage only. */
    p->kpage = kpage;

  pgl_release ();

  return kpage;

 update_error:
  palloc_free_page (kpage);
  pgl_release ();

  PANIC ("Error occured while updating "
         "page directory, frame table, or supplimental page table.");
  return NULL;
}

/* Free KPAGE */
void
frame_free (void *kpage)
{
  pgl_acquire ();

  struct fte *f = frame_table_find (kpage);
  ASSERT (f != NULL);

  struct thread *t = f->process;
  void *upage = f->upage;

  /* Discard fte from the frame table */
  frame_table_del_frame (f);
  free (f);

  suppl_page_table_set_evicted (t->suppl_page_table, upage);

  /* Free KPAGE */
  palloc_free_page (kpage);

  /* Discard upage - kpage mapping from pagedir */
  pagedir_clear_page (t->pagedir, upage);

  pgl_release ();
}

/* Select fte to be evicted */
static struct fte *
select_victim (void)
{
  return list_entry (list_begin (&victim_selector), struct fte, lelem);
}

/* ===== Helpers ===== */

/* Hash function for fte. */
static unsigned
fte_hash (const struct hash_elem *e, void *aux UNUSED)
{
  ASSERT (e != NULL);

  const struct fte *f = hash_entry (e, struct fte, elem);
  return hash_bytes (&f->kpage, sizeof f->kpage);
}

/* Compare fte's addresses. */
static bool
fte_cmp_kpage (const struct hash_elem *a_,
               const struct hash_elem *b_, void *aux UNUSED)
{
  const struct fte *a = hash_entry (a_, struct fte, elem);
  const struct fte *b = hash_entry (b_, struct fte, elem);

  return a->kpage < b->kpage;
}

void
pgl_acquire (void)
{
  if (!lock_held_by_current_thread (&paging_lock))
    lock_acquire (&paging_lock);
  pgl_nested++;
}

void
pgl_release (void)
{
  pgl_nested--;
  if (pgl_nested == 0)
    lock_release (&paging_lock);
}
