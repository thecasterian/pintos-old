#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/palloc.h"
#include "vm/page.h"
#include <hash.h>

/* Frame table entry */
struct fte
  {
    void *kpage;                /* Address to kernel page */
    void *upage;                /* Address to user page */
    struct thread *process;     /* Owner of this frame */

    struct hash_elem elem;
    struct list_elem lelem;
  };


void frame_table_init (void);
struct fte *frame_table_find (void *);

void *frame_alloc (void *, enum palloc_flags, bool);
void frame_free (void *kpage);

void pgl_acquire(void);
void pgl_release(void);

#endif
