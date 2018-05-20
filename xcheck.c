#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdint.h>
#include <assert.h>

// On-disk file system format.
// Both the kernel and user programs use this header file.

// Block 0 is unused.
// Block 1 is super block.
// Inodes start at block 2.

#define ROOTINO 1                                      // root i-number
#define BSIZE 512                                      // block size
#define T_DIR  1                                       // Directory
#define T_FILE 2                                       // File
#define T_DEV  3                                       // Device
#define IPB           (BSIZE / sizeof(dinode_t))       // Inodes per block.
#define IBLOCK(i)     ((i) / IPB + 2)                  // Block containing inode i
#define BPB           (BSIZE*8)                        // Bitmap bits per block
#define BBLOCK(b, ninodes) (b/BPB + (ninodes)/IPB + 3) // Block containing bit for block b
#define DIRSIZ 14                                      // Directory is a file containing a sequence of dirent structures.
#define NDIRECT 12
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT)
 
const char* IMAGE_NOT_FOUND  = "image not found.";
const char* ERR_INODE        = "ERROR: bad inode.";
const char* ERR_ADDR_DIR     = "ERROR: bad direct address in inode.";
const char* ERR_ADDR_IND     = "ERROR: bad indirect address in inode.";
const char* ERR_ROOT_DIR     = "ERROR: root directory does not exist.";
const char* ERR_DIR_FMT      = "ERROR: directory not properly formatted.";
const char* ERR_MKD_FREE     = "ERROR: address used by inode but marked free in bitmap.";
const char* ERR_MKD_USED     = "ERROR: bitmap marks block in use but it is not in use.";
const char* ERR_ADDR_DIR_DUP = "ERROR: direct address used more than once.";
const char* ERR_ADDR_IND_DUP = "ERROR: indirect address used more than once.";
const char* ERR_ITBL_USED    = "ERROR: inode marked use but not found in a directory.";
const char* ERR_ITBL_FREE    = "ERROR: inode referred to in directory but marked free.";
const char* ERR_FILE_REF     = "ERROR: bad reference count for file.";
const char* ERR_DIR_REF      = "ERROR: directory appears more than once in file system.";

// File system super block
struct superblock_s {
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
} typedef superblock_t;

// On-disk inode structure
struct dinode_s {
  short type;              // File type
  short major;             // Major device number (T_DEV only)
  short minor;             // Minor device number (T_DEV only)
  short nlink;             // Number of links to inode in file system
  uint size;               // Size of file (bytes)
  uint addrs[NDIRECT+1];   // Data block addresses {if type=DIR, each [i] points to dirent}
} typedef dinode_t;

// Directory entry
struct dirent_s {
  ushort inum;
  char name[DIRSIZ];
} typedef dirent_t;

void error(const char*);

void *image;
superblock_t *sb;
dinode_t *dip;
char * bitmap;
void *dp;

int in_use(int val, int *arr, int size){
  for (int i = 0; i < size; i++) {
    if (arr[i] == val)
      return 1;
    if (arr[i] == -1)
      return 0;
    //printf("%d ", arr[i]);
  }
  return 0;
}

void mark_block_used(superblock_t *sb, int * used, int block, int direct_flag) {
  // Linear search
  for(int i = 0; i < sb->nblocks; i++) {
    if(used[i] == -1) { // Add it to the list
      used[i] = block;
      break;
    }
    else if(used[i] == block) {
      if(direct_flag)
	error(ERR_ADDR_DIR_DUP);
      else
	error(ERR_ADDR_IND_DUP);
    }
  }
}

int get_bitmap_val(int block) {
  int subset;
  memcpy(&subset, (image + BSIZE * BBLOCK(0, sb->ninodes)) + (block / 8) , sizeof(int));
  return (subset >> (block % 8)) & 1;
}

void valid_addr(char *new_bm, uint addr) {
  if (addr == 0)
    return;

  // address OOB
  if (addr < (sb->ninodes / IPB + sb->nblocks / BPB + 4) || addr >= (sb->ninodes / IPB + sb->nblocks / BPB + 4 + sb->nblocks))
    error(ERR_ADDR_DIR);

  // in use, marked free
  char b = *(bitmap + addr / 8);
  if (!((b >> (addr % 8)) & 0x01))
    error(ERR_MKD_FREE);
}

