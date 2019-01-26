#include "helper.h"
#include <math.h>

unsigned char *disk;

int check_dir_entry(int block, int offset){
    int fix_count = 0;
    struct ext2_dir_entry *dir_entry = (struct ext2_dir_entry *)(disk + EXT2_BLOCK_SIZE * block + offset);
    struct ext2_inode *inode = get_inode(disk, dir_entry->inode);
    struct ext2_super_block* super_block = get_super_block(disk);
    struct ext2_group_desc *group_descriptor = get_group_descriptor(disk);

    //b
    int type_match =  FALSE;
    unsigned int mode_mask = 0xf000;
    unsigned int mode = (inode->i_mode & mode_mask);
    switch (mode) {
        case EXT2_S_IFLNK:
            if(dir_entry->file_type == EXT2_FT_SYMLINK){
                type_match = TRUE;
            }
            break;
        case EXT2_S_IFREG:
            if(dir_entry->file_type == EXT2_FT_REG_FILE){
                type_match = TRUE;
            }
            break;
        case EXT2_S_IFDIR:
            if(dir_entry->file_type == EXT2_FT_DIR){
                type_match = TRUE;
            }
            break;
    }
    if(!type_match){
        switch (mode) {
            case EXT2_S_IFLNK:
                dir_entry->file_type = EXT2_FT_SYMLINK;
                break;
            case EXT2_S_IFREG:
                dir_entry->file_type = EXT2_FT_REG_FILE;
                break;
            case EXT2_S_IFDIR:
                dir_entry->file_type = EXT2_FT_DIR;
                break;
        }
        printf("Fixed: Entry type vs inode mismatch: inode [%d]\n", dir_entry->inode);
        fix_count++;
    }

    //c
    if(check_bitmap(disk, dir_entry->inode, INODE) == 0){
        update_bitmap(disk, dir_entry->inode, 1, INODE);
        printf("Fixed: inode [%d] not marked as in-use\n", dir_entry->inode);
        super_block->s_free_inodes_count--;
        group_descriptor->bg_free_inodes_count--;
        fix_count++;
    }

    //d
    if(inode->i_dtime != 0){
        inode->i_dtime = 0;
        printf("Fixed: valid inode marked for deletion: [%d]\n", dir_entry->inode);
        fix_count++;
    }

    //e
    int block_fix_count = 0;
    int b;
    for(b = 0; b < 12; b++){
        if(inode->i_block[b] == 0){
            break;
        }
        if(check_bitmap(disk, inode->i_block[b], BLOCK) == 0){
            update_bitmap(disk, inode->i_block[b], 1, BLOCK);
            super_block->s_free_blocks_count--;
            group_descriptor->bg_free_blocks_count--;
            block_fix_count++;
        }
    }
    if(b >= 12 && inode->i_block[b] != 0){
        unsigned int *indirect_blocks = (unsigned int*)(disk + EXT2_BLOCK_SIZE * inode->i_block[b]);
        while(*indirect_blocks > 0){
            if(check_bitmap(disk, *indirect_blocks, BLOCK) == 0){
                update_bitmap(disk, *indirect_blocks, 1, BLOCK);
                super_block->s_free_blocks_count--;
                group_descriptor->bg_free_blocks_count--;
                block_fix_count++;
            }
            indirect_blocks++;
        }
    }
    fix_count += block_fix_count;
    if(block_fix_count > 0){
        printf("Fixed: %d in-use data blocks not marked in data bitmap for inode: [%d]\n", block_fix_count, dir_entry->inode);
    }

    return fix_count;
}

