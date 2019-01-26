#include "helper.h"
/*
Returns pointer to the starting point of the image, if fails, returns NULL.
*/
unsigned char* load_image(char *path){
    unsigned char* disk = NULL;
    DISK_IMAGE_FILE_DESCRIPTOR = open(path, O_RDWR);
    disk = mmap(NULL, 128 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, DISK_IMAGE_FILE_DESCRIPTOR, 0);
    if(disk == MAP_FAILED) {
        disk = NULL;
    }
    return disk;
}

/*
Writes the modified disk buffer back to the original file.
*/
int save_image(unsigned char* disk){
    return write(DISK_IMAGE_FILE_DESCRIPTOR, disk, BLOCK_COUNT * EXT2_BLOCK_SIZE);
}

/*
Takes in the path argument for EXT2, fixes path for trailing slashes
returns linked list of PathNodes to be processed by other functions.
TODO: Make notification system for relative paths!
*/
PathNode *create_path_list(char *path){
    char* token;
    PathNode *head = NULL;
    char *copy = NULL, *original = NULL;
    copy = original = strdup(path);

    while ((token = strsep(&copy, "/")) != NULL){
        if(strlen(token) > 0){
            if(head){
                PathNode *current = head;
                //Insert at the end of the list:
                while(current->next){
                    current = current->next;
                }
                PathNode *node = malloc(sizeof(PathNode));
                int length = strlen(token);
                node->filename = malloc(length + 1);
                strncpy(node->filename, token, length);
                node->filename[length] = '\0';
                node->next = NULL;

                current->next = node;
            }else{
                head = malloc(sizeof(PathNode));
                int length = strlen(token);
                head->filename = malloc(length + 1);
                strncpy(head->filename, token, length);
                head->filename[length] = '\0';
                head->next = NULL;
            }
        }
    }
    free(original);
    return head;
}

/*
Retuns block number of the next free block, search starting from the beginning
of the block bitmap. If no more free blocks, return -ENOSPC.
*/
int get_free_block(unsigned char* disk){
    unsigned char* bitmap = get_block_bitmap(disk);
    for(int i = 0; i < 16; i++){
        char mask = 1;
        for(int j = 0; j < 8; j++){
            if(!(bitmap[i] & mask)){
                return i*8 + j + 1;
            }
            mask = mask << 1;
        }
    }
    return -ENOSPC;
}

/*
Retuns inode number in inode table of the next free inode, search starting from
numbber EXT2_GOOD_OLD_FIRST_INO of the inode bitmap. If no more free inodes,
return -ENOSPC.
*/
int get_free_inode(unsigned char* disk){
    unsigned char* bitmap = get_inode_bitmap(disk);
    for(int i = 0; i < 4; i++){
        char mask = 1;
        for(int j = 0; j < 8; j++){
            if(!(bitmap[i] & mask)){
                return i*8 + j + 1;
            }
            mask = mask << 1;
        }
    }
    return -ENOSPC;
}

/*
Returns an inode struct from the disk's inode table at index inode_num.+
*/
struct ext2_inode* get_inode(unsigned char* disk, int inode_num){
    struct ext2_group_desc *gd = get_group_descriptor(disk);
    struct ext2_inode *inodes = (struct ext2_inode *)(disk + EXT2_BLOCK_SIZE * gd->bg_inode_table);
    return &inodes[inode_num - 1];
}

/*
Returns the only superblock struct we have to worry about.
*/
struct ext2_super_block* get_super_block(unsigned char* disk){
    struct ext2_super_block *superblock = (struct ext2_super_block *)(disk + EXT2_BLOCK_SIZE);
    return superblock;
}

/*
Returns the only group descriptor struct we have to worry about.
*/
struct ext2_group_desc* get_group_descriptor(unsigned char* disk){
    struct ext2_group_desc *group_descriptor = (struct ext2_group_desc *)(disk + EXT2_BLOCK_SIZE * 2);
    return group_descriptor;
}

/*
Returns pointer to the start of the inode bitmap.
*/
unsigned char* get_inode_bitmap(unsigned char* disk){
    struct ext2_group_desc *gd = get_group_descriptor(disk);
    unsigned char *inode_bitmap = (disk + EXT2_BLOCK_SIZE * gd->bg_inode_bitmap);
    return inode_bitmap;
}

/*
Returns pointer to the start of the inode bitmap.
*/
unsigned char* get_block_bitmap(unsigned char* disk){
    struct ext2_group_desc *gd = get_group_descriptor(disk);
    unsigned char *block_bitmap = (disk + EXT2_BLOCK_SIZE * gd->bg_block_bitmap);
    return block_bitmap;
}

