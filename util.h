#include "ext2.h"

// Common used helper functions
int get_parent_path(char *par_path, char *dest, int par_len);
int *get_block_bitmap(unsigned char *disk, struct ext2_group_desc *gd);
int *get_inode_bitmap(unsigned char *disk, struct ext2_group_desc *gd);
int new_inode_search(unsigned char *disk, int* inode_bitmap, struct ext2_super_block *sb, struct ext2_group_desc *gd);
int new_block_search(unsigned char *disk, int* block_bitmap, struct ext2_super_block *sb, struct ext2_group_desc *gd);
// String manupulation
int assign_name(char *name_field, char *base);
int print_name(char *name_field, int len);
int namecmp(char *name_field, int name_field_len, char *base);

// For ext2_ln
int copy_linkpath(unsigned char *disk, char *src_path, int block_need, int link_size, int *block_assign, struct ext2_inode *new_link_inode);
int check_src_existance (struct ext2_inode *inode, char *src_base);
int check_dest_existance (struct ext2_inode *inode, char *dest_base);

// For ext2_mkdir
int set_up_new_block(unsigned char *disk, int block_assign, int new_inode_num, int parent_inode_num);

// For ext2_cp
void copy(unsigned char *disk, unsigned char *src, int block_need, int fsize, int *block_assign, struct ext2_inode *new_file_inode);
