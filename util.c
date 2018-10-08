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
#include <errno.h>
#include <libgen.h>
#define BLOCK_OFFSET(block) (1024 + (block-1)*EXT2_BLOCK_SIZE)
unsigned char *disk;

/* Helper function to get the parent path */
int get_parent_path(char *par_path, char *dest, int par_len) {
  int i;
  for (i = 0; i < par_len; i++) {
    par_path[i] = dest[i];
  }
  par_path[par_len] = '\0';

  return 0;
}

int *get_inode_bitmap(unsigned char *disk, struct ext2_group_desc *gd) {
	int *inode_bitmap = malloc(sizeof(int) * 32);
	int i, j;
	for (i = 0; i < 4; i++) {
		for (j = 0; j < 8; j++) {
			// right shift 1 bit and store into inode_bitmap
			*(inode_bitmap + 8*i + j)
			= *(disk + BLOCK_OFFSET(gd->bg_inode_bitmap) + i) >> j & 1;
		}
	}
	return inode_bitmap;
}

int *get_block_bitmap(unsigned char *disk, struct ext2_group_desc *gd) {
	int *block_bitmap = malloc(sizeof(int) * 128);
	int i, j;
	for (i = 0; i < 16; i++) {
		for (j = 0; j < 8; j++) {
			// right shift 1 bit and store into inode_bitmap
			*(block_bitmap + 8*i + j)
			= *(disk + BLOCK_OFFSET(gd->bg_block_bitmap) + i) >> j & 1;
		}
	}
	return block_bitmap;
}


/* Helper function to get a free inode */
int new_inode_search(unsigned char *disk, int* inode_bitmap,
										 struct ext2_super_block *sb, struct ext2_group_desc *gd) {

	int new_inode;
	int i;
	int m, n;
	// A flag to check if inode bitmap is updated
	int inode_updated = 0;
	for (i = 11; i < 32; i++) {
		if (inode_bitmap[i] == 0) {
			inode_bitmap[i] = 1;
			for (m = 0; m < 4; m++) {
				for (n = 0; n < 8; n++) {
					// Find the corresponding bit in inode bitmap and update it
					if (m*8 + n == i) {
						*(disk+1024+(gd->bg_inode_bitmap-1)*1024 + m) |= (0x01 << n);
						inode_updated = 1;
						break;
					}
				}
				if (inode_updated == 1) {
					break;
				}
			}
		}
		if (inode_updated == 1) {
			new_inode = i;
			break;
		}
	}
	// Update free inode count in block group descriptor and
	// free inode count in super block
	sb->s_free_inodes_count--;
	gd->bg_free_inodes_count--;

	// Remember inode starts from 1
	return (new_inode + 1);
}

/* Helper function to get a free block */
int new_block_search(unsigned char *disk, int* block_bitmap,
	struct ext2_super_block *sb, struct ext2_group_desc *gd) {

	int new_block = -1;
	int i;
	int m, n;
	// A flag to check if block bitmap is updated
	int block_updated = 0;
	for (i = 0; i < 128; i++) {
		if (block_bitmap[i] == 0) {
			block_bitmap[i] = 1;
			for (m = 0; m < 16; m++) {
				for (n = 0; n < 8; n++) {
					// Find the corresponding bit in inode bitmap and update it
					if (m*8 + n == i) {
						*(disk+1024+(gd->bg_block_bitmap-1)*1024 + m) |= (0x01 << n);
						block_updated = 1;
						break;
					}
				}
				if (block_updated == 1) {
					break;
				}
			}
		}
		if (block_updated == 1) {
			new_block = i;
			break;
		}
	}

	if (new_block == -1) {
		fprintf(stderr, "No free block left\n");
		exit(1);
	}
	// Update free inode count in block group descriptor
	// and free inode count in super block
	sb->s_free_blocks_count--;
	gd->bg_free_blocks_count--;

	return new_block;
}


int assign_name(char *name_field, char *base) {
	int len = strlen(base);
	int padding = 4 - (len % 4);
	int i;
	for (i = 0; i < len; i++){
		name_field[i] = base[i];
	}
	if (padding > 0) {
		int j;
		for (j = len; j < len+padding; j++) {
			name_field[j] = '\0';
		}
	}
	return 0;
}

