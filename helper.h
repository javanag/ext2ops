#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>

#include "ext2.h"

#ifndef HELPER_FUNCTIONS
#define HELPER_FUNCTIONS

#define    PATH_ABSOLUTE 0
#define    PATH_RELATIVE 1
#define    FALSE 0
#define    TRUE 1
#define    HARDLINK 0
#define    SOFTLINK 1

#define    INODE_COUNT 32
#define    BLOCK_COUNT 128

/*
Extra info for the MKDIR. Need to know whether the end file is missing but the rest
of the path is good, or if the path is just bad altogether.
*/
#define BAD_PATH 1
#define MISSING_FILE 2
#define INODE 3
#define BLOCK 4
#define JUST_ROOT 5
#define SUPER_BLOCK 6
#define GROUP_DESC 7

typedef struct path_node {
    char* filename;
    struct path_node *next;
} PathNode;

typedef struct search_result {
    int error_code;
    int extra_info;
    int block_num;
    int offset;
    int file_type;
    int inode_num;
    char* softlink_path;
    //If following are -1, then parent is root.
    int parent_block_num;
    int parent_offset;
} SearchResult;

int DISK_IMAGE_FILE_DESCRIPTOR;

unsigned char* load_image(char*);
int save_image(unsigned char*);

int get_free_block(unsigned char*);
int get_free_inode(unsigned char*);

void update_bitmap(unsigned char*, int, int, int);
int check_bitmap(unsigned char*, int, int);

SearchResult find_dir_entry(unsigned char*, PathNode*, int);
SearchResult find_deleted_dir_entry(unsigned char*, PathNode*);

int search_dir_block(unsigned char*, char*, int);
int search_deleted_dir_block(unsigned char*, char*, int);

int find_prev_dir_entry(unsigned char*, char*, int);
int find_prev_deleted_dir_entry(unsigned char*, char*, int);

PathNode *create_path_list(char*);
void destroy_path_list(PathNode*);

void create_inode(unsigned char*, int, unsigned short, unsigned int, unsigned short, unsigned int, unsigned int*, int);
void update_inode(unsigned char*, int, unsigned int, unsigned short, unsigned int);
int create_dir_entry(unsigned char*, int, int, unsigned char, char, char*);
int add_block(unsigned char*, int);
int add_block_file(unsigned char*, int, int);
int remove_last_block(unsigned char*, int);

struct ext2_inode *get_inode(unsigned char*, int);
struct ext2_super_block *get_super_block(unsigned char*);
/*For this assignment we may assume there is only one group.*/
struct ext2_group_desc *get_group_descriptor(unsigned char*);
unsigned char* get_inode_bitmap(unsigned char*);
unsigned char* get_block_bitmap(unsigned char*);

//Stuff ported from readimage.c
void print_inode(unsigned char*, int);

#endif