int get_address(int offset, dinode_t *curr_dip, int flag) {
  if (offset / BSIZE <= NDIRECT && !flag)
    return curr_dip->addrs[offset / BSIZE];
  else 
    return *((int*) (image + curr_dip->addrs[NDIRECT] * BSIZE) + offset / BSIZE - NDIRECT);
}

// compare bitmaps - ret 1 when diff, 0 when same
int compare_bm(char *bm1, char *bm2, int sz) {
  for (int i = 0; i < sz; i++)
    if (*(bm1++) != *(bm2++))
      return 1;
  return 0;
}

// Reads a whole 512 byte datablock of dirents
void mark_inodes_referenced(int * inode_reference_bitmap, int addr) {
  for(int i = 0; i < BSIZE; i += sizeof(dirent_t)) {
    dirent_t d;
    memcpy(&d, image + addr + i, sizeof(dirent_t));
    if(d.inum > sb->ninodes)
      break; // null ref
    printf("ref: %d\n", d.inum);
    inode_reference_bitmap[d.inum] += 1;
  }
}

int main(int argc, char *argv[]) {
  int fd = open(argv[1], O_RDONLY);
  if (fd == -1)
    error(IMAGE_NOT_FOUND);

  int rc;
  struct stat stats;
  rc = fstat(fd, &stats);
  assert(rc == 0);

  image = mmap(0, stats.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  assert(image != MAP_FAILED); 

  sb = (superblock_t *) (image + BSIZE);
  
  // inode
  int i;
  dip    = (dinode_t *) (image + 2 * BSIZE);

  bitmap = (char *) (image + (sb->ninodes / IPB + 3) * BSIZE);
  dp     = (void *) (image + (sb->ninodes/IPB + sb->nblocks / BPB + 4) * BSIZE);

  int bm_size = (sb->nblocks + sb->ninodes / IPB + sb->nblocks / BPB + 4) / 8;
  int offset  = sb->ninodes / IPB + sb->nblocks / BPB + 4;
  int inode_ref[sb->ninodes + 1];
  memset(inode_ref, 0, (sb->ninodes + 1) * sizeof(int));
  char new_bm[bm_size];

  int dir_used[sb->nblocks], indir_used[sb->nblocks], direct_reference_bitmap[sb->ninodes], indirect_reference_bitmap[sb->ninodes];
  for(i = 0; i < sb->nblocks; i++) {
    dir_used[i]   = -1;
    indir_used[i] = -1;
  }
  for(i = 0; i < sb->ninodes - 1; i++) {
    direct_reference_bitmap[i]    = 0;
    indirect_reference_bitmap[i]  = 0;
  }
  
  memset(new_bm, 0, bm_size);
  memset(new_bm, 0xFF, offset / 8);
  char last = 0x00;
  for (i = 0; i < offset % 8; i++)
    last = (last << 1) | 0x01;
  new_bm[offset / 8] = last;

  // Check root
  dinode_t *root = dip + ROOTINO;
  if (!(dip + ROOTINO) || (dip + ROOTINO)->type != T_DIR) 
    error(ERR_ROOT_DIR);
  
  dirent_t root_parent;
  memcpy(&root_parent, image + BSIZE * root->addrs[0] + sizeof(dirent_t) , sizeof(dirent_t));
  if(root_parent.inum != ROOTINO)
    error(ERR_ROOT_DIR);
  
  //dfs(inode_ref, new_bm, ROOTINO, ROOTINO);

  dinode_t *curr_dip = dip;

  for (i = 1; i < sb->ninodes; i++) {
    curr_dip++;
    if (curr_dip->type == 0) 
      continue; // unalloc

    //    printf("[%d] - %d\n", i, curr_dip->type);
    // Invalid types
    if (curr_dip->type != T_FILE && curr_dip->type != T_DIR && curr_dip->type != T_DEV)
      error(ERR_INODE);
    
    // Direct Address in bounds check
    for(int j = 0; j < NDIRECT; j++) {
      if(curr_dip->addrs[j] == 0)
	break;
      if(curr_dip->addrs[j] >= sb->size || curr_dip->addrs[j] < BBLOCK(0, sb->ninodes))
	error(ERR_ADDR_DIR);
      if(!get_bitmap_val(curr_dip->addrs[j])) // Checking to make sure bitmask says this is valid
	error(ERR_MKD_FREE);
      mark_block_used(sb, dir_used, curr_dip->addrs[j], 1); // Marking block as used
      if(curr_dip->type == T_DIR) {
	mark_inodes_referenced(direct_reference_bitmap, curr_dip->addrs[j] * BSIZE); // Tracking the directories this points to
      }
    }
    
    // Indirect Address in bounds check
    if(curr_dip->addrs[NDIRECT] != 0) {
      int indirect_block_start = BSIZE * curr_dip->addrs[NDIRECT], curr_block;
      mark_block_used(sb, indir_used, curr_dip->addrs[NDIRECT], 0); // Marking block as used
      for(int j = 0; j < NINDIRECT; j++) {
	memcpy(&curr_block, image + indirect_block_start + 4*j, sizeof(int));
	if(curr_block == 0)
	  break;
	if(curr_block >= sb->size || curr_block < BBLOCK(0, sb->ninodes))
	  error(ERR_ADDR_IND);
	if(!get_bitmap_val(curr_block))
	  error(ERR_MKD_FREE);
	mark_block_used(sb, indir_used, curr_block, 0); // Marking block as used
	if(curr_dip->type == T_DIR) {
	  mark_inodes_referenced(indirect_reference_bitmap, curr_block * BSIZE); // Tracking the directories this points to
	}
      }
    }

    // Checking directory format for .. and .
    if(curr_dip->type == T_DIR) {
      dirent_t parent, self;
      memcpy(&self  , image + BSIZE * curr_dip->addrs[0]                   , sizeof(dirent_t));
      memcpy(&parent, image + BSIZE * curr_dip->addrs[0] + sizeof(dirent_t), sizeof(dirent_t));

      if(parent.inum == 0 || self.inum == 0 || strcmp(parent.name, "..") || strcmp(self.name, ".") || self.inum != i)
	error(ERR_DIR_FMT);
    }
  }

  // Making sure everything that is marked as used is actually used
  for(int i = BBLOCK(0, sb->ninodes) + 1; i < sb->nblocks; i++) { // Skipping superblock and stuff
    if(get_bitmap_val(i) && !(in_use(i, dir_used, sb->size) || in_use(i, indir_used, sb->size))) 
      error(ERR_MKD_USED);
    
  }
  /**
  for (i = 0; i < sb->ninodes; i++) {
    printf("%d", inode_reference_bitmap[i]);
    if(i % 20 == 0)
      printf("\n");
  }
  printf("\n");
  **/
  curr_dip = dip;
  for (i = 1; i < sb->ninodes; i++) {
    curr_dip++;
    if(curr_dip->type != 0 && direct_reference_bitmap[i] == 0 && indirect_reference_bitmap[i] == 0)
      error(ERR_ITBL_USED);
    if(curr_dip->type == 0 && direct_reference_bitmap[i] + indirect_reference_bitmap[i] > 0)
      error(ERR_ITBL_FREE);
    if(curr_dip->type == T_FILE && curr_dip->nlink != direct_reference_bitmap[i] + indirect_reference_bitmap[i])
      error(ERR_FILE_REF);
    if(i != ROOTINO && curr_dip->type == T_DIR && curr_dip->nlink > 1)
      error(ERR_DIR_REF);
    if(i != ROOTINO && curr_dip->type == T_DIR && indirect_reference_bitmap[i] > 1) {
      printf("%d\n", i);
     error(ERR_DIR_REF);
    }
  }
  return 0;
}

void error(const char* err) {
  fprintf(stderr, "%s\n", err);
  exit(1);
}
