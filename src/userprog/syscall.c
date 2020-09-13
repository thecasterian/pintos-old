#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

#include "threads/init.h"
#include "userprog/process.h"
#include "threads/malloc.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "devices/input.h"
#include "threads/vaddr.h"
#include "userprog/exception.h"
#ifdef VM
#include "vm/swap.h"
#endif
#include <string.h>
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/free-map.h"
#include "devices/disk.h"

#define INC_PTR(ptr, bytes) ptr = (((uint8_t *)(ptr)) + (bytes))
#define MAX_PATH_LEN 127
#define MAX_PATH_DEPTH 10

static void syscall_handler (struct intr_frame *);
static bool get_args (void *, int);
static struct file *fd_to_file (int);
#ifdef VM
static struct mmap_elem *mapid_to_me (int);
#endif
static int parse_path (char *, char [][NAME_MAX + 1]);
static struct dir *open_last_dir (char *, char *);

static tid_t syscall_exec (const char *);
static int syscall_wait (tid_t);
static bool syscall_create (char *, unsigned);
static bool syscall_remove (char *);
static int syscall_open (const char *);
static int syscall_filesize (int);
static int syscall_read (int, void *, unsigned);
static int syscall_write (int, void *, unsigned);
static void syscall_seek (int, unsigned);
static unsigned syscall_tell (int);
static void syscall_close (int);
#ifdef VM
static int syscall_mmap (int, void *);
#endif
static bool syscall_chdir (const char *);
static bool syscall_mkdir (const char *);
static bool syscall_readdir (int, char *);
static bool syscall_isdir (int);
static int syscall_inumber (int);

/* Lock for file read, write, etc... */
struct lock file_lock;

uint32_t args[7];

#if 1
const int arg_nums[] = {0, 1, 1, 1, 5, 1, 1, 1, 7, 7,
                        5, 1, 1, 5, 1, 1, 1, 5, 1, 1};
#else
const int arg_nums[] = {0, 1, 1, 1, 2, 1, 1, 1, 3, 3,
                        2, 1, 1, 2, 1, 1, 1, 2, 1, 1};
#endif

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&file_lock);
}

/* Number of arguments:

     exit:     1      seek:    5
     exec:     1      tell:    1
     wait:     1      close:   1
     create:   5      mmap:    5
     remove:   1      munmap:  1
     open:     1      chdir:   1
     filesize: 1      mkdir:   1
     read:     7      readdir: 2
     write:    7      isdir:   1
                      inumber: 1
*/