/*
Prints information about an inode, just like in readimage.c
May be useful for debugging stuff.
*/
void print_inode(unsigned char* disk, int inode_num){
    struct ext2_inode* inode = get_inode(disk, inode_num);
    inode_num -= 1;

    int offset = inode_num % 8;
    int byte = inode_num / 8;
    char mask = 1 << offset;
    unsigned char* inode_bitmap = get_inode_bitmap(disk);

    if((inode_num == EXT2_ROOT_INO - 1 || inode_num > EXT2_GOOD_OLD_FIRST_INO - 1) && inode_bitmap[byte] & mask){
        if(inode->i_size > 0){
            char type;
            unsigned int mode_mask = 0xf000;
            unsigned int mode = (inode->i_mode & mode_mask);
            switch(mode){
                case EXT2_S_IFDIR:
                    type = 'd';
                    break;
                case EXT2_S_IFLNK:
                    type = 's';
                    break;
                default:
                    type = 'f';
                    break;
            }
            printf("[%d] type: %c size: %d links: %d blocks: %d\n", inode_num + 1, type,
                inode->i_size, inode->i_links_count, inode->i_blocks);
            printf("[%d] Blocks: ", inode_num + 1);
            for(int j = 0; j < 15; j++){
                if(inode->i_block[j] > 0){
                    printf(" %d", inode->i_block[j]);
                }else{
                    break;
                }
            }
            printf("\n");
        }
    }
}
/*
Taking in a list of pathnodes and a pointer to virtual disk image, traverse the
directory entries one by one searching for the current indexed path node filename.
If at any point the path cannot be resolved, return -ENOENT. Otherwise, return
the block number of the file being sought after.
*/
SearchResult find_dir_entry(unsigned char* disk, PathNode* path, int ignore_symlink){
    PathNode *current = path;
    SearchResult result;
    result.error_code = -ENOENT;
    result.parent_block_num = -1;
    result.parent_offset = -1;
    result.extra_info = MISSING_FILE;
    result.file_type = EXT2_FT_DIR;
    result.softlink_path = NULL;
    struct ext2_inode *current_inode = get_inode(disk, EXT2_ROOT_INO);
    int offset = 0;

    if(!path){
        result.extra_info = JUST_ROOT;
        return result;
    }
    do{
        //Iterate through all block possible in directory inodes.
        offset = -ENOENT;
        int more_blocks = TRUE;
        for(int j = 0; j < 12; j++){
            if(current_inode->i_block[j] <= 0){
                more_blocks = FALSE;
                break;
            }
            offset = search_dir_block(disk, current->filename, current_inode->i_block[j]);
            if(offset >= 0){
                struct ext2_dir_entry *dir_entry = (struct ext2_dir_entry *)(disk + EXT2_BLOCK_SIZE * current_inode->i_block[j] + offset);
                if(current->next && dir_entry->file_type != EXT2_FT_DIR){
                    //Exit if there is more to the path but this current file is regular.
                    //We can assume no symbolic links will appear within path, just at end.
                    result.extra_info = BAD_PATH;
                    return result;
                }else if (!current->next){
                    //We reached the end of our filepath and came out on top!
                    //Check if the final file is a symbolic link.
                    if(dir_entry->file_type == EXT2_FT_SYMLINK && !ignore_symlink){
                        struct ext2_inode* link_inode = get_inode(disk, dir_entry->inode);
                        //Assume there is only one block for the symbolic link.
                        int link_block = link_inode->i_block[0];
                        char *link_path = (char*)(disk + EXT2_BLOCK_SIZE * link_block);
                        //Calculate length on first pass:
                        int counter = 0;
                        while(link_path[counter] != '\0'){
                            counter++;
                        }
                        char new_path[counter + 1];
                        strncpy(new_path, link_path, counter + 1);
                        PathNode *new_path_list = create_path_list(new_path);
                        SearchResult new_result = find_dir_entry(disk, new_path_list, ignore_symlink);
                        new_result.softlink_path = link_path;
                        destroy_path_list(new_path_list);
                        return new_result;
                    }
                    result.error_code = 0;
                    result.offset = offset;
                    result.block_num = current_inode->i_block[j];
                    result.file_type = dir_entry->file_type;
                    result.inode_num = dir_entry->inode;
                    return result;
                }else{
                    /*
                    We found the file we want at this level, no need to check other blocks.
                    Move on to the next file level in the path.
                    */
                    result.parent_block_num = current_inode->i_block[j];
                    result.parent_offset = offset;
                    current_inode = get_inode(disk, dir_entry->inode);
                    more_blocks = FALSE;
                    break;
                }
            }else{
                if(!current->next){
                    result.extra_info = MISSING_FILE;
                }else{
                    result.extra_info = BAD_PATH;
                }
                return result;
            }
        }
        //If there are still more blocks, they are indirectly listed.
        if(more_blocks){
            int block_list = current_inode->i_block[12];
            unsigned int *indirect_blocks = (unsigned int*)(disk + EXT2_BLOCK_SIZE * block_list);
            while(*indirect_blocks > 0){
                //These are the indefinite list of block numbers, call search like above.
                offset = search_dir_block(disk, current->filename, *indirect_blocks);
                if(offset != -ENOENT){
                    struct ext2_dir_entry *dir_entry = (struct ext2_dir_entry *)(disk + EXT2_BLOCK_SIZE * *indirect_blocks + offset);
                    if(current->next && dir_entry->file_type != EXT2_FT_DIR){
                        //TLDR we found it but it's the wrong type.
                        //Exit if there is more to the path but this current file is regular.
                        //We can assume no symbolic links will appear within path, just at end.
                        result.extra_info = BAD_PATH;
                        return result;
                    }else if (!current->next){
                        //We reached the end of our filepath and came out on top!
                        //Check if the final file is a symbolic link.
                        if(dir_entry->file_type == EXT2_FT_SYMLINK && !ignore_symlink){
                            struct ext2_inode* link_inode = get_inode(disk, dir_entry->inode);
                            //Assume there is only one block for the symbolic link.
                            int link_block = link_inode->i_block[0];
                            char *link_path = (char*)(disk + EXT2_BLOCK_SIZE * link_block);
                            //Calculate length on first pass:
                            int counter = 0;
                            while(link_path[counter] != '\0'){
                                counter++;
                            }
                            char new_path[counter + 1];
                            strncpy(new_path, link_path, counter + 1);
                            PathNode *new_path_list = create_path_list(new_path);
                            SearchResult new_result = find_dir_entry(disk, new_path_list, ignore_symlink);
                            new_result.softlink_path = link_path;
                            destroy_path_list(new_path_list);
                            return new_result;
                        }
                        result.error_code = 0;
                        result.offset = offset;
                        result.block_num = *indirect_blocks;
                        result.file_type = dir_entry->file_type;
                        result.inode_num = dir_entry->inode;
                        return result;
                    }else{
                        /*
                        We found the file we want at this level, no need to check other blocks.
                        Move on to the next file level in the path.
                        */
                        result.parent_block_num = *indirect_blocks;
                        result.parent_offset = offset;
                        current_inode = get_inode(disk, dir_entry->inode);
                        more_blocks = FALSE;
                        break;
                    }
                }else{
                    if(!current->next){
                        result.extra_info = MISSING_FILE;
                    }else{
                        result.extra_info = BAD_PATH;
                    }
                    return result;
                }
                indirect_blocks++;
            }
        }
        /*
        After looking through all the blocks, if we haven't found our file yet,
        we won't find it.
        */
        if(offset == -ENOENT){
            if(!current->next){
                result.extra_info = MISSING_FILE;
            }else{
                result.extra_info = BAD_PATH;
            }
            return result;
        }
        if(current){
            current = current->next;
        }
    }while(current);

    return result;
}

