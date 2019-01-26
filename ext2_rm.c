#include "helper.h"
#include <time.h>

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
    SearchResult result = find_dir_entry(disk, path, TRUE);

    if(result.error_code < 0 || result.file_type == EXT2_FT_DIR){
        free(file_path);
        destroy_path_list(path);
        if(result.file_type == EXT2_FT_DIR){
            return -EISDIR;
        }
        return result.error_code;
    }

    struct ext2_dir_entry *file_dir_entry = (struct ext2_dir_entry *)(disk + EXT2_BLOCK_SIZE * result.block_num + result.offset);
    struct ext2_inode *file_inode = get_inode(disk, file_dir_entry->inode);
    file_inode->i_links_count--;

    if(file_inode->i_links_count <= 0){

        update_bitmap(disk, file_dir_entry->inode, 0, INODE);
        file_inode->i_dtime = (unsigned)time(NULL);
        struct ext2_group_desc *group_descriptor = get_group_descriptor(disk);
        struct ext2_super_block* super_block = get_super_block(disk);
        group_descriptor->bg_free_inodes_count++;
        super_block->s_free_inodes_count++;

        //Zero out the old blocks of this file in the block bitmap:
        int i;
        for(i = 0; i < 12; i++){
            if(file_inode->i_block[i] == 0){
                break;
            }
            update_bitmap(disk, file_inode->i_block[i], 0, BLOCK);
            group_descriptor->bg_free_blocks_count++;
            super_block->s_free_blocks_count++;
        }
        //Also free indirect blocks
        if(i >= 12 && file_inode->i_block[i] != 0){
            update_bitmap(disk, file_inode->i_block[i], 0, BLOCK);
            group_descriptor->bg_free_blocks_count++;
            super_block->s_free_blocks_count++;

            unsigned int *indirect_blocks = (unsigned int*)(disk + EXT2_BLOCK_SIZE * file_inode->i_block[i]);
            for(int i = 0; i < EXT2_BLOCK_SIZE / sizeof(unsigned int); i++){
                if(indirect_blocks[i] == 0){
                    break;
                }
                update_bitmap(disk, indirect_blocks[i], 0, BLOCK);
                group_descriptor->bg_free_blocks_count++;
                super_block->s_free_blocks_count++;
            }
        }
    }

    //Get filename of file to delete.
    PathNode *cur = path;
    while(cur){
        if(!cur->next){
            break;
        }
        cur = cur->next;
    }

    if(result.offset > 0){
        int previous_entry_offset = find_prev_dir_entry(disk, cur->filename, result.block_num);
        struct ext2_dir_entry *previous_dir_entry = (struct ext2_dir_entry *)(disk + EXT2_BLOCK_SIZE * result.block_num + previous_entry_offset);
        previous_dir_entry->rec_len += file_dir_entry->rec_len;
    }else if(result.offset == 0){//Special case
        file_dir_entry->inode = 0;
    }

    free(file_path);
    destroy_path_list(path);

    return 0;
}
