#include "helper.h"

unsigned char *disk;

int main(int argc, char **argv) {
    if(argc != 3) {
        fprintf(stderr, "Usage: %s <image file name> <path>\n", argv[0]);
        exit(1);
    }
    disk = load_image(argv[1]);
    if(!disk){
        perror("Failed to open disk image.");
        exit(1);
    }

    char path_string[strlen(argv[2])];
    strcat(path_string, argv[2]);
    PathNode *path = create_path_list(argv[2]);
    if(!path){
        //Special case: if only / in path is given, then path will be null, and root already exists...
        fprintf(stderr, "%s: error %d directory already exists.\n", path_string+1, -EEXIST);
        destroy_path_list(path);
        return EEXIST;
    }
    SearchResult result = find_dir_entry(disk, path, FALSE);

    if(result.error_code >= 0){
        fprintf(stderr, "%s: error %d directory already exists.\n", path_string+1, -EEXIST);
        destroy_path_list(path);
        return EEXIST;
    }else{
        if(result.extra_info == BAD_PATH){
            fprintf(stderr, "%s: error %d bad path given.\n", path_string+1, result.error_code);
            destroy_path_list(path);
            return -result.error_code;
        }else if(result.extra_info == MISSING_FILE){
            //This is what we want.
        }else{
            destroy_path_list(path);
            return -result.error_code;
        }
    }

    int block = get_free_block(disk);
    if(block < 0){
        fprintf(stderr, "%s: error %d insufficient space.\n", argv[1], block);
        destroy_path_list(path);
        return block;
    }
    int inode = get_free_inode(disk);
    if(inode < 0){
        fprintf(stderr, "%s: error %d insufficient space.\n", argv[1], inode);
        destroy_path_list(path);
        return inode;
    }
    //printf("Allocated inode %d and block %d.\n", inode, block);

    //Update the block and inode bitmaps at the correct positions.
    update_bitmap(disk, inode, 1, INODE);
    update_bitmap(disk, block, 1, BLOCK);

    //Create an inode for the new directory.
    create_inode(disk, inode, EXT2_S_IFDIR, EXT2_BLOCK_SIZE, 2, 2, (unsigned int *) &block, 1);

    struct ext2_inode *parent_inode;
    int parent_inode_num;
    if(result.parent_block_num < 0 || result.parent_offset < 0){
        //Then the parent is the root.
        parent_inode = get_inode(disk, EXT2_ROOT_INO);
        parent_inode_num = EXT2_ROOT_INO;
    }else{
        struct ext2_dir_entry *parent_dir_entry = (struct ext2_dir_entry *)(disk + EXT2_BLOCK_SIZE * result.parent_block_num + result.parent_offset);
        parent_inode = get_inode(disk, parent_dir_entry->inode);
        parent_inode_num = parent_dir_entry->inode;
    }

    //Traverse down path to get new directory name.
    PathNode *cur = path;
    while(cur){
        if(!cur->next){
            break;
        }
        cur = cur->next;
    }
    int dir_result = create_dir_entry(disk, parent_inode_num, inode, strlen(cur->filename), EXT2_FT_DIR, cur->filename);
    if(dir_result == -ENOSPC){
        int create_result = add_block(disk, parent_inode_num);
        if(create_result < 0){
            fprintf(stderr, "%s: error %d insufficient space.\n", argv[1], create_result);
            destroy_path_list(path);
            return -create_result;
        }else{
            create_dir_entry(disk, parent_inode_num, inode, strlen(cur->filename), EXT2_FT_DIR, cur->filename);
        }
    }

    //Add links to current and parent directories in newly allocated directory block.
    char* current_name = ".";
    char* parent_name = "..";
    //No need to worry about insufficient space here because we know the new dir block is empty.
    create_dir_entry(disk, inode, inode, strlen(current_name), EXT2_FT_DIR, current_name);
    create_dir_entry(disk, inode, parent_inode_num, strlen(parent_name), EXT2_FT_DIR, parent_name);
    parent_inode->i_links_count++;

    struct ext2_group_desc *group_descriptor = get_group_descriptor(disk);
    group_descriptor->bg_free_blocks_count--;
    group_descriptor->bg_free_inodes_count--;
    group_descriptor->bg_used_dirs_count++;

    struct ext2_super_block* super_block = get_super_block(disk);
    super_block->s_free_blocks_count--;
    super_block->s_free_inodes_count--;

    save_image(disk);
    destroy_path_list(path);

    return 0;
}