/*
Taking in a list of pathnodes and a pointer to virtual disk image, traverse the
directory entries one by one searching for the current indexed path node filename.
If at any point the path cannot be resolved, return -ENOENT. Otherwise, return
the block number of the file being sought after.
*/
SearchResult find_deleted_dir_entry(unsigned char* disk, PathNode* path){
    PathNode *current = path;
    SearchResult result;
    result.error_code = -ENOENT;
    result.parent_block_num = -1;
    result.parent_offset = -1;
    result.extra_info = MISSING_FILE;
    result.file_type = EXT2_FT_UNKNOWN;
    struct ext2_inode *current_inode = get_inode(disk, EXT2_ROOT_INO);
    int offset = 0;

    if(!path){
        result.extra_info = JUST_ROOT;
        return result;
    }
    do{
        //Iterate through all block possible in directory inodes.
        offset = -ENOENT;
        int more_blocks = TRUE;
        for(int j = 0; j < 12; j++){
            if(current_inode->i_block[j] <= 0){
                more_blocks = FALSE;
                break;
            }
            /*
            On the final search for the real file, must alter search function
            to look in the gaps after dir entries where the rec_len is greater
            than the minimum possible rec_len
            */
            if (!current->next){
                offset = search_deleted_dir_block(disk, current->filename, current_inode->i_block[j]);
            }else{
                offset = search_dir_block(disk, current->filename, current_inode->i_block[j]);
            }
            if(offset >= 0){
                struct ext2_dir_entry *dir_entry = (struct ext2_dir_entry *)(disk + EXT2_BLOCK_SIZE * current_inode->i_block[j] + offset);
                if(current->next && dir_entry->file_type != EXT2_FT_DIR){
                    //Exit if there is more to the path but this current file is regular.
                    //We can assume no symbolic links will appear within path, just at end.
                    result.extra_info = BAD_PATH;
                    return result;
                }else if (!current->next){
                    //We reached the end of our filepath and came out on top!
                    //Check if the final file is a symbolic link.
                    /*
                    if(dir_entry->file_type == EXT2_FT_SYMLINK){
                        struct ext2_inode* link_inode = get_inode(disk, dir_entry->inode);
                        //Assume there is only one block for the symbolic link.
                        int link_block = link_inode->i_block[0];
                        char *link_path = (char*)(disk + EXT2_BLOCK_SIZE * link_block);
                        //Calculate length on first pass:
                        int counter = 0;
                        while(link_path[counter] != '\0'){
                            counter++;
                        }
                        char new_path[counter + 1];
                        strncpy(new_path, link_path, counter + 1);
                        PathNode *new_path_list = create_path_list(new_path);
                        SearchResult new_result = find_dir_entry(disk, new_path_list, FALSE);
                        new_result.softlink_path = link_path;
                        destroy_path_list(new_path_list);
                        return new_result;
                    }
                    */
                    result.error_code = 0;
                    result.offset = offset;
                    result.block_num = current_inode->i_block[j];
                    result.file_type = dir_entry->file_type;
                    result.inode_num = dir_entry->inode;
                    return result;
                }else{
                    /*
                    We found the file we want at this level, no need to check other blocks.
                    Move on to the next file level in the path.
                    */
                    result.parent_block_num = current_inode->i_block[j];
                    result.parent_offset = offset;
                    current_inode = get_inode(disk, dir_entry->inode);
                    more_blocks = FALSE;
                    break;
                }
            }else{
                if(!current->next){
                    result.extra_info = MISSING_FILE;
                }else{
                    result.extra_info = BAD_PATH;
                }
                return result;
            }
        }
        //If there are still more blocks, they are indirectly listed.
        if(more_blocks){
            int block_list = current_inode->i_block[12];
            unsigned int *indirect_blocks = (unsigned int*)(disk + EXT2_BLOCK_SIZE * block_list);
            while(*indirect_blocks > 0){
                //These are the indefinite list of block numbers, call search like above.

                /*
                On the final search for the real file, must alter search function
                to look in the gaps after dir entries where the rec_len is greater
                than the minimum possible rec_len
                */
                if (!current->next){
                    offset = search_deleted_dir_block(disk, current->filename, *indirect_blocks);
                }else{
                    offset = search_dir_block(disk, current->filename, *indirect_blocks);
                }
                if(offset != -ENOENT){
                    struct ext2_dir_entry *dir_entry = (struct ext2_dir_entry *)(disk + EXT2_BLOCK_SIZE * *indirect_blocks + offset);
                    if(current->next && dir_entry->file_type != EXT2_FT_DIR){
                        //TLDR we found it but it's the wrong type.
                        //Exit if there is more to the path but this current file is regular.
                        //We can assume no symbolic links will appear within path, just at end.
                        result.extra_info = BAD_PATH;
                        return result;
                    }else if (!current->next){
                        //We reached the end of our filepath and came out on top!
                        //Check if the final file is a symbolic link.
                        /*
                        if(dir_entry->file_type == EXT2_FT_SYMLINK){
                            struct ext2_inode* link_inode = get_inode(disk, dir_entry->inode);
                            //Assume there is only one block for the symbolic link.
                            int link_block = link_inode->i_block[0];
                            char *link_path = (char*)(disk + EXT2_BLOCK_SIZE * link_block);
                            //Calculate length on first pass:
                            int counter = 0;
                            while(link_path[counter] != '\0'){
                                counter++;
                            }
                            char new_path[counter + 1];
                            strncpy(new_path, link_path, counter + 1);
                            PathNode *new_path_list = create_path_list(new_path);
                            SearchResult new_result = find_dir_entry(disk, new_path_list, FALSE);
                            new_result.softlink_path = link_path;
                            destroy_path_list(new_path_list);
                            return new_result;
                        }
                        */
                        result.error_code = 0;
                        result.offset = offset;
                        result.block_num = *indirect_blocks;
                        result.file_type = dir_entry->file_type;
                        result.inode_num = dir_entry->inode;
                        return result;
                    }else{
                        /*
                        We found the file we want at this level, no need to check other blocks.
                        Move on to the next file level in the path.
                        */
                        result.parent_block_num = *indirect_blocks;
                        result.parent_offset = offset;
                        current_inode = get_inode(disk, dir_entry->inode);
                        more_blocks = FALSE;
                        break;
                    }
                }else{
                    if(!current->next){
                        result.extra_info = MISSING_FILE;
                    }else{
                        result.extra_info = BAD_PATH;
                    }
                    return result;
                }
                indirect_blocks++;
            }
        }
        /*
        After looking through all the blocks, if we haven't found our file yet,
        we won't find it.
        */
        if(offset == -ENOENT){
            if(!current->next){
                result.extra_info = MISSING_FILE;
            }else{
                result.extra_info = BAD_PATH;
            }
            return result;
        }
        if(current){
            current = current->next;
        }
    }while(current);

    return result;
}

