#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "LibDisk.h"
#include "LibFS.h"

// set to 1 to have detailed debug print-outs and 0 to have none
#define FSDEBUG    1

#if FSDEBUG
#define dprintf    printf
#else
#define dprintf    noprintf
void noprintf(char *str, ...) {
}

#endif

// the file system partitions the disk into five parts:

// 1. the superblock (one sector), which contains a magic number at
// its first four bytes (integer)
#define SUPERBLOCK_START_SECTOR    0

// the magic number chosen for our file system
#define OS_MAGIC                   0xdeadbeef

// 2. the inode bitmap (one or more sectors), which indicates whether
// the particular entry in the inode table (#4) is currently in use
#define INODE_BITMAP_START_SECTOR    1

// the total number of bytes and sectors needed for the inode bitmap;
// we use one bit for each inode (whether it's a file or directory) to
// indicate whether the particular inode in the inode table is in use
#define INODE_BITMAP_SIZE       ((MAX_FILES + 7) / 8)
#define INODE_BITMAP_SECTORS    ((INODE_BITMAP_SIZE + SECTOR_SIZE - 1) / SECTOR_SIZE)

// 3. the sector bitmap (one or more sectors), which indicates whether
// the particular sector in the disk is currently in use
#define SECTOR_BITMAP_START_SECTOR    (INODE_BITMAP_START_SECTOR + INODE_BITMAP_SECTORS)

// the total number of bytes and sectors needed for the data block
// bitmap (we call it the sector bitmap); we use one bit for each
// sector of the disk to indicate whether the sector is in use or not
#define SECTOR_BITMAP_SIZE       ((TOTAL_SECTORS + 7) / 8)
#define SECTOR_BITMAP_SECTORS    ((SECTOR_BITMAP_SIZE + SECTOR_SIZE - 1) / SECTOR_SIZE)

// 4. the inode table (one or more sectors), which contains the inodes
// stored consecutively
#define INODE_TABLE_START_SECTOR    (SECTOR_BITMAP_START_SECTOR + SECTOR_BITMAP_SECTORS)

// an inode is used to represent each file or directory; the data
// structure supposedly contains all necessary information about the
// corresponding file or directory
typedef struct _inode {
  int size;                       // the size of the file or number of directory entries
  int type;                       // 0 means regular file; 1 means directory
  int data[MAX_SECTORS_PER_FILE]; // indices to sectors containing data blocks
} inode_t;

// the inode structures are stored consecutively and yet they don't
// straddle accross the sector boundaries; that is, there may be
// fragmentation towards the end of each sector used by the inode
// table; each entry of the inode table is an inode structure; there
// are as many entries in the table as the number of files allowed in
// the system; the inode bitmap (#2) indicates whether the entries are
// current in use or not
#define INODES_PER_SECTOR      (SECTOR_SIZE / sizeof(inode_t))
#define INODE_TABLE_SECTORS    ((MAX_FILES + INODES_PER_SECTOR - 1) / INODES_PER_SECTOR)


// 5. the data blocks; all the rest sectors are reserved for data
// blocks for the content of files and directories
#define DATABLOCK_START_SECTOR    (INODE_TABLE_START_SECTOR + INODE_TABLE_SECTORS)

// other file related definitions

// max length of a path is 256 bytes (including the ending null)
#define MAX_PATH          256

// max length of a filename is 16 bytes (including the ending null)
#define MAX_NAME          16

// max number of open files is 256
#define MAX_OPEN_FILES    256

// each directory entry represents a file/directory in the parent
// directory, and consists of a file/directory name (less than 16
// bytes) and an integer inode number
typedef struct _dirent {
  char fname[MAX_NAME]; // name of the file
  int  inode;           // inode of the file
} dirent_t;

// the number of directory entries that can be contained in a sector
#define DIRENTS_PER_SECTOR    (SECTOR_SIZE / sizeof(dirent_t))

// global errno value here
int osErrno;

// the name of the disk backstore file (with which the file system is booted)
static char bs_filename[1024];

/* the following functions are internal helper functions */

// check magic number in the superblock; return 1 if OK, and 0 if not
static int check_magic() {
  dprintf("First data sector is #%d\n", DATABLOCK_START_SECTOR);
  char buf[SECTOR_SIZE];

  if (Disk_Read(SUPERBLOCK_START_SECTOR, buf) < 0) {
    return(0);
  }
  if (*(int *)buf == OS_MAGIC) {
    return(1);
  }else {
    return(0);
  }
}

int sgn(int n) {
  if (n == 0) {
    return(0);
  }else if (n > 0) {
    return(1);
  }else {
    return(-1);
  }
}

// Written by Dario Gonzalez
// initialize a bitmap with 'num' sectors starting from 'start'
// sector; all bits should be set to zero except that the first
// 'nbits' number of bits are set to one
static void bitmap_init(int start, int num, int nbits) {
  dprintf("Creating new bitmap at sec %d, %d secs long, %d bits set to 1\n", start, num, nbits);
  char bitmap_buf[SECTOR_SIZE];    //chars are size 1

  int a = nbits / 8 / SECTOR_SIZE; //number of sectors that are all 1
  int b = nbits / 8 % SECTOR_SIZE; //number of bytes on first sector after a
  int c = num - a - sgn(b);        //number of sectors that are all 0

  //write out all full sectors

  for (int i = 0; i < SECTOR_SIZE; i++) {
    bitmap_buf[i] = 0xff;
  }
  for (int i = start; i < start + a; i++) {
    Disk_Write(i, bitmap_buf);
  }

  unsigned char bits[8] = { 0x0, 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE };
  //write out partial sector
  int r = nbits % 8;
  dprintf("about to write partial byte %x\n", bits[r]);

  bitmap_buf[b] = bits[r];
  for (int i = b + 1; i < SECTOR_SIZE; i++) {
    bitmap_buf[i] = 0;
  }
  Disk_Write(start + a, bitmap_buf);


  //write out 0 sectors
  for (int i = 0; i < b; i++) {
    bitmap_buf[i] = 0;
  }
  for (int i = start + a + 1; i < start + a + 1 + c; i++) {
    Disk_Write(i, bitmap_buf);
  }
}