int check_all_files(int parent_dir_inode_num){
    int fix_count = 0;
    struct ext2_inode *parent_inode = get_inode(disk, parent_dir_inode_num);

    int b;
    for(b = 0; b < 12; b++){
        if(parent_inode->i_block[b] == 0){
            break;
        }
        unsigned char *file_caret = (disk + EXT2_BLOCK_SIZE * parent_inode->i_block[b]);
        struct ext2_dir_entry *file;
        int offset = 0;
        while(offset < EXT2_BLOCK_SIZE){
            file = (struct ext2_dir_entry *)(file_caret);
            if(file->name_len > 0 && file->inode != 0){
                fix_count += check_dir_entry(parent_inode->i_block[b], offset);
            }
            if(file->file_type == EXT2_FT_DIR && strncmp(file->name, ".", file->name_len) != 0 && strncmp(file->name, "..", file->name_len) != 0){
                fix_count += check_all_files(file->inode);
            }
            offset += file->rec_len;
            file_caret += file->rec_len;
        }
    }
    if(b >= 12 && parent_inode->i_block[b] != 0){
        //Go through the indirect blocks.
        unsigned int *indirect_blocks = (unsigned int*)(disk + EXT2_BLOCK_SIZE * parent_inode->i_block[b]);
        while(*indirect_blocks > 0){
            unsigned char *file_caret = (disk + EXT2_BLOCK_SIZE * (*indirect_blocks));
            struct ext2_dir_entry *file;
            int offset = 0;
            while(offset < EXT2_BLOCK_SIZE){
                file = (struct ext2_dir_entry *)(file_caret);
                if(file->name_len > 0){
                    fix_count += check_dir_entry(*indirect_blocks, offset);
                }
                if(file->file_type == EXT2_FT_DIR && strncmp(file->name, ".", file->name_len) != 0 && strncmp(file->name, "..", file->name_len) != 0){
                    fix_count += check_all_files(file->inode);
                }
                offset += file->rec_len;
                file_caret += file->rec_len;
            }
            indirect_blocks++;
        }
    }

    return fix_count;
}

int print_count_fix(int record_count, int bitmap_count, int record_type, int bitmap_type){
    int difference = abs(record_count - bitmap_count);
    switch (record_type) {
        case SUPER_BLOCK:
            printf("Fixed: superblock's ");
            break;
        case GROUP_DESC:
            printf("Fixed: block group's ");
            break;
    }
    switch (bitmap_type) {
        case INODE:
            printf("free inodes counter was off by ");
            break;
        case BLOCK:
            printf("free blocks counter was off by ");
            break;
    }
    printf("%d compared to the bitmap\n", difference);
    return difference;
}

int main(int argc, char **argv) {
    if(argc != 2) {
        fprintf(stderr, "Usage: %s <image file name>\n", argv[0]);
        exit(1);
    }
    disk = load_image(argv[1]);
    if(!disk){
        perror("Failed to open disk image.");
        exit(1);
    }

    struct ext2_super_block* super_block = get_super_block(disk);
    struct ext2_group_desc *group_descriptor = get_group_descriptor(disk);

    //a
    int free_inode_count = 0, free_block_count = 0, total_fixes = 0;

    for(int i = 1; i <= 32; i++){
        if(check_bitmap(disk, i, INODE) == 0){
            free_inode_count++;
        }
    }
    if(super_block->s_free_inodes_count != free_inode_count){
        total_fixes += print_count_fix(super_block->s_free_inodes_count, free_inode_count, SUPER_BLOCK, INODE);
    }
    if(group_descriptor->bg_free_inodes_count != free_inode_count){
        total_fixes += print_count_fix(group_descriptor->bg_free_inodes_count, free_inode_count, GROUP_DESC, INODE);
    }

    for(int b = 1; b <= 128; b++){
        if(check_bitmap(disk, b, BLOCK) == 0){
            free_block_count++;
        }
    }
    if(super_block->s_free_blocks_count != free_block_count){
        total_fixes += print_count_fix(super_block->s_free_blocks_count, free_block_count, SUPER_BLOCK, BLOCK);
    }
    if(group_descriptor->bg_free_blocks_count != free_block_count){
        total_fixes += print_count_fix(group_descriptor->bg_free_blocks_count, free_block_count, GROUP_DESC, BLOCK);
    }

    total_fixes += check_all_files(EXT2_ROOT_INO);
    if(total_fixes > 0){
        printf("%d file system inconsistencies repaired!\n", total_fixes);
    }else{
        printf("No file system inconsistencies detected!\n");
    }

    save_image(disk);

    return 0;
}