/*
Iterates through the entries of directory block block_num on disk, searching for
a file named filename. Returns the offset inside the block where the dir_entry
starts, or -ENOENT if not found.
*/
int search_dir_block(unsigned char* disk, char *filename, int block_num){
    unsigned char *file_caret = (disk + EXT2_BLOCK_SIZE * block_num);
    struct ext2_dir_entry *file;
    int offset = 0;
    while(offset < EXT2_BLOCK_SIZE){
        file = (struct ext2_dir_entry *)(file_caret);
        if(strncmp(filename, file->name, strlen(filename)) == 0){
            return offset;
        }
        offset += file->rec_len;
        file_caret += file->rec_len;
    }
    return -ENOENT;
}

int search_deleted_dir_block(unsigned char* disk, char *filename, int block_num){
    unsigned char *file_caret = (disk + EXT2_BLOCK_SIZE * block_num);
    struct ext2_dir_entry *file;
    int offset = 0;
    while(offset < EXT2_BLOCK_SIZE){
        file = (struct ext2_dir_entry *)(file_caret);

        //Check if we have found garbage data:
        if(file->name_len > file->rec_len || offset + file->rec_len > EXT2_BLOCK_SIZE || file->name_len == 0){
            break;
        }

        if(strncmp(filename, file->name, strlen(filename)) == 0){
            return offset;
        }

        int min_rec_len = 8 + file->name_len;
        if(min_rec_len % 4 != 0){
            min_rec_len += 4 - ((8 + file->name_len) % 4);
        }

        offset += min_rec_len;
        file_caret += min_rec_len;
    }
    return -ENOENT;
}