static void
syscall_handler (struct intr_frame *f UNUSED)
{
  if (is_kernel_vaddr (f->esp))
    syscall_exit (-1);

  int syscall_num = *(int *)f->esp;
  if (syscall_num < SYS_HALT || syscall_num > SYS_INUMBER)
    syscall_exit (-1);

  bool success = get_args (f->esp, syscall_num);

  if (!success)
    syscall_exit (-1);

#ifdef VM
  /* Copy esp for page fault handling */
  thread_current ()->user_esp = f->esp;
#endif

  int argc = arg_nums[syscall_num];

  switch (syscall_num)
    {
    case SYS_HALT:      /* void -> void */
      power_off ();
      break;
    case SYS_EXIT:      /* int -> void */
    {
      syscall_exit ((int)args[0]);
      break;
    }
    case SYS_EXEC:      /* char * -> pid_t */
    {
      tid_t tid = syscall_exec ((char *)args[0]);

      f->eax = (uint32_t)tid;
      break;
    }
    case SYS_WAIT:      /* pid_t -> int */
    {
      int child_status = syscall_wait ((tid_t)args[0]);

      f->eax = (uint32_t)child_status;
      break;
    }
    case SYS_CREATE:    /* char *, unsigned -> bool */
    {
      bool success = syscall_create ((char *)args[argc-2], (off_t)args[argc-1]);

      f->eax = (uint32_t)success;
      break;
    }
    case SYS_REMOVE:    /* char * -> bool */
    {
      bool success = syscall_remove ((char *)args[0]);

      f->eax = (uint32_t)success;
      break;
    }
    case SYS_OPEN:      /* char * -> int */
    {
      int fd = syscall_open ((char *)args[0]);

      f->eax = (uint32_t)fd;
      break;
    }
    case SYS_FILESIZE:  /* int -> int */
    {
      int len = syscall_filesize ((int)args[0]);

      f->eax = (uint32_t)len;
      break;
    }
    case SYS_READ:      /* int, void *, unsigned -> int */
    {
      int size = syscall_read ((int)args[argc-3], (void *)args[argc-2],
                               (unsigned)args[argc-1]);

      f->eax = (uint32_t)size;
      break;
    }
    case SYS_WRITE:     /* int, void *, unsigned -> int */
    {
      int size = syscall_write ((int)args[argc-3], (void *)args[argc-2],
                                (unsigned)args[argc-1]);

      f->eax = (uint32_t)size;
      break;
    }
    case SYS_SEEK:      /* int, unsigned -> void */
    {
      syscall_seek ((int)args[argc-2], (unsigned)args[argc-1]);
      break;
    }
    case SYS_TELL:      /* int -> unsigned */
    {
      unsigned pos = syscall_tell ((int)args[0]);

      f->eax = pos;
      break;
    }
    case SYS_CLOSE:     /* int -> void */
    {
      syscall_close ((int)args[0]);
      break;
    }
#ifdef VM
    case SYS_MMAP:
    {
      int mapid = syscall_mmap ((int)args[argc-2], (void *)args[argc-1]);

      f->eax = mapid;
      break;
    }
    case SYS_MUNMAP:
    {
      syscall_munmap ((int)args[0]);
      break;
    }
#endif
    case SYS_CHDIR:
    {
      bool success = syscall_chdir ((const char *)args[0]);

      f->eax = success;
      break;
    }
    case SYS_MKDIR:
    {
      bool success = syscall_mkdir ((const char *)args[0]);

      f->eax = success;
      break;
    }
    case SYS_READDIR:
    {
      bool success = syscall_readdir ((int)args[argc-2], (char *)args[argc-1]);

      f->eax = success;
      break;
    }
    case SYS_ISDIR:
    {
      bool success = syscall_isdir ((int)args[0]);

      f->eax = success;
      break;
    }
    case SYS_INUMBER:
    {
      int inumber = syscall_inumber ((int)args[0]);

      f->eax = inumber;
      break;
    }
    default:
      break;
    }
}

/* Return true if succeeded to get arguments */
static bool
get_args (void *esp, int num)
{
  int argc = arg_nums[num];
  if (argc == 0)
    return true;

  if (is_kernel_vaddr (esp))
    return false;

  INC_PTR (esp, 4);

  int i;
  for (i = 0; i < argc; i++)
    {
      if (is_kernel_vaddr (esp))
        return false;
      args[i] = *(uint32_t *)esp;
      INC_PTR (esp, 4);
    }

  return true;
}

static struct file *
fd_to_file (int fd)
{
  if (fd == 0 || fd == 1)
    return NULL;

  struct thread *curr = thread_current ();
  struct list_elem *e;
  struct fd_elem *fe;

  for (e = list_begin (&curr->fd_list);
       e != list_end (&curr->fd_list); e = list_next (e))
    {
      fe = list_entry (e, struct fd_elem, elem);
      if (fe->fd == fd)
        return fe->file;
    }
  return NULL;
}

static struct dir *
fd_to_dir (int fd)
{
  if (fd == 0 || fd == 1)
    return NULL;

  struct thread *curr = thread_current ();
  struct list_elem *e;
  struct fd_elem *fe;

  for (e = list_begin (&curr->fd_list);
       e != list_end (&curr->fd_list); e = list_next (e))
    {
      fe = list_entry (e, struct fd_elem, elem);
      if (fe->fd == fd)
        return fe->dir;
    }
  return NULL;
}

