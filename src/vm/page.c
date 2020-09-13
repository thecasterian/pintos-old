#include "vm/page.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include <stdio.h>

static unsigned spte_hash (const struct hash_elem *, void *);
static bool spte_cmp_upage (const struct hash_elem *,
                            const struct hash_elem *, void *);
static void spt_clear_func (struct hash_elem *, void *);

/* Initialize the supplemental page table. Called by load() */
struct hash *
suppl_page_table_create (void)
{
  struct hash *spt = malloc (sizeof (struct hash));
  if (spt != NULL)
    hash_init (spt, spte_hash, spte_cmp_upage, NULL);

  return spt;
}

void
suppl_page_table_del (struct hash *spt)
{
  pgl_acquire ();

  hash_destroy (spt, spt_clear_func);
  free (spt);

  pgl_release ();
}

/* Insert spte to the supplemental page table */
bool
suppl_page_table_insert (struct hash *spt, struct spte *p)
{
  pgl_acquire ();
  bool success = (hash_insert (spt, &p->elem) == NULL);
  pgl_release ();

  return success;
}

/* Find spte from address. If not exist, return NULL */
struct spte *
suppl_page_table_find (struct hash *spt, void *upage)
{
  pgl_acquire ();

  struct spte p;
  p.upage = upage;

  struct hash_elem *e = hash_find (spt, &p.elem);

  struct spte *p_found;
  if (e != NULL)
    p_found = hash_entry (e, struct spte, elem);
  else
    p_found = NULL;

  pgl_release ();

  return p_found;
}

void
suppl_page_table_del_page (struct hash *spt, struct spte *p)
{
  pgl_acquire ();

  hash_delete (spt, &p->elem);
  free (p);

  pgl_release ();
}

/* Change status of the page */
void
suppl_page_table_set_evicted (struct hash *spt, void *upage)
{
  pgl_acquire ();

  struct spte *p = suppl_page_table_find (spt, upage);
  if (p != NULL)
    p->kpage = NULL;

  pgl_release ();
}



/* ===== Helpers ===== */

/* Hash function */
static unsigned
spte_hash (const struct hash_elem *e, void *aux UNUSED)
{
  const struct spte *p = hash_entry (e, struct spte, elem);
  return hash_bytes (&p->upage, sizeof p->upage);
}

/* Compare fte's addresses */
static bool
spte_cmp_upage (const struct hash_elem *a_,
                const struct hash_elem *b_, void *aux UNUSED)
{
  const struct spte *a = hash_entry (a_, struct spte, elem);
  const struct spte *b = hash_entry (b_, struct spte, elem);

  return a->upage < b->upage;
}

static void
spt_clear_func (struct hash_elem *e, void *aux UNUSED)
{
  struct spte *p = hash_entry (e, struct spte, elem);

  if (p->kpage != NULL)
    {
      struct fte *f = frame_table_find (p->kpage);
      if (f != NULL)
        {
          ASSERT (f->process == thread_current ());
          frame_free (f->kpage);
        }
      else
        PANIC ("Cannot find fte from frame table");
    }
  else
    {
      if (p->file == NULL)
        swap_table_set_available (p->pg_idx, true);
    }

  free (p);
}
