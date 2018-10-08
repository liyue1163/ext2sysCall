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
#define BLOCK_OFFSET(block) (1024 + (block-1)*EXT2_BLOCK_SIZE)
unsigned char *disk;



int update_inode_table(struct ext2_inode *new_link_inode, int block_need, int link_size);
int update_dir_entry(unsigned char *disk, struct ext2_inode *parent_inode, int new_inode, char *src_base, int *block_bitmap, int soft);
int path_walk(unsigned char *disk, char *dest, struct ext2_group_desc *gd);


int main(int argc, char **argv) {
  // initialize variables
  char src_path[4096];
  char src_par_path[4096];
  char *src_base;
  char dest_path[4096];
  char dest_par_path[4096];
  char *dest_base;

  int *inode_bitmap;
  int *block_bitmap;
  // A flag indicates creating hard link soft = 0; or a symbolic link, soft = 1
  int soft;

  if (argc < 4 || argc > 5) {
    fprintf(stderr, "Usage: ext2_ln <image file name>[<-s>]<source path><dest path>\n");
		exit(1);
  }
  if (argc == 4) {
    soft = 0;
    strcpy(src_path, argv[2]);
    src_base = basename(src_path);
    strcpy(dest_path, argv[3]);
    dest_base = basename(dest_path);
  }
  else if (argc == 5) {
    soft = 1;
    strcpy(src_path, argv[3]);
    src_base = basename(src_path);
    strcpy(dest_path, argv[4]);
    dest_base = basename(dest_path);
  }

  /* Check arguments validity */
	int fd = open(argv[1], O_RDWR);
	disk = mmap(NULL, 128 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (disk == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}

  // Get super block, group descriptor and inode table
	struct ext2_super_block *sb = (struct ext2_super_block *) (disk + 1024);
	struct ext2_group_desc *gd = (struct ext2_group_desc *) (disk + 1024 +
																sizeof(struct ext2_super_block));
	char *inode_table;
	inode_table = (char *) (disk + BLOCK_OFFSET(gd->bg_inode_table));

  // Check source path validity, src file must exist
  get_parent_path(src_par_path, src_path, strlen(src_path)-strlen(src_base));
  int src_par_inode_num = path_walk(disk, src_par_path, gd);
  struct ext2_inode *src_par_inode = (struct ext2_inode *) (inode_table +
                              (src_par_inode_num-1)*sizeof(struct ext2_inode));
  int src_inode_num = check_src_existance(src_par_inode, src_base);

  // Check dest path validity, dest link must NOT exist
  get_parent_path(dest_par_path, dest_path, strlen(dest_path)-strlen(dest_base));
  int dest_par_inode_num = path_walk(disk, dest_par_path, gd);
  struct ext2_inode *dest_par_inode = (struct ext2_inode *) (inode_table +
                              (dest_par_inode_num-1)*sizeof(struct ext2_inode));
  check_dest_existance(dest_par_inode, dest_base);
  /* Finish checking arguments validity */

  // Store inode bitmap and block bitmap
	inode_bitmap = get_inode_bitmap(disk, gd);
	block_bitmap = get_block_bitmap(disk, gd);

  // Create a hard link
  if (soft == 0) {
    // Cerate a new dir entry in dest parent directory
    update_dir_entry(disk, dest_par_inode, src_inode_num, dest_base,
                                                block_bitmap, soft);
    // Since hard link, i_links_count of source file being linked added by 1
    struct ext2_inode *src_inode = (struct ext2_inode *) (inode_table +
                                (src_inode_num-1)*sizeof(struct ext2_inode));
    src_inode->i_links_count++;
  }
  // Create a symbolic link
  else if (soft == 1) {
    // assign a new inode for symbolic link
  	if (sb->s_free_inodes_count == 0) {
  		fprintf(stderr, "No free inode left\n");
  		exit(ENOENT);
  	}
  	int new_inode_num = new_inode_search(disk, inode_bitmap, sb, gd);

    // Get the positon of assigned inode for new dir
  	struct ext2_inode *new_link_inode = (struct ext2_inode *) (inode_table +
  																	 (new_inode_num-1)*sizeof(struct ext2_inode));

    // Initially set the all blocks to 0;
    int j;
  	for (j = 0; j < 15; j++) {
  		new_link_inode->i_block[j] = 0;
  	}

    // find relative path to store in soft link file
    int k;
    int cnt = 0;
    for (k = 0; k < strlen(dest_path); k++) {
      if (dest_path[k] == '/') {
        cnt++;
      }
    }
    cnt--;
    char relative_path[3*cnt+strlen(src_path)];
    relative_path[0] = '\0';
    int q;
    for (q = 0; q < cnt; q++) {
      strcat(relative_path, "../");
    }
    strcat(relative_path, &src_path[1]);

    // Assign free blocks for new dir
    int link_size = strlen(relative_path) + 1;
    int block_need = (link_size - 1) / EXT2_BLOCK_SIZE + 1;
    if (sb->s_free_blocks_count < block_need) {
  		fprintf(stderr, "Lack of free blocks\n");
  		exit(ENOENT);
  	}
  	int block_assign[block_need];
    int i;
  	for (i = 0; i < block_need; i++) {
  		block_assign[i] = new_block_search(disk, block_bitmap, sb, gd);
  	}


    // copy the source file path in assigned blocks for symbolic link
    copy_linkpath(disk, relative_path, block_need, link_size, block_assign, new_link_inode);

    // Update new inode assigned for new dir in inode table
  	update_inode_table(new_link_inode, block_need, link_size);

    // Update directory entry
  	update_dir_entry(disk, dest_par_inode, new_inode_num, dest_base,
                                                block_bitmap, soft);

  }
  // free allocated memory
  free(inode_bitmap);
  free(block_bitmap);

  return 0;
}



int update_inode_table(struct ext2_inode *new_file_inode, int block_need, int link_size) {
	new_file_inode->i_mode = EXT2_S_IFLNK;
	new_file_inode->i_size = link_size;
	new_file_inode->i_dtime = 0;          /* By default for a new inode*/
	new_file_inode->i_links_count = 1;
	new_file_inode->i_blocks = 2*block_need;
	/* Note that i_block[] is updated during the copy*/

	return 0;
}

/* Update the parent directory*/
int update_dir_entry(unsigned char *disk, struct ext2_inode *parent_inode,
							 int new_inode_num, char *src_base, int *block_bitmap, int soft) {

  // Locate the last block used in inode
	int check_block;
	int last_block;
	for (check_block = 0; check_block < 13; check_block++) {
		// Since i_block is assigned in order, if we reach a block is 0,
		// then the number of blocks used to store dir entries is up to check
		if (parent_inode->i_block[check_block] == 0) {
			last_block = check_block - 1;
			break;
		}
	}

	int last_block_num = parent_inode->i_block[last_block];
	// Check if last block has enough space to store new dir entry
	struct ext2_dir_entry_2 *entry =
	(struct ext2_dir_entry_2 *) (disk + BLOCK_OFFSET(last_block_num));

	int rec_len_count = 0;
	int last_rec_len = 0;
	int rest_rec_len;
	// rec len needed for source file
	int rec_len_need;
	while (rec_len_count < EXT2_BLOCK_SIZE && entry->rec_len != 0 && entry->inode != 0) {

		rec_len_count = rec_len_count + entry->rec_len;
		// Next dir entry is NOT the end of the block
		if (last_rec_len + entry->rec_len < EXT2_BLOCK_SIZE) {
			last_rec_len += entry->rec_len;
		}
		// Nexr dir entry reach the end of the block
		else {
			// Current the dir entry is the last dir entry
      // break here so that entry points to last dir entry
			break;
		}
		entry = (void *) entry + entry->rec_len;
	}
	// Finally rec_len_count should be 1024
	assert(rec_len_count == 1024);

  // Get last dir entry
  struct ext2_dir_entry_2 *last_entry = (struct ext2_dir_entry_2 *) entry;
  // Decide the size of last dir entry
  int next_rec_len;
  if (last_entry->name_len % 4 == 0) {
    next_rec_len = 8 + last_entry->name_len;
  }
  else if (last_entry->name_len % 4 > 0) {
    next_rec_len = 8 + 4 * (last_entry->name_len / 4 + 1);
  }

	// Calculate the remaining free space
	rest_rec_len = rec_len_count - last_rec_len - next_rec_len;

  // Calculate the rec_len need for this file
	if (strlen(src_base) % 4 == 0) {
		rec_len_need = 8 + strlen(src_base);
	}
	else if (strlen(src_base) % 4 > 0) {
		rec_len_need = 8 + 4 * (strlen(src_base) / 4 + 1);
	}

	// If the last block is enough to hold the new dir entry
	if (rest_rec_len >= rec_len_need) {

		// Update last dir entry's rec_len, which points to new dir entry
		last_entry->rec_len = next_rec_len;

		// Get new dir entry of the link
		struct ext2_dir_entry_2 *new_entry = (void *) last_entry + next_rec_len;
		// Update new dir entry
		new_entry->inode = new_inode_num;
		new_entry->rec_len = rest_rec_len;
		new_entry->name_len = strlen(src_base);
    if (soft == 0) {
      new_entry->file_type = EXT2_FT_REG_FILE;
    }
		else {
      new_entry->file_type = EXT2_FT_SYMLINK;
    }
		assign_name(new_entry->name, src_base);
	}
	// New block needed to store the new dir entry
	else if (rest_rec_len < rec_len_need) {
		struct ext2_super_block *sb = (struct ext2_super_block *) (disk + 1024);
		struct ext2_group_desc *gd = (struct ext2_group_desc *) (disk + 1024 +
																	sizeof(struct ext2_super_block));
		int new_block = new_block_search(disk, block_bitmap, sb, gd);
		parent_inode->i_blocks += 2;
		parent_inode->i_block[check_block] = new_block;
		// get the location of new dir entry
		struct ext2_dir_entry_2 *new_entry =
		(struct ext2_dir_entry_2 *) (disk + BLOCK_OFFSET(new_block));
		// Update new dir entry
		new_entry->inode = new_inode_num;
		new_entry->rec_len = 1024;
		new_entry->name_len = strlen(src_base);
    if (soft == 0) {
      new_entry->file_type = EXT2_FT_REG_FILE;
    }
		else {
      new_entry->file_type = EXT2_FT_SYMLINK;
    }
		assign_name(new_entry->name, src_base);
	}
	return 0;
}


/* Helper function to walk through path and
 * check if the target dir already exists.
 * Return the inode number of dest if the given dest path is valid
 */
int path_walk(unsigned char *disk, char *dest, struct ext2_group_desc *gd) {

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
      if (block_count < 12) {
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
      }
      if (find == 1) {
        // Find the target, No need to search rest blocks in current directory
        break;
      }
      // We are handling indirect block
      if (block_count == 12) {
        int indirect_count;
        unsigned int *indirect_num;
        for (indirect_count = 0; indirect_count < 256; indirect_count++) {
          size = 0;
          indirect_num = (unsigned int *) (disk + BLOCK_OFFSET(block_num) +
                                            indirect_count*sizeof(unsigned int));
          entry = (struct ext2_dir_entry_2 *) (disk + BLOCK_OFFSET(*indirect_num));
          // Start check current block
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
            break;
          }
        }
      }
    }
    // Finish checking all the blocks
    if (find == 0) {
      fprintf(stderr, "No such a file or directory\n");
      exit(ENOENT);
    }
    part = strtok(NULL, delim);
  }

 	free(part);
 	return inode_num;
}