#ifdef VM
static struct mmap_elem *
mapid_to_me (int mapid)
{
  struct thread *curr = thread_current ();
  struct list_elem *e;
  struct mmap_elem *me;

  for (e = list_begin (&curr->mmap_list);
       e != list_end (&curr->mmap_list); e = list_next (e))
    {
      me = list_entry (e, struct mmap_elem, elem);
      if (me->mapid == mapid)
        return me;
    }
  return NULL;
}
#endif

static int
parse_path (char *path, char parsed[][NAME_MAX + 1])
{
  char *ret_ptr, *next_ptr;

  ret_ptr = strtok_r (path, "/", &next_ptr);

  int i = 0;
  while (ret_ptr)
    {
      strlcpy (parsed[i], ret_ptr, NAME_MAX + 1);

      if (strlen (ret_ptr) > NAME_MAX)
        /* Too long name. Maybe error. */
        return 0;

      ret_ptr = strtok_r (NULL, "/", &next_ptr);
      i++;
    }

  return i;
}

static struct dir *
open_last_dir (char *path, char *remain)
{
  /* E.g.
         path = "/foo/bar/hello.c"
       then
         return = opened directory 'bar'
         remain = "hello.c" */

  struct thread *t = thread_current ();

  char parsed[MAX_PATH_DEPTH][NAME_MAX + 1];
  int n = parse_path (path, parsed);

  if (n == 0)
    return NULL;

  struct dir *curr_dir;
  if (path[0] == '/')
    /* Absolute path. */
    curr_dir = dir_open_root ();
  else
    /* Relative path. */
    {
      if (inode_is_removed (dir_get_inode (t->curr_dir)))
        return NULL;
      curr_dir = dir_reopen (t->curr_dir);
    }

  struct inode *curr_inode;

  int i;
  for (i = 0; i < n-1; i++)
    {
      bool success;

      if (!strcmp (parsed[i], "."))
        {
          /* Change to current directory. */
          if (inode_is_removed (dir_get_inode (t->curr_dir)))
            {

              success = false;
            }
          else
            {
              curr_inode = inode_reopen (dir_get_inode (t->curr_dir));
              success = true;
            }
        }
      else if (!strcmp (parsed[i], ".."))
        {
          /* Change to parent directory. */
          disk_sector_t parent = inode_get_parent_sector (dir_get_inode (curr_dir));
          success = (parent != -1u);
          if (success)
            curr_inode = inode_open (parent);
        }
      else
        success = dir_lookup (curr_dir, parsed[i], &curr_inode);

      dir_close (curr_dir);

      if (!success)
        return NULL;

      curr_dir = dir_open (curr_inode);
    }

  strlcpy (remain, parsed[n-1], NAME_MAX + 1);

  return curr_dir;
}



/* ================================= */
/* Syscall handlers for each syscall */
/* ================================= */

void
syscall_exit (int status)
{
  thread_current ()->exit_status = status;

  printf ("%s: exit(%d)\n", thread_name (), status);
  thread_exit ();
}

static tid_t
syscall_exec (const char *file)
{
  lock_acquire (&file_lock);

  tid_t tid = process_execute (file);

  lock_release (&file_lock);
  return tid;
}

static int
syscall_wait (tid_t tid)
{
  /* If thread is already waiting, cannot wait. */
  if (thread_current ()->is_waiting)
    return -1;

  return process_wait (tid);
}

static bool
syscall_create (char *name, unsigned initial_size)
{
  if (name == NULL)
    syscall_exit (-1);

  char path[MAX_PATH_LEN + 1];
  strlcpy (path, name, MAX_PATH_LEN + 1);

  char new_file_name[NAME_MAX + 1];

  struct dir *last_dir = open_last_dir (path, new_file_name);
  if (last_dir == NULL)
    return false;

  dir_lock_acquire (last_dir);

  bool success = filesys_create (last_dir, new_file_name, initial_size);

  dir_lock_release (last_dir);
  dir_close (last_dir);
  return success;
}