int find_prev_deleted_dir_entry(unsigned char* disk, char *filename, int block_num){
    unsigned char *file_caret = (disk + EXT2_BLOCK_SIZE * block_num);
    struct ext2_dir_entry *file;
    int offset = 0, last_offset = 0, next_real_entry = 0;
    while(offset < EXT2_BLOCK_SIZE){
        file = (struct ext2_dir_entry *)(file_caret);

        //Check if we have found garbage data:
        if(file->name_len > file->rec_len || offset + file->rec_len > EXT2_BLOCK_SIZE || file->name_len == 0){
            break;
        }

        if(strncmp(filename, file->name, strlen(filename)) == 0){
            return last_offset;
        }

        int min_rec_len = 8 + file->name_len;
        if(min_rec_len % 4 != 0){
            min_rec_len += 4 - ((8 + file->name_len) % 4);
        }

        if(offset == next_real_entry){
            last_offset = offset;
            next_real_entry += file->rec_len;
        }

        offset += min_rec_len;
        file_caret += min_rec_len;
    }
    return -ENOENT;
}

int find_prev_dir_entry(unsigned char* disk, char *filename, int block_num){
    unsigned char *file_caret = (disk + EXT2_BLOCK_SIZE * block_num);
    struct ext2_dir_entry *file;
    int offset = 0, last_offset = 0;
    while(offset < EXT2_BLOCK_SIZE){
        file = (struct ext2_dir_entry *)(file_caret);
        if(strncmp(filename, file->name, strlen(filename)) == 0){
            return last_offset;
        }
        last_offset = offset;
        offset += file->rec_len;
        file_caret += file->rec_len;
    }
    return -ENOENT;
}

/*
Recursively free the nodes and the filename strings along an entire linked list
of path nodes.
*/
void destroy_path_list(PathNode *path){
    if(path->next){
        destroy_path_list(path->next);
    }
    free(path->filename);
    free(path);
    return;
}

/*
Initializes a new inode given the inode number and certain fields:
mode, size, links count, blocks (sectors), block array, block array size.
*/
void create_inode(unsigned char* disk, int inode_num, unsigned short mode, unsigned int size, unsigned short links, unsigned int sectors, unsigned int* blocks, int block_count){
    struct ext2_group_desc *gd = get_group_descriptor(disk);
    struct ext2_inode *inode = (struct ext2_inode *)(disk + EXT2_BLOCK_SIZE * gd->bg_inode_table + (inode_num - 1) * sizeof(struct ext2_inode));

    //Fill members according to specifications.
    inode->i_mode = mode;
    inode->i_uid = 0;
    inode->i_size = size;
    inode->i_atime = 0;
    inode->i_dtime = 0;
    inode->i_mtime = 0;
    inode->i_ctime = 0;
    inode->i_gid = 0;
    inode->i_links_count = links;
    inode->i_blocks = sectors;
    inode->osd1 = 0;
    inode->i_generation = 0;
    inode->i_file_acl = 0;
    inode->i_dir_acl = 0;
    inode->i_faddr = 0;

    int last_block = 0;
    for(int i = 0; i < block_count; i++){
        inode->i_block[i] = blocks[i];
        last_block++;
    }
    //0 terminate the block list:
    if(last_block < 14){
        inode->i_block[last_block + 1] = 0;
    }

    return;
}

