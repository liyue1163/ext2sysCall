#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include "ext2.h"
#include "util.h"
#include <errno.h>
#include <libgen.h>
#define BLOCK_OFFSET(block) (1024 + (block-1)*EXT2_BLOCK_SIZE)
unsigned char *disk;

void print_content(char *inode_start, struct ext2_dir_entry_2 *entry);


int main(int argc, char **argv) {
  // To check regular file and link, retrieve basename
  char *copy;
  char *base;

  // flag indicates if ./ and ../ should be printed, defaultly set to 0.
  int flag;
  int fd;
  // find indicates if walk path is successful.
  int find;
  char path[4096];

  if (argc < 3 || argc > 4) {
      fprintf(stderr, "Usage: ext2_ls <image file name><-a><path>\n");
      exit(1);
  }
  // Not need to print ./ and ../
  if (argc == 3) {
    copy = strdup(argv[2]);
    base = basename(copy);
    flag = 0;
    strcpy(path, argv[2]);
  }
  // -a is specified, need to print ./ and ../
  if (argc == 4) {
    copy = strdup(argv[3]);
    base = basename(copy);
    flag = 1;
    strcpy(path, argv[3]);
  }

  // Open the virtual disk
  fd = open(argv[1], O_RDWR);
  disk = mmap(NULL, 128 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if(disk == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }

  //struct ext2_super_block *sb = (struct ext2_super_block *)(disk + 1024);

  // Get the location of ext2_group_descriptor,
  // which is immediately following the super block.
  struct ext2_group_desc *gd = (struct ext2_group_desc *)(disk + 1024 +
                                sizeof(struct ext2_super_block));

  // A very weird bug would occur if the pointer type below
  // is NOT declared as char *(A pointer to char)
  char *inode_start;
  inode_start = (char *) (disk + BLOCK_OFFSET(gd->bg_inode_table));

  // Start from the root inode;
  struct ext2_inode *inode = (struct ext2_inode *) (inode_start +
                                                   1*sizeof(struct ext2_inode));
  char *part = malloc(sizeof(char) * 255);
  char *delim = "/";
  part = strtok(path, delim);

  // Do path walk
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


          // Check if given path is a regular file or a link
          if (namecmp(entry->name, entry->name_len, base) == 0) {
            if (entry->file_type == 1 || entry->file_type == 7){

              printf("%s\n", argv[1]);
              return 0;
            }
          }

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
              inode = (struct ext2_inode *) (inode_start +
                      (entry->inode-1)*sizeof(struct ext2_inode));
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
                inode = (struct ext2_inode *) (inode_start +
                        (entry->inode-1)*sizeof(struct ext2_inode));
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
      fprintf(stderr, "No such file or directory\n");
      exit(ENOENT);
    }
    part = strtok(NULL, delim);
  }

  // Now inode represents the destination of given path

  for (block_count = 0; block_count < 13; block_count++) {

    size = 0;
    block_num = inode->i_block[block_count];
    entry = (struct ext2_dir_entry_2 *) (disk + BLOCK_OFFSET(block_num));
    if (block_count < 12) {
      while (size < EXT2_BLOCK_SIZE && entry->rec_len != 0 && entry->inode != 0) {

        if (namecmp(entry->name, entry->name_len, ".") == 0 || namecmp(entry->name, entry->name_len, "..") == 0) {
          if (flag == 1) {
            print_name(entry->name, entry->name_len);
          }
          else {
            // Do nothing inorder to prevent printing ./ and ../
          }
        }
        else {
          print_name(entry->name, entry->name_len);
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
        // Start check current block
        while (size < EXT2_BLOCK_SIZE && entry->rec_len != 0 && entry->inode != 0) {
          if (namecmp(entry->name, entry->name_len, ".") == 0 || namecmp(entry->name, entry->name_len, "..") == 0) {
            if (flag == 1) {

              print_name(entry->name, entry->name_len);
            }
            else {
              // Do nothing inorder to prevent printing ./ and ../
            }
          }
          else {

            print_name(entry->name, entry->name_len);

          }
          size += entry->rec_len;
          entry = (void *)entry + entry->rec_len;

        }
      }
    }
  }
  free(part);

  return 0;
}

void print_content(char *inode_start, struct ext2_dir_entry_2 *entry) {

  struct ext2_inode *file_inode =
  file_inode = (struct ext2_inode *) (inode_start +
          (entry->inode-1)*sizeof(struct ext2_inode));
  int cnt = 0;
  int num = file_inode->i_block[cnt];
  char *content;
  unsigned int *in_num;
  while (num != 0 && cnt < 13) {
    if (cnt < 12) {
      
      content = (char *)(disk + BLOCK_OFFSET(num));
      int j;
      for (j = 0; j < 1024; j++) {
        printf("%c", *(content+j));
      }
      printf("\n");

    }
    else if (cnt == 12) {

      in_num = (unsigned int *) (disk + BLOCK_OFFSET(num));
      int i = 1;
      while (*in_num != 0) {
        printf("++++ in_block: %d blk_num: %d ++++\n", i, *in_num);
        content = (char *)(disk + BLOCK_OFFSET(*in_num));
        int k;
        for (k = 0; k < 1024; k++) {
          printf("%c", *(content+k));
        }
        printf("\n");
        in_num = (void *) (in_num) + sizeof(unsigned int);
        i++;
      }
    }
    cnt ++;
    num = file_inode->i_block[cnt];
  }
}