static bool
syscall_remove (char *name)
{
  char path[MAX_PATH_LEN + 1];
  strlcpy (path, name, MAX_PATH_LEN + 1);

  char target_name[NAME_MAX + 1];

  struct dir *last_dir = open_last_dir (path, target_name);
  if (last_dir == NULL)
    return false;

  dir_lock_acquire (last_dir);

  struct inode *target_inode;
  bool exist = dir_lookup (last_dir, target_name, &target_inode);
  if (!exist)
    {
      dir_lock_release (last_dir);
      dir_close (last_dir);
      return false;
    }

  /* If the target is a directory and have children,
     deny removal. */
  if (inode_is_dir (target_inode))
    {
      struct dir *target_dir = dir_open (target_inode);
      char child_name[NAME_MAX + 1];
      if (dir_readdir (target_dir, child_name))
        {
          dir_lock_release (last_dir);
          dir_close (last_dir);
          dir_close (target_dir);
          return false;
        }
    }

  bool success = dir_remove (last_dir, target_name);
  dir_lock_release (last_dir);
  dir_close (last_dir);
  inode_close (target_inode);
  return success;
}

static int
syscall_open (const char *file)
{
  if (file == NULL)
    syscall_exit (-1);

  if (is_kernel_vaddr (file))
    syscall_exit (-1);

  struct thread *t = thread_current ();
  struct file *f;

  /* Exception : open root directory. */
  if (!strcmp (file, "/"))
    f = file_open (inode_open (ROOT_DIR_SECTOR));
  else
    {
      char path[MAX_PATH_LEN + 1];
      strlcpy (path, file, MAX_PATH_LEN + 1);

      char open_file_name[NAME_MAX + 1];

      struct dir *last_dir = open_last_dir (path, open_file_name);
      if (last_dir == NULL)
        return -1;

      if (!strcmp (open_file_name, "."))
        {
          struct inode *curr_inode = dir_get_inode (t->curr_dir);

          if (inode_is_removed (curr_inode))
            goto removed_error;

          f = file_open (inode_reopen (dir_get_inode (t->curr_dir)));
        }
      else if (!strcmp (open_file_name, ".."))
        {
          disk_sector_t parent = inode_get_parent_sector (dir_get_inode (last_dir));
          if (parent == -1u)
            goto removed_error;
          f = file_open (inode_open (parent));
        }
      else
        f = filesys_open (last_dir, open_file_name);

      dir_close (last_dir);
      goto success;

 removed_error:
      dir_close (last_dir);
      return -1;
    }

 success:
  /* File open failed */
  if (f == NULL)
    return -1;

  /* If this file is executable, deny writing */
  if (thread_same_name (file))
    file_deny_write (f);

  struct thread *curr = thread_current ();
  struct fd_elem *fe = malloc (sizeof (struct fd_elem));

  /* fd_elem allocation failed */
  if (fe == NULL)
    {
      file_close (f);
      return -1;
    }

  /* Allocate new fd */
  struct list *fl = &curr->fd_list;
  int new_fd = 2;

  if (!list_empty (fl))
    new_fd = list_entry (list_back (fl),
                         struct fd_elem, elem)->fd + 1;

  fe->fd = new_fd;
  fe->file = f;
  fe->dir = dir_open (inode_reopen (file_get_inode (f)));

  list_push_back (&curr->fd_list, &fe->elem);
  return new_fd;
}

static int
syscall_filesize (int fd)
{
  struct file *file = fd_to_file (fd);

  if (file == NULL)
    return -1;

  return file_length (file);
}

static int
syscall_read (int fd, void *buffer, unsigned size)
{
  if (buffer == NULL)
    return -1;

  if (is_kernel_vaddr (buffer) ||
      is_kernel_vaddr ((uint8_t *)buffer + size - 1))
    syscall_exit (-1);

  /* Use input_getc() */
  if (fd == 0)
    {
      unsigned i;
      for (i = 0; i < size; i++)
        /* Read one character and write to buffer */
        ((uint8_t *)buffer)[i] = input_getc ();

      return (int)size;
    }

  if (fd == 1)
    return -1;

  struct file *f = fd_to_file (fd);
  if (f == NULL)
    return -1;

  return (int)file_read (f, buffer, size);
}