/*
Creates a new directory entry in the latest directory block of the parent directory.
*/
int create_dir_entry(unsigned char* disk, int parent_inode_num, int inode, unsigned char name_len, char file_type, char* name){
    struct ext2_inode *parent_inode = get_inode(disk, parent_inode_num);
    int more_blocks = TRUE, found_space = FALSE;
    int current_block = 0;
    int offset = 0;
    int last_direct_block_parent = 0;
    for(int j = 0; j < 12; j++){
        if(parent_inode->i_block[j] <= 0){
            break;
        }
        last_direct_block_parent = j;
    }

    for(int j = last_direct_block_parent; j < 12; j++){
        if(found_space){
            //No need to check any more blocks.
            more_blocks = FALSE;
            break;
        }else{
            current_block = parent_inode->i_block[j];
        }
        if(current_block <= 0){
            /*
            If this happens there was no space on any of the previous blocks
            so return ENOSPC to notify caller that they need to add a new block
            to the parent inode and then try again (inefficient but simple)
            */
            return -ENOSPC;
        }
        unsigned char *file_caret = (disk + EXT2_BLOCK_SIZE * current_block);
        struct ext2_dir_entry *file;
        offset = 0;
        int previous_dir_entry_size = 0;

        int new_dir_entry_size = 8 + name_len;
        if(new_dir_entry_size % 4 != 0){
            new_dir_entry_size += 4 - ((8 + name_len) % 4);
        }

        while(offset < EXT2_BLOCK_SIZE){
            file = (struct ext2_dir_entry *)(file_caret);
            previous_dir_entry_size = 8 + file->name_len;
            if(previous_dir_entry_size % 4 != 0){
                previous_dir_entry_size += 4 - ((8 + file->name_len) % 4);
            }
            if((offset + file->rec_len >= EXT2_BLOCK_SIZE && EXT2_BLOCK_SIZE - (offset + previous_dir_entry_size + new_dir_entry_size) >= 0 )|| file->rec_len == 0){
                //Handle special case where we are first file in the block.
                if(file->rec_len > 0){
                    //"Crop" original capstone file size.
                    file->rec_len = previous_dir_entry_size;
                    offset += file->rec_len;
                }
                found_space = TRUE;
                break;
            }
            offset += file->rec_len;
            file_caret += file->rec_len;
        }
    }
    if(more_blocks && parent_inode->i_block[12] > 0){
        //get indirect block and then for each num in there (only add to LAST indir block)
        //do same as above
        unsigned int *indirect_blocks = (unsigned int*)(disk + EXT2_BLOCK_SIZE * parent_inode->i_block[12]);
        int last_indirect_block = 0;
        for(int i = 0; i < EXT2_BLOCK_SIZE / sizeof(unsigned int); i++){
            if(indirect_blocks[i] <= 0){
                break;
            }
            last_indirect_block = i;
        }
        unsigned char *file_caret = (disk + EXT2_BLOCK_SIZE * indirect_blocks[last_indirect_block]);
        struct ext2_dir_entry *file;
        offset = 0;
        int previous_dir_entry_size = 0;
        int new_dir_entry_size = 8 + name_len;
        if(new_dir_entry_size % 4 != 0){
            new_dir_entry_size += 4 - ((8 + name_len) % 4);
        }
        while(offset < EXT2_BLOCK_SIZE){
            file = (struct ext2_dir_entry *)(file_caret);
            previous_dir_entry_size = 8 + file->name_len;
            if(previous_dir_entry_size % 4 != 0){
                previous_dir_entry_size += 4 - ((8 + file->name_len) % 4);
            }
            if((offset + file->rec_len >= EXT2_BLOCK_SIZE && EXT2_BLOCK_SIZE - (offset + previous_dir_entry_size + new_dir_entry_size) >= 0 )|| file->rec_len == 0){
                //Handle special case where we are first file in the block.
                if(file->rec_len > 0){
                    //"Crop" original capstone file size.
                    file->rec_len = previous_dir_entry_size;
                    offset += file->rec_len;
                }
                found_space = TRUE;
                break;
            }
            offset += file->rec_len;
            file_caret += file->rec_len;
        }
        if(!found_space){
            //Then we need a new block!
            return -ENOSPC;
        }
    }else if((more_blocks && parent_inode->i_block[12] == 0) || !found_space){
        return -ENOSPC;
    }

    struct ext2_dir_entry* new_dir_entry = (struct ext2_dir_entry*)(disk + EXT2_BLOCK_SIZE * current_block + offset);
    new_dir_entry->inode = inode;
    //Fill in to the end of the directory block.
    new_dir_entry->rec_len = EXT2_BLOCK_SIZE - offset;
    new_dir_entry->name_len = name_len;
    new_dir_entry->file_type = file_type;
    strncpy(new_dir_entry->name, name, name_len);

    //printf("Created dir_entry in block %d, offset %d with rec_len %d.\n", current_block, offset, new_dir_entry->rec_len);
    return new_dir_entry->rec_len;
}