// Written by Dario Gonzalez
// set the first unused bit from a bitmap of 'nbits' bits (flip the
// first zero appeared in the bitmap to one) and return its location;
// return -1 if the bitmap is already full (no more zeros)
static int bitmap_first_unused(int start, int num, int nbits) {
  unsigned char buf[SECTOR_SIZE];
  unsigned char bits[8] = { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 };

  for (int i = 0; i < num; i++) {
    if (Disk_Read(i + start, buf) < 0) {
      return(-2);
    }

    for (int j = 0; j < SECTOR_SIZE; j++) {
      if (buf[j] < 0xff) {//a bit isn't one since 1111 1111 is 255
        unsigned char b = buf[j];
        for (int k = 0; k < 8; k++) {
          if ((b & bits[k]) == 0) {
            dprintf("found a free bit at byte %d, bit %d\n", j, k);

            int pos = (i * SECTOR_SIZE + j) * 8 + k;
            //check if we went too far
            buf[j] |= bits[k];
            if (Disk_Write(i + start, buf) < 0) {
              return(-2);
            }

            if (pos < nbits) {
              return(pos);
            }else {
              return(-1);
            }
          }
        }
      }
    }
  }
  return(-1);
}

// Written by Dario Gonzalez
// reset the i-th bit of a bitmap with 'num' sectors starting from
// 'start' sector; return 0 if successful, -1 otherwise
static int bitmap_reset(int start, int num, int ibit) {
  int           sector = start + ibit / SECTOR_SIZE;
  int           byte   = ibit % SECTOR_SIZE / 8; //ie which byte is it in
  int           bit    = ibit % 8;
  unsigned char buf[SECTOR_SIZE];

  if (Disk_Read(sector, buf) < 0) {
    return(-2);
  }

  unsigned char mask = ~(128 >> bit); //ie 7 '1's with a 0 somewhere
  buf[byte] &= mask;
  if (Disk_Write(sector, buf) < 0) {
    return(-2);
  }

  return(0);
}

