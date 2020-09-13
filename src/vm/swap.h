#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "devices/disk.h"
#include "threads/thread.h"
#include "vm/frame.h"
#include <hash.h>

void swap_table_init (void);
void swap_table_set_available (size_t idx, bool available);

void swap_out (struct fte *);
void swap_in (struct spte *);

#endif