/*
Modifies either inode or block bitmap specified in bitmap_type, and sets bit at
index to value.
*/
void update_bitmap(unsigned char *disk, int index, int value, int bitmap_type){
    unsigned char* bitmap;
    index--;
    char mask = 1 << index % 8;
    switch(bitmap_type){
        case INODE:
            bitmap = get_inode_bitmap(disk);
            if(!value){
                bitmap[index/8] &= ~mask;
            }else{
                bitmap[index/8] |= mask;
            }
            break;
        case BLOCK:
            bitmap = get_block_bitmap(disk);
            if(!value){
                bitmap[index/8] &= ~mask;
            }else{
                bitmap[index/8] |= mask;
            }
            break;
    }
}

int check_bitmap(unsigned char *disk, int index, int bitmap_type){
    unsigned char* bitmap;
    index--;
    char mask = 1 << index % 8;
    switch(bitmap_type){
        case INODE:
            bitmap = get_inode_bitmap(disk);
            if(bitmap[index/8] & mask){
                return 1;
            }else{
                return 0;
            }
            break;
        case BLOCK:
            bitmap = get_block_bitmap(disk);
            if(bitmap[index/8] & mask){
                return 1;
            }else{
                return 0;
            }
            break;
        default:
            return -1;
            break;
    }
}

/*
Add a new free block to the i_block list at inode, if a single indirection is
required, then it will create an extra block to hold the pointers, and
then the block where the data should go.
*/
int add_block(unsigned char* disk, int inode_num){
    struct ext2_inode *inode = get_inode(disk, inode_num);
    int ret_block_num = 0;
    int i;
    //Check for the last direct block.
    for(i = 0; i < 12; i++){
        if(inode->i_block[i] == 0){
            break;
        }
    }
    if(i >= 12){
        //Get block for the actual data.
        int new_block_num = get_free_block(disk);
        if(new_block_num < 0){
            return -ENOSPC;
        }
        update_bitmap(disk, new_block_num, 1, BLOCK);
        ret_block_num = new_block_num;
        int block_list = inode->i_block[i];

        if(block_list <= 0){
            //Must setup single indirection oursevles!
            block_list = get_free_block(disk);
            if(block_list < 0){
                //Can't allocate any more blocks. Just give up!
                return -ENOSPC;
            }
            inode->i_block[i] = block_list;
            update_bitmap(disk, block_list, 1, BLOCK);

            //Add our new data block as the first block in the indirect list.
            unsigned int *indirect_blocks = (unsigned int*)(disk + EXT2_BLOCK_SIZE * block_list);
            *indirect_blocks = new_block_num;

            //Zero out next block to stop list (if applicable)
            indirect_blocks++;
            *indirect_blocks = 0;

            struct ext2_group_desc *group_descriptor = get_group_descriptor(disk);
            group_descriptor->bg_free_blocks_count -= 2;

            struct ext2_super_block* super_block = get_super_block(disk);
            super_block->s_free_blocks_count -= 2;

            inode->i_blocks+= 4;
            inode->i_size += EXT2_BLOCK_SIZE * 2;
        }else{
            //Just add to the end of the single indirection list.
            unsigned int *indirect_blocks = (unsigned int*)(disk + EXT2_BLOCK_SIZE * block_list);
            int allocated = FALSE;
            for(int i = 0; i < EXT2_BLOCK_SIZE / sizeof(unsigned int); i++){
                if(indirect_blocks[i] == 0){
                    indirect_blocks[i] = new_block_num;
                    allocated = TRUE;
                    break;
                }
            }
            if(!allocated){
                //We are really out of luck! No more indirect space. GG.
                return -ENOSPC;
            }

            struct ext2_group_desc *group_descriptor = get_group_descriptor(disk);
            group_descriptor->bg_free_blocks_count--;

            struct ext2_super_block* super_block = get_super_block(disk);
            super_block->s_free_blocks_count--;

            inode->i_blocks+= 2;
            inode->i_size += EXT2_BLOCK_SIZE;
        }
    }else{
        //Just slap in another direct block.
        int new_block_num = get_free_block(disk);
        ret_block_num = new_block_num;
        if(new_block_num < 0){
            return -ENOSPC;
        }
        inode->i_block[i] = new_block_num;
        //Add two sectors.
        inode->i_blocks += 2;
        inode->i_size += EXT2_BLOCK_SIZE;
        update_bitmap(disk, new_block_num, 1, BLOCK);
    }
    return ret_block_num;
}

