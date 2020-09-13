#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <list.h>

void syscall_init (void);

struct fd_elem
  {
    int fd;
    struct file *file;
    struct dir *dir;
    struct list_elem elem;
  };

struct mmap_elem
  {
    int mapid;
    int fd;
    struct file *file;
    int pg_num;
    void *addr;
    struct list_elem elem;
  };

void syscall_exit (int);
#ifdef VM
void syscall_munmap (int);
#endif

#endif /* userprog/syscall.h */
