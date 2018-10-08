#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <assert.h>
#include "ext2.h"
#include "util.h"
#include <errno.h>
#include <libgen.h>
#include <time.h>
#define BLOCK_OFFSET(block) (1024 + (block-1)*EXT2_BLOCK_SIZE)
unsigned char *disk;

int update_block_bitmap(unsigned char *disk, int *block_bitmap, int *block_assign, int block_need);
int update_inode_bitmap(unsigned char *disk, int *inode_bitmap, int src_inode_num);
int update_superblock(struct ext2_super_block *sb, int block_need);
int update_groupdesrc(struct ext2_group_desc *gd, int block_need);
int update_dir_entry(struct ext2_inode *src_par_inode, char *src_base);
int path_walk(unsigned char *disk, char *dest, char *src_base, struct ext2_group_desc *gd);


int main(int argc, char **argv) {
  char src[4096];
  char src_par_path[4096];
  char *src_base;
  int *inode_bitmap;
	int *block_bitmap;

  if (argc != 3) {
    fprintf(stderr, "Usage: ext2_rm <image file name><file path>\n");
		exit(1);
  }

  if (argv[2][0] != '/') {
   fprintf(stderr, "Need an absolute path starting with /\n");
   exit(1);
  }

  // Open virtual disk image
	int fd = open(argv[1], O_RDWR);
	disk = mmap(NULL, 128 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (disk == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}

  // Get source path and base name
  strcpy(src, argv[2]);
  src_base = basename(src);

  // Get the absolute parent path of source
  int par_len = strlen(src) - strlen(src_base);
  get_parent_path(src_par_path, src, par_len);

  // Get super block, group descriptor and inode table
  struct ext2_super_block *sb = (struct ext2_super_block *) (disk + 1024);
  struct ext2_group_desc *gd = (struct ext2_group_desc *) (disk + 1024 +
                                sizeof(struct ext2_super_block));
  char *inode_table;
  inode_table = (char *) (disk + BLOCK_OFFSET(gd->bg_inode_table));


  // Store inode bitmap and block bitmap
	inode_bitmap = get_inode_bitmap(disk, gd);
	block_bitmap = get_block_bitmap(disk, gd);

  // Check target path validity, target file must exist,
  // get inode num of given arguement if it is valid

  int src_par_inode_num = path_walk(disk, src_par_path, src_base, gd);
  struct ext2_inode *src_par_inode = (struct ext2_inode *) (inode_table +
                              (src_par_inode_num-1)*sizeof(struct ext2_inode));
  int src_inode_num = check_src_existance(src_par_inode, src_base);
  struct ext2_inode *src_inode =
  (struct ext2_inode *) (inode_table + (src_inode_num-1)*sizeof(struct ext2_inode));

  // Get the usage of the file
  int block_need = src_inode->i_blocks/2;


  int block_assign[block_need];
  int cnt;
  for (cnt = 0; cnt < block_need; cnt++) {
    if (cnt < 12) {
      block_assign[cnt] = src_inode->i_block[cnt];
    }
    if (cnt == 12) {
      block_assign[cnt] = src_inode->i_block[cnt];
    }
    else if (cnt > 12) {
      block_assign[cnt] =
      *(disk + BLOCK_OFFSET(block_assign[12]) + (cnt-13)*sizeof(unsigned int));
    }

  }

  src_inode->i_links_count -= 1;
  // Need to free blocks and update bit map
  if (src_inode->i_links_count == 0) {
    update_block_bitmap(disk, block_bitmap, block_assign, block_need);
    update_inode_bitmap(disk, inode_bitmap, src_inode_num);
    update_superblock(sb, block_need);
    update_groupdesrc(gd, block_need);
    src_inode->i_size = 0;
    src_inode->i_blocks = 0;
  }
  // Update delete time
  time_t t = time(NULL);
  unsigned int time = t;
  src_inode->i_dtime = time;

  update_dir_entry(src_par_inode, src_base);

  // free allocated space
  free(block_bitmap);
  free(inode_bitmap);

  return 0;
}


int update_dir_entry(struct ext2_inode *src_par_inode, char *src_base) {
  int find = 0;
  int size;
  int cnt = 0;
  int block_num = src_par_inode->i_block[cnt];
  struct ext2_dir_entry_2 *curr_entry;
  struct ext2_dir_entry_2 *last_entry;
  while (block_num != 0) {
    curr_entry = (struct ext2_dir_entry_2 *) (disk + BLOCK_OFFSET(block_num));
    while (size < 1024 && curr_entry->inode != 0 && curr_entry->inode != 0) {
      if (namecmp(curr_entry->name, curr_entry->name_len, src_base) == 0) {
        find = 1;
        break;
      }
      size += curr_entry->rec_len;
      last_entry = curr_entry;
      curr_entry = (void *) curr_entry + curr_entry->rec_len;
    }
    if (find == 1) {
      break;
    }
    cnt += 1;
    block_num = src_par_inode->i_block[cnt];
  }
  if (find == 0) {
    fprintf(stderr, "Cannot find file under parent file\n");
    exit(ENOENT);
  }
  // now curr_entry corresponds to the file need to remove
  // and last_entry corresponds to the file immediately in front of it.
  // Update the rec_len of last_entry
  last_entry->rec_len += curr_entry->rec_len;

  return 0;
}


int update_block_bitmap(unsigned char *disk, int *block_bitmap, int *block_assign, int block_need) {
	struct ext2_group_desc *gd = (struct ext2_group_desc *) (disk + 1024 +
																sizeof(struct ext2_super_block));
  int m;
  int i;
  int j;
  for (m = 0; m < block_need; m++) {
    for (i = 0; i < 16; i++) {
      for (j = 0; j < 8; j++) {
        if ((8*i + j) == block_assign[m]) {
          *(disk+1024+(gd->bg_block_bitmap-1)*1024+i) &= ~(0x01 << j);
          block_bitmap[8*i + j] = 0;
        }
      }
    }
  }
  return 0;
}

int update_inode_bitmap(unsigned char*disk, int *inode_bitmap, int src_inode_num) {
  struct ext2_group_desc *gd = (struct ext2_group_desc *) (disk + 1024 +
																sizeof(struct ext2_super_block));
  inode_bitmap[src_inode_num-1] = 0;
  int i, j;
  for (i = 0; i < 4; i++) {
    for (j = 0; j < 8; j++) {
      if ((8*i + j) == src_inode_num - 1) {
        *(disk+1024+(gd->bg_inode_bitmap-1)*1024+i) &= ~(0x01 << j);
      }
    }
  }
  return 0;
}


int update_superblock(struct ext2_super_block *sb, int block_need) {
  int i;
  for (i = 0; i < block_need; i++) {
    sb->s_free_blocks_count += 1;
  }
  sb->s_free_inodes_count += 1;
  return 0;
}

int update_groupdesrc(struct ext2_group_desc *gd, int block_need) {
  int i;
  for (i = 0; i < block_need; i++) {
    gd->bg_free_blocks_count += 1;
  }
  gd->bg_free_inodes_count += 1;
  return 0;
}

/* Helper function to walk through path and
 * check if the target dir already exists.
 * Return the inode number of dest if the given dest path is valid
 */
int path_walk(unsigned char *disk, char *dest, char *src_base, struct ext2_group_desc *gd) {

 	char *inode_table;
 	inode_table = (char *) (disk + BLOCK_OFFSET(gd->bg_inode_table));
 	// Start from the root inode;
 	struct ext2_inode *inode = (struct ext2_inode *) (inode_table +
 	                                                 1*sizeof(struct ext2_inode));
  int inode_num = 2;
 	char *part = malloc(sizeof(char) * 255);
 	char *delim = "/";
 	part = strtok(dest, delim);
 	// Do path walk
 	int find;
  int size;
  int block_count;
  unsigned int block_num;
  struct ext2_dir_entry_2 *entry;
  while (part != NULL) {

    find = 0;
    for (block_count = 0; block_count <= 12; block_count++) {
      size = 0;
      block_num = inode->i_block[block_count];
      entry = (struct ext2_dir_entry_2 *) (disk + BLOCK_OFFSET(block_num));
      // We are handling direct block
      while (size < inode->i_size && entry->rec_len != 0 && entry->inode != 0) {

        // Current entry is NOT a dir.
        if (entry->file_type != 2) {
          size += entry->rec_len;
          entry = (void *)entry + entry->rec_len;
          continue;
        }

        // Current entry is a directory.
        else if (entry->file_type == 2) {
          if (namecmp(entry->name, entry->name_len, part) != 0) {
 					  size += entry->rec_len;
 						entry = (void *)entry + entry->rec_len;
            continue;
          }
          else if (namecmp(entry->name, entry->name_len, part) == 0) {
            find = 1;
            inode = (struct ext2_inode *) (inode_table +
                       (entry->inode-1)*sizeof(struct ext2_inode));
            inode_num = entry->inode;
            break;
          }
        }
      }
      if (find == 1) {
        // Find the target, No need to search rest blocks in current directory
        break;
      }
    }
    // Finish checking all the blocks

    if (find == 0) {
      fprintf(stderr, "Destination does not exist\n");
      exit(ENOENT);
    }
    part = strtok(NULL, delim);
  }

  if (find == 0) {
    fprintf(stderr, "No such deletion targer\n");
    exit(ENOENT);
  }

 	free(part);
 	return inode_num;
 }