static int
syscall_write (int fd, void *buffer, unsigned size)
{
  if (buffer == NULL)
    return -1;

  if (is_kernel_vaddr (buffer) ||
      is_kernel_vaddr ((uint8_t *) buffer + size - 1))
    syscall_exit (-1);

  if (fd == 0)
    return -1;

  if (fd == 1)
    {
      putbuf (buffer, size);
      return (int) size;
    }

  struct file *f = fd_to_file (fd);
  if (f == NULL)
    return -1;

  /* If directory, writing must be failed. */
  if (inode_is_dir (file_get_inode (f)))
    return -1;

  return (int) file_write (f, buffer, size);

}

static void
syscall_seek (int fd, unsigned pos)
{
  struct file *file = fd_to_file (fd);
  if (file == NULL)
    return;

  file_seek (file, pos);
}

static unsigned
syscall_tell (int fd)
{
  struct file *file = fd_to_file (fd);
  return file_tell (file);
}

static void
syscall_close (int fd)
{
  struct list_elem *e;

#ifdef VM
  struct list *ml = &thread_current ()->mmap_list;
  struct mmap_elem *me;

  for (e = list_begin (ml); e != list_end (ml); e = list_next (e))
    {
      me = list_entry (e, struct mmap_elem, elem);
      if (me->fd == fd)
        {
          /* If file is mapped, reject closing. */
          return;
        }
    }
#endif

  struct list *fl = &thread_current ()->fd_list;
  struct fd_elem *fe;

  for (e = list_begin (fl); e != list_end (fl); e = list_next (e))
    {
      fe = list_entry (e, struct fd_elem, elem);
      if (fe->fd == fd)
        {
          list_remove (&fe->elem);
          file_close (fe->file);
          dir_close (fe->dir);
          free (fe);
          return;
        }
    }
}

#ifdef VM
static int
syscall_mmap (int fd, void *addr)
{
  if (addr == NULL || pg_ofs (addr) != 0)
    return -1;

  struct thread *t = thread_current ();

  struct file *file = fd_to_file (fd);
  if (file == NULL)
    return -1;

  int length = file_length (file);
  if (length == 0)
    return -1;

  int pg_num = (int) pg_round_up ((void *) length) / PGSIZE;
  if (addr + PGSIZE*pg_num >= PHYS_BASE - MAX_STACK_SIZE)
    return -1;

  int i;
  for (i = 0; i < pg_num; i++)
    if (suppl_page_table_find (t->suppl_page_table, addr + PGSIZE*i) != NULL)
      return -1;

  struct mmap_elem *me = malloc (sizeof (struct mmap_elem));
  if (me == NULL)
    return -1;

  for (i = 0; i < pg_num; i++)
    {
      struct spte *p = malloc (sizeof (struct spte));
      ASSERT (p != NULL);

      p->kpage = NULL;
      p->upage = addr + PGSIZE*i;

      p->writable = true;
      p->mapped = true;

      p->file = file;
      p->ofs = PGSIZE*i;
      p->page_read_bytes = (length > PGSIZE) ? PGSIZE : length;
      p->page_zero_bytes = PGSIZE - p->page_read_bytes;

      length -= PGSIZE;

      suppl_page_table_insert (t->suppl_page_table, p);
    }

  int new_mapid = 0;
  if (!list_empty (&t->mmap_list))
    new_mapid = list_entry (list_end (&t->mmap_list), struct mmap_elem, elem)->mapid + 1;

  me->mapid = new_mapid;
  me->fd = fd;
  me->file = file;
  me->pg_num = pg_num;
  me->addr = addr;

  list_push_back (&t->mmap_list, &me->elem);

  return new_mapid;
}