int print_name(char *name_field, int len) {
  int i;
  for (i = 0; i < len; i++) {
    printf("%c", name_field[i]);
  }
  printf("\n");
  return 0;
}

int namecmp(char *name_field, int name_field_len, char *base) {
  int len = strlen(base);
  if (len != name_field_len) {
    return -1;
  }
  int i;
  for (i = 0; i < len; i++) {
    if (name_field[i] == base[i]) {
      continue;
    }
    return -1;
  }
  return 0;
}

/* copy the path to new symbolic link*/
int copy_linkpath(unsigned char *disk, char *src_path, int block_need, int link_size,
              int *block_assign, struct ext2_inode *new_link_inode) {
  int i;
  int remain = link_size;
  int block_idx;
  for (i = 0; i < block_need; i++) {
    // copying to direct block
    block_idx = block_assign[i];
    if (remain < EXT2_BLOCK_SIZE) {
      memcpy(disk + BLOCK_OFFSET(block_idx), src_path + i*EXT2_BLOCK_SIZE, remain);
    }
    else {
      memcpy(disk + BLOCK_OFFSET(block_idx), src_path + i*EXT2_BLOCK_SIZE, EXT2_BLOCK_SIZE);
      remain -= EXT2_BLOCK_SIZE;
    }
    new_link_inode->i_block[i] = block_idx;
  }
    // no need to handle indirect block since length of path is at most 4096
  return 0;
}


/* Set up the dir entry in block assigned for new dir */
int set_up_new_block(unsigned char *disk, int block_assign,
                      int new_inode_num, int parent_inode_num) {
  struct ext2_dir_entry_2 *entry =
	(struct ext2_dir_entry_2 *) (disk + BLOCK_OFFSET(block_assign));
  // initialize .
  entry->inode = new_inode_num;
  entry->rec_len = 12;
  entry->name_len = 1;
  entry->file_type = EXT2_FT_DIR;
  strcpy(entry->name, ".");
  // initialize ..
  entry = (void *) entry + entry->rec_len;
  entry->inode = parent_inode_num;
  entry->rec_len = 1012;
  entry->name_len = 2;
  entry->file_type = EXT2_FT_DIR;
  strcpy(entry->name, "..");

  return 0;
}

void copy(unsigned char *disk, unsigned char *src, int block_need, int fsize,
					int *block_assign, struct ext2_inode *new_file_inode) {
	int i;
	int remain = fsize;
	int block_idx;
	int indirect_block;
	for (i = 0; i < block_need; i++) {
		// copying to direct block
		if (i < 12) {
			block_idx = block_assign[i];
			if (remain < EXT2_BLOCK_SIZE) {
				memcpy(disk + BLOCK_OFFSET(block_idx), src + i*EXT2_BLOCK_SIZE, remain);
			}
			else {
				memcpy(disk + BLOCK_OFFSET(block_idx), src + i*EXT2_BLOCK_SIZE, EXT2_BLOCK_SIZE);
				remain -= EXT2_BLOCK_SIZE;
			}
			new_file_inode->i_block[i] = block_idx;
		}
		// assigne single indrect block
		else if (i == 12) {
			indirect_block = block_assign[i];
			new_file_inode->i_block[i] = indirect_block;
		}
		// copy to indirect_block
		else if (i > 12) {
			block_idx = block_assign[i];
			if (remain < EXT2_BLOCK_SIZE) {
				memcpy(disk + BLOCK_OFFSET(block_idx), src + (i-1)*EXT2_BLOCK_SIZE, remain);
			}
			else {
				memcpy(disk + BLOCK_OFFSET(block_idx), src + (i-1)*EXT2_BLOCK_SIZE, EXT2_BLOCK_SIZE);
				remain -= EXT2_BLOCK_SIZE;
			}
			*(disk+BLOCK_OFFSET(indirect_block)+(i-13)*sizeof(unsigned int)) =
			(unsigned int)block_idx;
		}
	}
}


