#include "helper.h"

unsigned char *disk;

int main(int argc, char **argv) {
    if(argc != 3) {
        fprintf(stderr, "Usage: %s <image file name> <path to file>\n", argv[0]);
        exit(1);
    }

    disk = load_image(argv[1]);
    if(!disk){
        perror("Failed to open disk image.");
        exit(1);
    }

    char *file_path = strdup(argv[2]);
    PathNode *path = create_path_list(argv[2]);
    //Check if it has even been deleted:
    SearchResult result = find_dir_entry(disk, path, TRUE);
    if(result.error_code >= 0){
        free(file_path);
        destroy_path_list(path);
        printf("File has not been deleted.\n");
        return -EEXIST;
    }

    result = find_deleted_dir_entry(disk, path);
    if(result.error_code < 0 || result.file_type == EXT2_FT_DIR){
        free(file_path);
        destroy_path_list(path);
        if(result.file_type == EXT2_FT_DIR){
            printf("Not regular file.\n");
            return -EISDIR;
        }
        printf("File not found.\n");
        return result.error_code;
    }

    struct ext2_dir_entry *file_dir_entry = (struct ext2_dir_entry *)(disk + EXT2_BLOCK_SIZE * result.block_num + result.offset);
    if(file_dir_entry->inode == 0){
        free(file_path);
        destroy_path_list(path);
        printf("Unable to recover file.\n");
        return -ENOENT;
    }
    struct ext2_inode *file_inode = get_inode(disk, file_dir_entry->inode);
    if(file_inode->i_dtime == 0 || check_bitmap(disk, file_dir_entry->inode, INODE) == 1){
        //Then the inode has been reused. Can't recover.
        free(file_path);
        destroy_path_list(path);
        printf("Unable to recover file.\n");
        return -ENOENT;
    }

    int block_reused = FALSE;
    //Check all blocks have not been reused.
    int i;
    for(i = 0; i < 12; i++){
        if(file_inode->i_block[i] == 0){
            break;
        }
        if(check_bitmap(disk, file_inode->i_block[i], BLOCK) == 1){
            block_reused = TRUE;
            break;
        }
    }
    //Also free indirect blocks
    if(i >= 12 && file_inode->i_block[i] != 0){
        if(check_bitmap(disk, file_inode->i_block[i], BLOCK) == 1){
            block_reused = TRUE;
        }

        unsigned int *indirect_blocks = (unsigned int*)(disk + EXT2_BLOCK_SIZE * file_inode->i_block[i]);
        for(int i = 0; i < EXT2_BLOCK_SIZE / sizeof(unsigned int); i++){
            if(indirect_blocks[i] == 0){
                break;
            }
            if(check_bitmap(disk, indirect_blocks[i], BLOCK) == 1){
                block_reused = TRUE;
                break;
            }
        }
    }

    if(block_reused){
        free(file_path);
        destroy_path_list(path);
        printf("Unable to recover file.\n");
        return -ENOENT;
    }

    //At this point all the previous structures are intact. Begin to recover file:

    PathNode *cur = path;
    while(cur){
        if(!cur->next){
            break;
        }
        cur = cur->next;
    }

    int previous_dir_entry_offset = find_prev_deleted_dir_entry(disk, cur->filename, result.block_num);
    struct ext2_dir_entry *previous_dir_entry = (struct ext2_dir_entry *)(disk + EXT2_BLOCK_SIZE * result.block_num + previous_dir_entry_offset);

    //Restore record lengths:
    int min_len = 8 + previous_dir_entry->name_len;
    previous_dir_entry->rec_len = min_len + (result.offset - previous_dir_entry_offset - min_len);

    //Restore inode:
    struct ext2_group_desc *group_descriptor = get_group_descriptor(disk);
    struct ext2_super_block* super_block = get_super_block(disk);

    file_inode->i_dtime = 0;
    file_inode->i_links_count++;
    update_bitmap(disk, file_dir_entry->inode, 1, INODE);
    group_descriptor->bg_free_inodes_count--;
    super_block->s_free_inodes_count--;

    //Restore the inode's blocks:
    for(i = 0; i < 12; i++){
        if(file_inode->i_block[i] == 0){
            break;
        }
        update_bitmap(disk, file_inode->i_block[i], 1, BLOCK);
        group_descriptor->bg_free_blocks_count--;
        super_block->s_free_blocks_count--;
    }
    //Also free indirect blocks
    if(i >= 12 && file_inode->i_block[i] != 0){
        update_bitmap(disk, file_inode->i_block[i], 1, BLOCK);
        group_descriptor->bg_free_blocks_count--;
        super_block->s_free_blocks_count--;

        unsigned int *indirect_blocks = (unsigned int*)(disk + EXT2_BLOCK_SIZE * file_inode->i_block[i]);
        for(int i = 0; i < EXT2_BLOCK_SIZE / sizeof(unsigned int); i++){
            if(indirect_blocks[i] == 0){
                break;
            }
            update_bitmap(disk, indirect_blocks[i], 1, BLOCK);
            group_descriptor->bg_free_blocks_count--;
            super_block->s_free_blocks_count--;
        }
    }

    free(file_path);
    destroy_path_list(path);

    return 0;
}