// Written by Dario Gonzalez
// return 1 if the file name is illegal; otherwise, return 0; legal
// characters for a file name include letters (case sensitive),
// numbers, dots, dashes, and underscores; and a legal file name
// should not be more than MAX_NAME-1 in length
static int illegal_filename(char *name) {
  char *curr;
  int   length = 0;

  for (curr = name; *curr != '\0'; curr++) {
    length++;
    char c = *curr;
    if (!((c >= 'A' && c <= 'Z') ||
          (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') ||
          c == '.' ||
          c == '-' ||
          c == '_')) {
      return(1);
    }
  }
  if (length < MAX_NAME) {
    return(0);
  }
  return(1);
}

// return the child inode of the given file name 'fname' from the
// parent inode; the parent inode is currently stored in the segment
// of inode table in the cache (we cache only one disk sector for
// this); once found, both cached_inode_sector and cached_inode_buffer
// may be updated to point to the segment of inode table containing
// the child inode; the function returns -1 if no such file is found;
// it returns -2 is something else is wrong (such as parent is not
// directory, or there's read error, etc.)
static int find_child_inode(int parent_inode, char *fname,
                            int *cached_inode_sector, char *cached_inode_buffer) {
  int cached_start_entry = ((*cached_inode_sector) - INODE_TABLE_START_SECTOR) * INODES_PER_SECTOR;
  int offset             = parent_inode - cached_start_entry;

  assert(0 <= offset && offset < INODES_PER_SECTOR);
  inode_t *parent = (inode_t *)(cached_inode_buffer + offset * sizeof(inode_t));
  dprintf("... load parent inode: %d (size=%d, type=%d)\n",
          parent_inode, parent->size, parent->type);
  if (parent->type != 1) {
    dprintf("... parent not a directory\n");
    return(-2);
  }

  int nentries = parent->size;       // remaining number of directory entries
  int idx      = 0;
  while (nentries > 0) {
    char buf[SECTOR_SIZE];             // cached content of directory entries
    if (Disk_Read(parent->data[idx], buf) < 0) {
      return(-2);
    }
    for (int i = 0; i < DIRENTS_PER_SECTOR; i++) {
      if (i > nentries) {
        break;
      }
      if (!strcmp(((dirent_t *)buf)[i].fname, fname)) {
        // found the file/directory; update inode cache
        int child_inode = ((dirent_t *)buf)[i].inode;
        dprintf("... found child_inode=%d\n", child_inode);
        int sector = INODE_TABLE_START_SECTOR + child_inode / INODES_PER_SECTOR;
        if (sector != (*cached_inode_sector)) {
          *cached_inode_sector = sector;
          if (Disk_Read(sector, cached_inode_buffer) < 0) {
            return(-2);
          }
          dprintf("... load inode table for child\n");
        }
        return(child_inode);
      }
    }
    idx++; nentries -= DIRENTS_PER_SECTOR;
  }
  dprintf("... could not find child inode\n");
  return(-1);      // not found
}

// follow the absolute path; if successful, return the inode of the
// parent directory immediately before the last file/directory in the
// path; for example, for '/a/b/c/d.txt', the parent is '/a/b/c' and
// the child is 'd.txt'; the child's inode is returned through the
// parameter 'last_inode' and its file name is returned through the
// parameter 'last_fname' (both are references); it's possible that
// the last file/directory is not in its parent directory, in which
// case, 'last_inode' points to -1; if the function returns -1, it
// means that we cannot follow the path
static int follow_path(char *path, int *last_inode, char *last_fname) {
  if (!path) {
    dprintf("... invalid path\n");
    return(-1);
  }
  if (path[0] != '/') {
    dprintf("... '%s' not absolute path\n", path);
    return(-1);
  }

  // make a copy of the path (skip leading '/'); this is necessary
  // since the path is going to be modified by strsep()
  char pathstore[MAX_PATH];
  strncpy(pathstore, path + 1, MAX_PATH - 1);
  pathstore[MAX_PATH - 1] = '\0';     // for safety
  char *lpath = pathstore;

  int parent_inode = -1, child_inode = 0;       // start from root
  // cache the disk sector containing the root inode
  int  cached_sector = INODE_TABLE_START_SECTOR;
  char cached_buffer[SECTOR_SIZE];
  if (Disk_Read(cached_sector, cached_buffer) < 0) {
    return(-1);
  }
  dprintf("... load inode table for root from disk sector %d\n", cached_sector);

  // for each file/directory name separated by '/'
  char *token;
  while ((token = strsep(&lpath, "/")) != NULL) {
    dprintf("... process token: '%s'\n", token);
    if (*token == '\0') {
      continue;                              // multiple '/' ignored
    }
    if (illegal_filename(token)) {
      dprintf("... illegal file name: '%s'\n", token);
      return(-1);
    }
    if (child_inode < 0) {
      // regardless whether child_inode was not found previously, or
      // there was issues related to the parent (say, not a
      // directory), or there was a read error, we abort
      dprintf("... parent inode can't be established\n");
      return(-1);
    }
    parent_inode = child_inode;
    child_inode  = find_child_inode(parent_inode, token,
                                    &cached_sector, cached_buffer);
    if (last_fname) {
      strcpy(last_fname, token);
    }
  }
  if (child_inode < -1) {
    return(-1);                         // if there was error, abort
  }else {
    // there was no error, several possibilities:
    // 1) '/': parent = -1, child = 0
    // 2) '/valid-dirs.../last-valid-dir/not-found': parent=last-valid-dir, child=-1
    // 3) '/valid-dirs.../last-valid-dir/found: parent=last-valid-dir, child=found
    // in the first case, we set parent=child=0 as special case
    if (parent_inode == -1 && child_inode == 0) {
      parent_inode = 0;
    }
    dprintf("... found parent_inode=%d, child_inode=%d\n", parent_inode, child_inode);
    *last_inode = child_inode;
    return(parent_inode);
  }
}

// add a new file or directory (determined by 'type') of given name
// 'file' under parent directory represented by 'parent_inode'
int add_inode(int type, int parent_inode, char *file) {
  // get a new inode for child
  int child_inode = bitmap_first_unused(INODE_BITMAP_START_SECTOR, INODE_BITMAP_SECTORS, INODE_BITMAP_SIZE);

  if (child_inode < 0) {
    dprintf("... error: inode table is full\n");
    return(-1);
  }
  dprintf("... new child inode %d\n", child_inode);

  // load the disk sector containing the child inode
  int  inode_sector = INODE_TABLE_START_SECTOR + child_inode / INODES_PER_SECTOR;
  char inode_buffer[SECTOR_SIZE];
  if (Disk_Read(inode_sector, inode_buffer) < 0) {
    return(-1);
  }
  dprintf("... load inode table for child inode from disk sector %d\n", inode_sector);

  // get the child inode
  int inode_start_entry = (inode_sector - INODE_TABLE_START_SECTOR) * INODES_PER_SECTOR;
  int offset            = child_inode - inode_start_entry;
  assert(0 <= offset && offset < INODES_PER_SECTOR);
  inode_t *child = (inode_t *)(inode_buffer + offset * sizeof(inode_t));

  // update the new child inode and write to disk
  memset(child, 0, sizeof(inode_t));
  child->type = type;
  if (Disk_Write(inode_sector, inode_buffer) < 0) {
    return(-1);
  }
  dprintf("... update child inode %d (size=%d, type=%d), update disk sector %d\n",
          child_inode, child->size, child->type, inode_sector);

  // get the disk sector containing the parent inode
  inode_sector = INODE_TABLE_START_SECTOR + parent_inode / INODES_PER_SECTOR;
  if (Disk_Read(inode_sector, inode_buffer) < 0) {
    return(-1);
  }
  dprintf("... load inode table for parent inode %d from disk sector %d\n",
          parent_inode, inode_sector);

  // get the parent inode
  inode_start_entry = (inode_sector - INODE_TABLE_START_SECTOR) * INODES_PER_SECTOR;
  offset            = parent_inode - inode_start_entry;
  assert(0 <= offset && offset < INODES_PER_SECTOR);
  inode_t *parent = (inode_t *)(inode_buffer + offset * sizeof(inode_t));
  dprintf("... get parent inode %d (size=%d, type=%d)\n",
          parent_inode, parent->size, parent->type);

  // get the dirent sector
  if (parent->type != 1) {
    dprintf("... error: parent inode is not directory\n");
    return(-2);            // parent not directory
  }
  int  group = parent->size / DIRENTS_PER_SECTOR;
  char dirent_buffer[SECTOR_SIZE];
  if (group * DIRENTS_PER_SECTOR == parent->size) {
    // new disk sector is needed
    int newsec = bitmap_first_unused(SECTOR_BITMAP_START_SECTOR, SECTOR_BITMAP_SECTORS, SECTOR_BITMAP_SIZE);
    if (newsec < 0) {
      dprintf("... error: disk is full\n");
      return(-1);
    }
    parent->data[group] = newsec;
    memset(dirent_buffer, 0, SECTOR_SIZE);
    dprintf("... new disk sector %d for dirent group %d\n", newsec, group);
  }else {
    if (Disk_Read(parent->data[group], dirent_buffer) < 0) {
      return(-1);
    }
    dprintf("... load disk sector %d for dirent group %d\n", parent->data[group], group);
  }

  // add the dirent and write to disk
  int start_entry = group * DIRENTS_PER_SECTOR;
  offset = parent->size - start_entry;
  dirent_t *dirent = (dirent_t *)(dirent_buffer + offset * sizeof(dirent_t));
  strncpy(dirent->fname, file, MAX_NAME);
  dirent->inode = child_inode;
  if (Disk_Write(parent->data[group], dirent_buffer) < 0) {
    return(-1);
  }
  dprintf("... append dirent %d (name='%s', inode=%d) to group %d, update disk sector %d\n",
          parent->size, dirent->fname, dirent->inode, group, parent->data[group]);

  // update parent inode and write to disk
  parent->size++;
  if (Disk_Write(inode_sector, inode_buffer) < 0) {
    return(-1);
  }
  dprintf("... update parent inode on disk sector %d\n", inode_sector);

  return(0);
}

// used by both File_Create() and Dir_Create(); type=0 is file, type=1
// is directory
int create_file_or_directory(int type, char *pathname) {
  int  child_inode;
  char last_fname[MAX_NAME];
  int  parent_inode = follow_path(pathname, &child_inode, last_fname);

  if (parent_inode >= 0) {
    if (child_inode >= 0) {
      dprintf("... file/directory '%s' already exists, failed to create\n", pathname);
      osErrno = E_CREATE;
      return(-1);
    }else {
      if (add_inode(type, parent_inode, last_fname) >= 0) {
        dprintf("... successfully created file/directory: '%s'\n", pathname);
        return(0);
      }else {
        dprintf("... error: something wrong with adding child inode\n");
        osErrno = E_CREATE;
        return(-1);
      }
    }
  }else {
    dprintf("... error: something wrong with the file/path: '%s'\n", pathname);
    osErrno = E_CREATE;
    return(-1);
  }
}

// Written by Dario Gonzalez
// remove the child from parent; the function is called by both
// File_Unlink() and Dir_Unlink(); the function returns 0 if success,
// -1 if general error, -2 if directory not empty, -3 if wrong type
int remove_inode(int type, int parent_inode, int child_inode) {
  //load child node info
  //first have to find the sector it is in
  int child_loc        = INODE_TABLE_START_SECTOR + child_inode / INODES_PER_SECTOR;
  int child_loc_offset = child_inode % INODES_PER_SECTOR;

  char child_inode_buf[SECTOR_SIZE];

  if (Disk_Read(child_loc, child_inode_buf) < 0) {
    return(-1);
  }
  inode_t *child = (inode_t *)(child_inode_buf + child_loc_offset * sizeof(inode_t));
  if (child->type != type) {
    dprintf("remove_inode: Given type %d, found %d when removing inode\n", child->type, type);
    return(-3);
  }
  if (child->type == 1) {  //ie the file is a directory
    if (child->size > 0) { //the directory isn't empty
      dprintf("remove_inode: Tried to unlink a directory of size %d\n", child->size);
      return(-2);
    }
  }else{
    if (child->size > 0) {
      dprintf("remove_inode: tried to remove a file that still claimed blocks\n");
      return(-1); //the file to be deleted still claims blocks
    }
  }


  int parent_loc        = INODE_TABLE_START_SECTOR + parent_inode / INODES_PER_SECTOR;
  int parent_loc_offset = parent_inode % INODES_PER_SECTOR;

  //for now create a new buffer to store the parent_inode
  char parent_inode_buf[SECTOR_SIZE];
  //load parent_inode info
  if (Disk_Read(parent_loc, parent_inode_buf) < 0) {
    return(-1);
  }
  inode_t *parent = (inode_t *)(parent_inode_buf + parent_loc_offset * sizeof(inode_t));
  if (parent->type != 1) { //ie the parent isn't a directory
    dprintf("remove_inode: Tried to unlink from a file with type %d \n", parent->type);
    return(-3);
  }

  //now we need to find the child inode among the dirents and zero it
  int found              = 0;
  int full_dirent_secs   = parent->size / DIRENTS_PER_SECTOR;
  int partial_dirent_sec = sgn(parent->size - full_dirent_secs * DIRENTS_PER_SECTOR); //ie either 0 or 1

  char dirent_buf[SECTOR_SIZE];
  //search all full dirent sectors
  dprintf("remove_inode: searching full dirent sectors...\n");
  for (int dir_sec = 0; dir_sec < full_dirent_secs; dir_sec++) {
    if (!found) {
      break; //might be extraneous
    }
    if (Disk_Read(parent->data[dir_sec], dirent_buf) < 0) {
      return(-1);
    }
    int dir = 0;
    for (dirent_t *cur_dir_ent = (dirent_t *)dirent_buf; dir < DIRENTS_PER_SECTOR; dir++, cur_dir_ent++) {
      if (cur_dir_ent->inode == child_inode) {         //we found it!
        dprintf("remove_inode: found inode at dir %d in sector %d, the parent's %d data sector\n", dir, parent->data[dir_sec], dir_sec);
        memset(cur_dir_ent, 0, sizeof *cur_dir_ent);   //zero-out the dir entry
        found = parent->data[dir_sec];
        break;
      }
    }
  }
  //check the last data sector

  dprintf("remove_inode: searching last dirent sectors...\n");
  if (!found && partial_dirent_sec) {
    if (Disk_Read(parent->data[full_dirent_secs], dirent_buf) < 0) {
      return(-1);
    }
    int dir = 0;
    for (dirent_t *cur_dir_ent = (dirent_t *)dirent_buf; dir < parent->size % DIRENTS_PER_SECTOR; dir++, cur_dir_ent++) {
      if (cur_dir_ent->inode == child_inode) {         //we found it!
        dprintf("remove_inode: found inode at dir %d in sector %d, the parent's %d data sector\n", dir, parent->data[full_dirent_secs], full_dirent_secs);

        memset(cur_dir_ent, 0, sizeof *cur_dir_ent);   //zero-out the dir entry
        found = parent->data[full_dirent_secs];
        break;
      }
    }
  }

  if (!found) {
    return(-1);
  }

  //All this does is zero out that entry, but the size of the directory
  //never changes, so we get horrible fragmentation
  //TODO figure out a better way of dealing with this
  //maybe always run through dirents backwards, and for each one that is all 0
  //decrement the size of the directory. This has the benefit that directories
  //aren't permanent, but for example if you add 1000 directories and delete
  //all but the last one, that directory still occupies tons of space.

  //set the child's inode to free
  bitmap_reset(INODE_BITMAP_START_SECTOR, INODE_BITMAP_SECTORS, child_inode);

  //write out zeroed dirent to corresponding parent data sector
  if (Disk_Write(found, dirent_buf) < 0) {
    return(-1);
  }
  return(0);
}

// representing an open file
typedef struct _open_file {
  int inode;     // pointing to the inode of the file (0 means entry not used)
  int size;      // file size cached here for convenience
  int pos;       // read/write position
} open_file_t;
static open_file_t open_files[MAX_OPEN_FILES];

// return true if the file pointed to by inode has already been open
int is_file_open(int inode) {
  for (int i = 0; i < MAX_OPEN_FILES; i++) {
    if (open_files[i].inode == inode) {
      return(1);
    }
  }
  return(0);
}

// return a new file descriptor not used; -1 if full
int new_file_fd() {
  for (int i = 0; i < MAX_OPEN_FILES; i++) {
    if (open_files[i].inode <= 0) {
      return(i);
    }
  }
  return(-1);
}

/* end of internal helper functions, start of API functions */

int FS_Boot(char *backstore_fname) {
  dprintf("FS_Boot('%s'):\n", backstore_fname);
  // initialize a new disk (this is a simulated disk)
  if (Disk_Init() < 0) {
    dprintf("... disk init failed\n");
    osErrno = E_GENERAL;
    return(-1);
  }
  dprintf("... disk initialized\n");

  // we should copy the filename down; if not, the user may change the
  // content pointed to by 'backstore_fname' after calling this function
  strncpy(bs_filename, backstore_fname, 1024);
  bs_filename[1023] = '\0';       // for safety

  // we first try to load disk from this file
  if (Disk_Load(bs_filename) < 0) {
    dprintf("... load disk from file '%s' failed\n", bs_filename);

    // if we can't open the file; it means the file does not exist, we
    // need to create a new file system on disk
    if (diskErrno == E_OPENING_FILE) {
      dprintf("... couldn't open file, create new file system\n");

      // format superblock
      char buf[SECTOR_SIZE];
      memset(buf, 0, SECTOR_SIZE);
      *(int *)buf = OS_MAGIC;
      if (Disk_Write(SUPERBLOCK_START_SECTOR, buf) < 0) {
        dprintf("... failed to format superblock\n");
        osErrno = E_GENERAL;
        return(-1);
      }
      dprintf("... formatted superblock (sector %d)\n", SUPERBLOCK_START_SECTOR);

      // format inode bitmap (reserve the first inode to root)
      bitmap_init(INODE_BITMAP_START_SECTOR, INODE_BITMAP_SECTORS, 1);
      dprintf("... formatted inode bitmap (start=%d, num=%d)\n",
              (int)INODE_BITMAP_START_SECTOR, (int)INODE_BITMAP_SECTORS);

      // format sector bitmap (reserve the first few sectors to
      // superblock, inode bitmap, sector bitmap, and inode table)
      bitmap_init(SECTOR_BITMAP_START_SECTOR, SECTOR_BITMAP_SECTORS,
                  DATABLOCK_START_SECTOR);
      dprintf("... formatted sector bitmap (start=%d, num=%d)\n",
              (int)SECTOR_BITMAP_START_SECTOR, (int)SECTOR_BITMAP_SECTORS);

      // format inode tables
      for (int i = 0; i < INODE_TABLE_SECTORS; i++) {
        memset(buf, 0, SECTOR_SIZE);
        if (i == 0) {
          // the first inode table entry is the root directory
          ((inode_t *)buf)->size = 0;
          ((inode_t *)buf)->type = 1;
        }
        if (Disk_Write(INODE_TABLE_START_SECTOR + i, buf) < 0) {
          dprintf("... failed to format inode table\n");
          osErrno = E_GENERAL;
          return(-1);
        }
      }
      dprintf("... formatted inode table (start=%d, num=%d)\n",
              (int)INODE_TABLE_START_SECTOR, (int)INODE_TABLE_SECTORS);

      // we need to synchronize the disk to the backstore file (so
      // that we don't lose the formatted disk)
      if (Disk_Save(bs_filename) < 0) {
        // if can't write to file, something's wrong with the backstore
        dprintf("... failed to save disk to file '%s'\n", bs_filename);
        osErrno = E_GENERAL;
        return(-1);
      }else {
        // everything's good now, boot is successful
        dprintf("... successfully formatted disk, boot successful\n");
        memset(open_files, 0, MAX_OPEN_FILES * sizeof(open_file_t));
        return(0);
      }
    }else {
      // something wrong loading the file: invalid param or error reading
      dprintf("... couldn't read file '%s', boot failed\n", bs_filename);
      osErrno = E_GENERAL;
      return(-1);
    }
  }else {
    dprintf("... load disk from file '%s' successful\n", bs_filename);

    // we successfully loaded the disk, we need to do two more checks,
    // first the file size must be exactly the size as expected (thiis
    // supposedly should be folded in Disk_Load(); and it's not)
    int   sz = 0;
    FILE *f  = fopen(bs_filename, "r");
    if (f) {
      fseek(f, 0, SEEK_END);
      sz = ftell(f);
      fclose(f);
    }
    if (sz != SECTOR_SIZE * TOTAL_SECTORS) {
      dprintf("... check size of file '%s' failed\n", bs_filename);
      osErrno = E_GENERAL;
      return(-1);
    }
    dprintf("... check size of file '%s' successful\n", bs_filename);

    // check magic
    if (check_magic()) {
      // everything's good by now, boot is successful
      dprintf("... check magic successful\n");
      memset(open_files, 0, MAX_OPEN_FILES * sizeof(open_file_t));
      return(0);
    }else {
      // mismatched magic number
      dprintf("... check magic failed, boot failed\n");
      osErrno = E_GENERAL;
      return(-1);
    }
  }
}

int FS_Sync() {
  if (Disk_Save(bs_filename) < 0) {
    // if can't write to file, something's wrong with the backstore
    dprintf("FS_Sync():\n... failed to save disk to file '%s'\n", bs_filename);
    osErrno = E_GENERAL;
    return(-1);
  }else {
    // everything's good now, sync is successful
    dprintf("FS_Sync():\n... successfully saved disk to file '%s'\n", bs_filename);
    return(0);
  }
}

int File_Create(char *file) {
  dprintf("File_Create('%s'):\n", file);
  return(create_file_or_directory(0, file));
}

//Written by Dario Gonzalez

/*
 * This function is the opposite of File_Create(). This function should delete the file referenced by
 * file, including removing its name from the directory it is in, and freeing up any data blocks and
 * inodes that the file has been using. If the file does not currently exist, return -1 and set osErrno to
 * E_NO_SUCH_FILE. If the file is currently open, return -1 and set osErrno to E_FILE_IN_USE
 * (and do NOT delete the file). Upon success, return 0.
 *
 */
int File_Unlink(char *file) {
  char file_name[255];
  int  child_inode;
  int  parent_inode = follow_path(file, &child_inode, file_name);

  if (parent_inode < 0) {
    osErrno = E_NO_SUCH_FILE;
    return(-1);
  }
  if (is_file_open(child_inode)) {
    osErrno = E_FILE_IN_USE;
    return(-1);
  }
  int  child_inode_sec = INODE_TABLE_START_SECTOR + child_inode / INODES_PER_SECTOR;
  char child_inode_buffer[SECTOR_SIZE];
  int  child_loc_offset = child_inode % INODES_PER_SECTOR;
  if (Disk_Read(child_inode_sec, child_inode_buffer) < 0) {
    return(-1);
  }

  inode_t *child = (inode_t *)(child_inode_buffer + child_loc_offset * sizeof(inode_t));
  if (child->type != 0) {
    return(-2); //file isnt a file
  }
  //free child sectors
  dprintf("File_Unlink: deleting %d sectors of file \n", child->size);
  for (int i = 0; i < child->size; i++) {
    bitmap_reset(SECTOR_BITMAP_START_SECTOR, SECTOR_BITMAP_SECTORS, child->data[i]);
  }
  child->size = 0;
  if (Disk_Write(child_inode_sec, child_inode_buffer) < 0) {
    return(-1);
  }

  //TODO error check
  int r;
  if ((r = remove_inode(0, parent_inode, child_inode)) < 0) {
    dprintf("File_Unlink: remove_inode returned an error: %d\n", r);
    return(-1);
  }

  return(0);
}

int File_Open(char *file) {
  dprintf("File_Open('%s'):\n", file);
  int fd = new_file_fd();
  if (fd < 0) {
    dprintf("... max open files reached\n");
    osErrno = E_TOO_MANY_OPEN_FILES;
    return(-1);
  }

  int child_inode;
  follow_path(file, &child_inode, NULL);
  if (child_inode >= 0) {      // child is the one
    // load the disk sector containing the inode
    int  inode_sector = INODE_TABLE_START_SECTOR + child_inode / INODES_PER_SECTOR;
    char inode_buffer[SECTOR_SIZE];
    if (Disk_Read(inode_sector, inode_buffer) < 0) {
      osErrno = E_GENERAL; return(-1);
    }
    dprintf("... load inode table for inode from disk sector %d\n", inode_sector);

    // get the inode
    int inode_start_entry = (inode_sector - INODE_TABLE_START_SECTOR) * INODES_PER_SECTOR;
    int offset            = child_inode - inode_start_entry;
    assert(0 <= offset && offset < INODES_PER_SECTOR);
    inode_t *child = (inode_t *)(inode_buffer + offset * sizeof(inode_t));
    dprintf("... inode %d (size=%d, type=%d)\n",
            child_inode, child->size, child->type);

    if (child->type != 0) {
      dprintf("... error: '%s' is not a file\n", file);
      osErrno = E_GENERAL;
      return(-1);
    }

    // initialize open file entry and return its index
    open_files[fd].inode = child_inode;
    open_files[fd].size  = child->size;
    open_files[fd].pos   = 0;
    return(fd);
  }else {
    dprintf("... file '%s' is not found\n", file);
    osErrno = E_NO_SUCH_FILE;
    return(-1);
  }
}

//Written by Dario Gonzalez

/*
 * File_Read() should read size bytes from the file referenced by the file descriptor fd. The data should
 * be read into the buffer pointed to by buffer. All reads should begin at the current location of the file
 * pointer, and file pointer should be updated after the read to the new location. If the file is not open,
 * return -1, and set osErrno to E_BAD_FD. If the file is open, the number of bytes actually read
 * should be returned, which can be less than or equal to size. (The number could be less than the
 * requested bytes because the end of the file could be reached.) If the file pointer is already at the end
 * of the file, zero should be returned, even under repeated calls to File_Read().
 *
 */
int File_Read(int fd, void *buffer, int size) {
  dprintf("File_Read: reading from file %d, up to %d bytes\n", fd, size);
  if (!is_file_open(fd)) {
    osErrno = E_BAD_FD;
    return(-1);
  }
  open_file_t *f = &open_files[fd];
  dprintf("File_Read: file size is %d, file cursor at %d\n", f->size, f->pos);
  if (f->pos == f->size) {
    return(0);
  }

  //Taken from File_Open
  // load the disk sector containing the inode
  int  inode_sector = INODE_TABLE_START_SECTOR + f->inode / INODES_PER_SECTOR;
  char inode_buffer[SECTOR_SIZE];
  if (Disk_Read(inode_sector, inode_buffer) < 0) {
    osErrno = E_GENERAL; return(-1);
  }
  dprintf("... load inode table for inode from disk sector %d\n", inode_sector);

  // get the inode
  int inode_start_entry = (inode_sector - INODE_TABLE_START_SECTOR) * INODES_PER_SECTOR;
  int offset            = f->inode - inode_start_entry;
  assert(0 <= offset && offset < INODES_PER_SECTOR);
  inode_t *child = (inode_t *)(inode_buffer + offset * sizeof(inode_t));
  //Done taking from File_Open

  int  left            = size;
  int  curr_pos_in_sec = f->pos % SECTOR_SIZE;
  int  curr_sec        = f->pos / SECTOR_SIZE;
  int  out_pos         = 0;
  char data_buf[SECTOR_SIZE];
  dprintf("File_Read: Going to read %d bytes from sec %d, starting at offset %d\n", left, curr_sec, curr_pos_in_sec);

  while (left > 0 && f->pos < f->size) {
    //read in sector, write to buffer, update left and pos
    if (Disk_Read(child->data[curr_sec], data_buf) < 0) {
      return(-1);
    }
    int to_read = 0;
    if (left + curr_pos_in_sec > SECTOR_SIZE) {
      to_read = SECTOR_SIZE - curr_pos_in_sec;
    }else{
      to_read = left;
    }
    memcpy(buffer, data_buf + curr_pos_in_sec, to_read);

    left           -= to_read;
    curr_pos_in_sec = 0;
    f->pos         += to_read;
    curr_sec       += 1;
    out_pos        += to_read;
    if (f->pos > f->size) {
      f->pos = f->size;
    }
  }
  return(out_pos);
}

//Written by Dario Gonzalez

/*
 * File_Write() should write size bytes from buffer and write them into the file referenced by fd. All
 * writes should begin at the current location of the file pointer and the file pointer should be updated
 * after the write to its current location plus size. Note that writes are the only way to extend the size
 * of a file. If the file is not open, return -1 and set osErrno to E_BAD_FD. Upon success of the write,
 * all the data should be written out to disk and the value of size should be returned. If the write cannot
 * complete (due to a lack of space on disk), return -1 and set osErrno to E_NO_SPACE. Finally, if
 * the file exceeds the maximum file size, you should return -1and set osErrno to E_FILE_TOO_BIG
 */
int File_Write(int fd, void *buffer, int size) {
  if (!is_file_open(fd)) {
    dprintf("tried to write to file that wasn't open\n");
    osErrno = E_BAD_FD;
    return(-1);
  }
  open_file_t *f = &open_files[fd];
  if (f->pos + size > MAX_SECTORS_PER_FILE * SECTOR_SIZE) {
    dprintf("tried to write too much to a file\n");
    osErrno = E_FILE_TOO_BIG;
    return(-1);
  }

  //Taken from File_Open
  // load the disk sector containing the inode
  int  inode_sector = INODE_TABLE_START_SECTOR + f->inode / INODES_PER_SECTOR;
  char inode_buffer[SECTOR_SIZE];
  if (Disk_Read(inode_sector, inode_buffer) < 0) {
    osErrno = E_GENERAL; return(-1);
  }
  dprintf("... load inode table for inode from disk sector %d\n", inode_sector);

  // get the inode
  int inode_start_entry = (inode_sector - INODE_TABLE_START_SECTOR) * INODES_PER_SECTOR;
  int offset            = f->inode - inode_start_entry;
  assert(0 <= offset && offset < INODES_PER_SECTOR);
  inode_t *child = (inode_t *)(inode_buffer + offset * sizeof(inode_t));
  //Done taking from File_Open

  int allocated_secs = (f->size + SECTOR_SIZE - 1) / SECTOR_SIZE;
  int needed_secs    = (size - (allocated_secs * SECTOR_SIZE - f->pos) + SECTOR_SIZE - 1) / SECTOR_SIZE;
  for (int i = allocated_secs; i < allocated_secs + needed_secs; i++) {
    int next = bitmap_first_unused(SECTOR_BITMAP_START_SECTOR, SECTOR_BITMAP_SECTORS, SECTOR_BITMAP_SIZE);
    dprintf("assigning block %d to file for writing\n", next);
    if (next < 0) {
      dprintf("disk ran out of space when allocating blocks to write\n");
      osErrno = E_NO_SPACE;
      return(-1);
    }
    child->data[i] = next;
  }
  child->size += size;
  if (Disk_Write(inode_sector, inode_buffer) < 0) {
    return(-1);
  }

  allocated_secs += needed_secs;
  f->size         = f->pos + size;
  child->size     = f->size;

  int  left            = size;
  int  curr_pos_in_sec = f->pos % SECTOR_SIZE;
  int  curr_sec        = f->pos / SECTOR_SIZE;
  int  in_pos          = 0;
  char data_buf[SECTOR_SIZE];

  //write out while still in allocated secs
  while (left > 0 && curr_sec < allocated_secs) {
    //read in sector, write to buffer, update left and pos
    if (Disk_Read(child->data[curr_sec], data_buf) < 0) {
      return(-1);
    }
    int to_write = 0;
    if (left > SECTOR_SIZE) {
      to_write = SECTOR_SIZE - curr_pos_in_sec;
    }else{
      to_write = left - curr_pos_in_sec;
    }
    memcpy(data_buf + curr_pos_in_sec, buffer + in_pos, to_write);
    if (Disk_Write(child->data[curr_sec], data_buf) < 0) {
      return(-1);
    }

    left           -= to_write;
    curr_pos_in_sec = 0;
    f->pos         += to_write;
    curr_sec       += 1;
    in_pos         += to_write;
  }
  return(size - left);
}

//Written by Dario Gonzalez
int File_Seek(int fd, int offset) {
  if (!is_file_open(fd)) {
    osErrno = E_BAD_FD;
    return(-1);
  }
  open_file_t *f = &open_files[fd];
  if (offset > f->size || offset < 0) {
    osErrno = E_SEEK_OUT_OF_BOUNDS;
    return(-1);
  }
  f->pos = offset;
  return(0);
}

int File_Close(int fd) {
  dprintf("File_Close(%d):\n", fd);
  if (0 > fd || fd > MAX_OPEN_FILES) {
    dprintf("... fd=%d out of bound\n", fd);
    osErrno = E_BAD_FD;
    return(-1);
  }
  if (open_files[fd].inode <= 0) {
    dprintf("... fd=%d not an open file\n", fd);
    osErrno = E_BAD_FD;
    return(-1);
  }

  dprintf("... file closed successfully\n");
  open_files[fd].inode = 0;
  return(0);
}

int Dir_Create(char *path) {
  dprintf("Dir_Create('%s'):\n", path);
  return(create_file_or_directory(1, path));
}

//Written by Marcelo Valencia

/*
 * Dir_Unlink() removes a directory referred to by path, freeing up its inode and data blocks, and
 * removing its entry from the parent directory. Upon success, return 0. If the directory does not
 * currently exist, return -1 and set osErrno to E_NO_SUCH_DIR. Dir_Unlink() should only be
 * successful if there are no files within the directory. If there are still files within the directory, return
 * -1 and set osErrno to E_DIR_NOT_EMPTY. It’s not allowed to remove the root directory ("/"),
 * in which case the function should return -1 and set osErrno to E_ROOT_DIR.
 */
int Dir_Unlink(char *path) {
  /* YOUR CODE */
  char *rootPath = "/";

  if (strcmp(rootPath, path) == 0) {
    osErrno = E_ROOT_DIR;
    return(-1);
  }
  char path_name[255];
  int  child_inode;
  int  parent_inode = follow_path(path, &child_inode, path_name);
  if (parent_inode < 0) {
    osErrno = E_NO_SUCH_DIR;
    return(-1);
  }

  int success = remove_inode(1, parent_inode, child_inode);
  if (success < 0) {
    dprintf("...DIR not empty %d\n", success);
    osErrno = E_DIR_NOT_EMPTY;
    return(-1);
  }
  printf("...DIR unlinked\n");
  return(0);
}

//Written by Marcelo Valencia

/*
 * Dir_Size() returns the number of bytes in the directory referred to by path. This function should be
 * used to find the size of the directory before calling Dir_Read() (described below) to find the
 * contents of the directory.
 */
int Dir_Size(char *path) {
  /* YOUR CODE */
  char child_name[16]; child_name[15] = '\0';
  int  child_node;
  int  parent_node = follow_path(path, &child_node, child_name);

  dprintf("Dir_Size: followed path\n");
  if (parent_node < 0) {
    osErrno = E_NO_SUCH_DIR;
    return(-1);
  }
  int  inode_sector = INODE_TABLE_START_SECTOR + child_node / INODES_PER_SECTOR;
  char inode_buffer[SECTOR_SIZE];

  Disk_Read(inode_sector, inode_buffer);
  int      offset = child_node % INODES_PER_SECTOR;
  inode_t *child  = (inode_t *)(inode_buffer + offset * sizeof(inode_t));
  return(child->size * sizeof(dirent_t));
  //return 0;
}

//Written by Dario Gonzalez

/*
 * Dir_Read() can be used to read the contents of a directory. It should return in the buffer a set of
 * directory entries. Each entry is of size 20 bytes and contains 16-byte names of the files (or
 * directories) within the directory named by path, followed by the 4-byte integer inode number.
 * If size is not big enough to contain all the entries, return -1 and set osErrno to
 * E_BUFFER_TOO_SMALL. Otherwise, read the data into the buffer, and return the number of
 * directory entries that are in the directory (e.g., 2 if there are two entries in the directory).
 */
int Dir_Read(char *path, void *buffer, int size) {
  char child_name[16]; child_name[15] = '\0';
  int  child_node;
  int  parent_node = follow_path(path, &child_node, child_name);

  dprintf("Dir_Read: followed path\n");
  if (parent_node < 0) {
    osErrno = E_NO_SUCH_DIR;
    return(-1);
  }
  int  inode_sector = INODE_TABLE_START_SECTOR + child_node / INODES_PER_SECTOR;
  char inode_buffer[SECTOR_SIZE];

  if (Disk_Read(inode_sector, inode_buffer) < 0) {
    return(-1);
  }
  int      offset      = child_node % INODES_PER_SECTOR;
  inode_t *child       = (inode_t *)(inode_buffer + offset * sizeof(inode_t));
  int      num_entries = child->size;
  if (size < child->size * sizeof(dirent_t)) {
    osErrno = E_BUFFER_TOO_SMALL;
    return(-1);
  }

  int out_pos = 0;
  //now we need to read potentially many sectors into buffer
  //first copy all dirents in full sectors
  char sec_buf[SECTOR_SIZE];
  for (int i = 0; i < child->size / DIRENTS_PER_SECTOR; i++) {
    if (Disk_Read(child->data[i], sec_buf) < 0) {
      return(-1);
    }
    memcpy(buffer + out_pos, sec_buf, DIRENTS_PER_SECTOR * sizeof(dirent_t));
    out_pos += DIRENTS_PER_SECTOR * sizeof(dirent_t);
  }
  //copy over the last, partially filled sector
  int left = child->size % DIRENTS_PER_SECTOR;
  if (left > 0) {
    if (Disk_Read(child->data[child->size / DIRENTS_PER_SECTOR], sec_buf) < 0) {
      return(-1);
    }
    memcpy(buffer + out_pos, sec_buf, left * sizeof(dirent_t));
  }
  return(child->size);
}