void
syscall_munmap (int mapid)
{
  struct thread *t = thread_current ();
  struct hash *spt = t->suppl_page_table;

  struct mmap_elem *me = mapid_to_me (mapid);
  if (me == NULL)
    return;

  int pg_num = me->pg_num;
  void *addr = me->addr;

  struct spte *p;
  struct fte *f;
  int i;
  for (i = 0; i < pg_num; i++)
    {
      p = suppl_page_table_find (spt, addr + PGSIZE*i);
      if (p->kpage != NULL)
        {
          f = frame_table_find (p->kpage);
          swap_out (f);
        }
      suppl_page_table_del_page (spt, p);
    }

  list_remove (&me->elem);
  free (me);
}
#endif

static bool
syscall_chdir (const char *dir)
{
  /* Exception: change to root directory. */
  if (!strcmp (dir, "/"))
    {
      dir_close (thread_current ()->curr_dir);
      thread_current ()->curr_dir = dir_open_root ();
    }
  else
    {
      char path[MAX_PATH_LEN + 1];
      strlcpy (path, dir, MAX_PATH_LEN + 1);

      char target_dir_name[NAME_MAX + 1];

      struct dir *last_dir = open_last_dir (path, target_dir_name);
      if (last_dir == NULL)
        return false;

      bool success;
      struct inode *new_inode;

      if (!strcmp (target_dir_name, "."))
        {
          new_inode = inode_reopen (dir_get_inode (last_dir));
          success = true;
        }
      else if (!strcmp (target_dir_name, ".."))
        {
          disk_sector_t parent = inode_get_parent_sector (dir_get_inode (last_dir));
          if (parent == -1u)
            success = false;
          else
            {
              new_inode = inode_open (parent);
              success = true;
            }
        }
      else
        success = dir_lookup (last_dir, target_dir_name, &new_inode);

      dir_close (last_dir);

      /* If directory does not exist, it fails. */
      if (!success)
        return false;

      /* Close the current thread's current directory. */
      dir_close (thread_current ()->curr_dir);

      /* Change current directory. */
      thread_current ()->curr_dir = dir_open (new_inode);
    }
  return true;
}

static bool
syscall_mkdir (const char *dir)
{
  char path[MAX_PATH_LEN + 1];
  strlcpy (path, dir, MAX_PATH_LEN + 1);

  char new_dir_name[NAME_MAX + 1];

  struct dir *last_dir = open_last_dir (path, new_dir_name);
  if (last_dir == NULL)
    return false;

  dir_lock_acquire (last_dir);

  struct inode *new_inode;
  bool success = dir_lookup (last_dir, new_dir_name, &new_inode);

  /* If directory already exists, it fails. */
  if (success)
    goto error;

  disk_sector_t new_dir_sector;
  if (!free_map_allocate (1, &new_dir_sector))
    goto error;

  if (!dir_create (new_dir_sector, 16))
    {
      free_map_release (new_dir_sector, 1);
      goto error;
    }

  dir_add (last_dir, new_dir_name, new_dir_sector);

  dir_lock_release (last_dir);
  dir_close (last_dir);
  inode_close (new_inode);
  return true;

 error:
  dir_lock_release (last_dir);
  dir_close (last_dir);
  inode_close (new_inode);
  return false;
}

static bool
syscall_readdir (int fd, char *name)
{
  if (!syscall_isdir (fd))
    return false;

  struct dir *d = fd_to_dir (fd);
  if (d == NULL)
    return false;

  dir_lock_acquire (d);

  bool success = dir_readdir (d, name);

  dir_lock_release (d);
  return success;
}

static bool
syscall_isdir (int fd)
{
  struct dir *d = fd_to_dir (fd);
  if (d == NULL)
    return false;

  return inode_is_dir (dir_get_inode (d));
}

static int
syscall_inumber (int fd)
{
  struct dir *d = fd_to_dir (fd);
  if (d == NULL)
    return -1;

  int inumber = inode_get_inumber (dir_get_inode (d));

  return inumber;
}