/* If dest link exists under its parent directory, return EEXIST*/
int check_dest_existance (struct ext2_inode *inode, char *dest_base) {
  int block_count;
  int size;
  int block_num;
  struct ext2_dir_entry_2 * entry;
  // Inode points to parent inode, check if dest link already exits
 	for (block_count = 0; block_count < 13; block_count++) {
    size = 0;
    block_num = inode->i_block[block_count];
    entry = (struct ext2_dir_entry_2 *) (disk + BLOCK_OFFSET(block_num));
    if (block_count < 12) {
      while (size < inode->i_size && entry->rec_len != 0) {
        if (namecmp(entry->name, entry->name_len, dest_base) == 0) {
 					fprintf(stderr, "link name already exist\n");
 					exit(EEXIST);
        }
 				size += entry->rec_len;
 			 	entry = (void *)entry + entry->rec_len;
      }
    }
    // Handle sigle indirect block
    else if (block_count == 12) {
      int indirect_count;
      unsigned int *indirect_num;
      for (indirect_count = 0; indirect_count < 256; indirect_count++) {
        size = 0;
        indirect_num = (unsigned int *) (disk + BLOCK_OFFSET(block_num) +
                                          indirect_count*sizeof(unsigned int));

        entry = (struct ext2_dir_entry_2 *) (disk + BLOCK_OFFSET(*indirect_num));
        // Start checking the current block
        while (size < inode->i_size && entry->rec_len != 0 && entry->inode != 0) {

          if (namecmp(entry->name, entry->name_len, dest_base)== 0) {
            fprintf(stderr, "link name already exist\n");
   					exit(EEXIST);
          }
 					size += entry->rec_len;
 					entry = (void *)entry + entry->rec_len;
        }
      }
    }
  }
  return 0;
}

// Heler fuction for ext2_rm

/* Check if (1) src file exists under its parent directory
 *          (2) src file is a directory under its parent directory
 * If the given src path is valid, return the corresponding src inode num
 */
int check_src_existance (struct ext2_inode *inode, char *src_base) {
  int find = -1;
  int block_count;
  int size;
  int block_num;
  int inode_num = -1;
  struct ext2_dir_entry_2 *entry;
  // Inode points to parent inode, check if target file already exits
 	for (block_count = 0; block_count < 13; block_count++) {
    size = 0;
    block_num = inode->i_block[block_count];
    entry = (struct ext2_dir_entry_2 *) (disk + BLOCK_OFFSET(block_num));

    if (block_count < 12) {
      while (size < inode->i_size && entry->rec_len != 0) {

        if (entry->file_type == 2 && namecmp(entry->name, entry->name_len, src_base) == 0) {
 					fprintf(stderr, "souce file cannot be a directory\n");
 					exit(EISDIR);
        }
        if (entry->file_type != 2 && namecmp(entry->name, entry->name_len, src_base) == 0) {

          find = 1;
          inode_num = entry->inode;
          break;
        }
 				size += entry->rec_len;
 			 	entry = (void *)entry + entry->rec_len;
      }
    }
    if (find == 1) {
      break;
    }
    // Handle sigle indirect block
    else if (block_count == 12) {
      int indirect_count;
      unsigned int *indirect_num;
      for (indirect_count = 0; indirect_count < 256; indirect_count++) {
        size = 0;
        indirect_num = (unsigned int *) (disk + BLOCK_OFFSET(block_num) +
                                          indirect_count*sizeof(unsigned int));

        entry = (struct ext2_dir_entry_2 *) (disk + BLOCK_OFFSET(*indirect_num));
        // Start check current block
        while (size < inode->i_size && entry->rec_len != 0 && entry->inode != 0) {
          // Find src file but it is a directory
          if (entry->file_type == 2 && namecmp(entry->name, entry->name_len, src_base)== 0) {
            fprintf(stderr, "souce file cannot be a directory\n");
   					exit(EISDIR);
          }
          if (entry->file_type != 2 && namecmp(entry->name, entry->name_len, src_base) == 0) {
            find = 1;
            inode_num = entry->inode;
            break;
          }
 					size += entry->rec_len;
 					entry = (void *)entry + entry->rec_len;
        }
        if (find == 1) {
          break;
        }
      }
    }
  }
  if (find != 1) {
    fprintf(stderr, "source file does not exist\n");
    exit(ENOENT);
  }

  return inode_num;
}