/*
Same as add block but for copying files, inlcudes custom size field.
*/
int add_block_file(unsigned char* disk, int inode_num, int size){
    struct ext2_inode *inode = get_inode(disk, inode_num);
    int ret_block_num = 0;
    int i;
    //Check for the last direct block.
    for(i = 0; i < 12; i++){
        if(inode->i_block[i] == 0){
            break;
        }
    }
    if(i >= 12){
        int block_list = inode->i_block[i];
        if(block_list <= 0){
            //Must setup single indirection oursevles!
            block_list = get_free_block(disk);
            if(block_list < 0){
                //Can't allocate any more blocks. Just give up!
                return -ENOSPC;
            }
            inode->i_block[i] = block_list;
            update_bitmap(disk, block_list, 1, BLOCK);

            //Get block for the actual data.
            int new_block_num = get_free_block(disk);
            if(new_block_num < 0){
                return -ENOSPC;
            }
            update_bitmap(disk, new_block_num, 1, BLOCK);
            ret_block_num = new_block_num;

            //Add our new data block as the first block in the indirect list.
            unsigned int *indirect_blocks = (unsigned int*)(disk + EXT2_BLOCK_SIZE * block_list);
            *indirect_blocks = new_block_num;

            //Zero out next block to stop list (if applicable)
            indirect_blocks++;
            *indirect_blocks = 0;

            struct ext2_group_desc *group_descriptor = get_group_descriptor(disk);
            group_descriptor->bg_free_blocks_count -= 2;

            struct ext2_super_block* super_block = get_super_block(disk);
            super_block->s_free_blocks_count -= 2;

            //i_blocks doesn't count indirect apparently...but solutions include it?
            inode->i_blocks+= 4;
            inode->i_size += size;
        }else{
            //Get block for the actual data.
            int new_block_num = get_free_block(disk);
            if(new_block_num < 0){
                return -ENOSPC;
            }
            update_bitmap(disk, new_block_num, 1, BLOCK);
            ret_block_num = new_block_num;

            //Just add to the end of the single indirection list.
            unsigned int *indirect_blocks = (unsigned int*)(disk + EXT2_BLOCK_SIZE * block_list);
            int allocated = FALSE;
            for(int i = 0; i < EXT2_BLOCK_SIZE / sizeof(unsigned int); i++){
                if(indirect_blocks[i] == 0){
                    indirect_blocks[i] = new_block_num;
                    allocated = TRUE;
                    break;
                }
            }
            if(!allocated){
                //We are really out of luck! No more indirect space. GG.
                return -ENOSPC;
            }

            struct ext2_group_desc *group_descriptor = get_group_descriptor(disk);
            group_descriptor->bg_free_blocks_count--;

            struct ext2_super_block* super_block = get_super_block(disk);
            super_block->s_free_blocks_count--;

            inode->i_blocks+= 2;
            inode->i_size += size;
        }
    }else{
        //Just slap in another direct block.
        int new_block_num = get_free_block(disk);
        ret_block_num = new_block_num;
        if(new_block_num < 0){
            return -ENOSPC;
        }
        inode->i_block[i] = new_block_num;
        //Add two sectors.
        inode->i_blocks += 2;
        inode->i_size += size;
        update_bitmap(disk, new_block_num, 1, BLOCK);

        struct ext2_group_desc *group_descriptor = get_group_descriptor(disk);
        group_descriptor->bg_free_blocks_count--;

        struct ext2_super_block* super_block = get_super_block(disk);
        super_block->s_free_blocks_count--;
    }
    return ret_block_num;
}

int remove_last_block(unsigned char* disk, int inode_num){
    struct ext2_inode *inode = get_inode(disk, inode_num);
    int i, block_to_free;
    //Check for the last direct block
    for(i = 0; i < 12; i++){
        if(inode->i_block[i] == 0){
            block_to_free = inode->i_block[i-1];
            inode->i_block[i-1] = 0;
            break;
        }
    }

    inode->i_blocks -= 2;

    struct ext2_group_desc *group_descriptor = get_group_descriptor(disk);
    group_descriptor->bg_free_blocks_count++;

    struct ext2_super_block* super_block = get_super_block(disk);
    super_block->s_free_blocks_count++;

    update_bitmap(disk, block_to_free, 0, BLOCK);
    return 0;
}
