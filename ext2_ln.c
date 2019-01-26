#include "helper.h"

unsigned char *disk;

int main(int argc, char **argv) {
    int type = HARDLINK;

    if(argc == 4) {
        //Hard link
        type = HARDLINK;
    }else if(argc == 5 && strcmp(argv[2], "-s") == 0){
        //Soft link
        type = SOFTLINK;
    }else{
        fprintf(stderr, "Usage: %s <image file name> <path to source file> <path to dest>\n", argv[0]);
        fprintf(stderr, "OR: %s <image file name> -s <path to source file> <path to dest>\n", argv[0]);
        exit(1);
    }

    disk = load_image(argv[1]);
    if(!disk){
        perror("Failed to open disk image.");
        exit(1);
    }

    char *real_file_path;
    PathNode *source_path;
    SearchResult source_result;

    if(type == HARDLINK){
        real_file_path = strdup(argv[2]);
        source_path = create_path_list(argv[2]);
        source_result = find_dir_entry(disk, source_path, TRUE);
    }else{
        real_file_path = strdup(argv[3]);
        source_path = create_path_list(argv[3]);
        source_result = find_dir_entry(disk, source_path, FALSE);
    }

    if((source_result.error_code < 0 || source_result.file_type == EXT2_FT_DIR) && type == HARDLINK){
        free(real_file_path);
        destroy_path_list(source_path);
        if(source_result.file_type == EXT2_FT_DIR){
            return EISDIR;
        }
        return -source_result.error_code;
    }

    char *dest_file_path;
    PathNode *dest_path;
    SearchResult dest_result;

    if(type == HARDLINK){
        dest_file_path = strdup(argv[3]);
        dest_path = create_path_list(argv[3]);
        dest_result = find_dir_entry(disk, dest_path, FALSE);
    }else{
        dest_file_path = strdup(argv[4]);
        dest_path = create_path_list(argv[4]);
        dest_result = find_dir_entry(disk, dest_path, FALSE);
    }

    if(dest_result.error_code >= 0 && dest_result.file_type != EXT2_FT_DIR){
        free(real_file_path);
        free(dest_file_path);
        destroy_path_list(source_path);
        destroy_path_list(dest_path);
        return EEXIST;
    }else if((dest_result.error_code >= 0 && dest_result.file_type == EXT2_FT_DIR) || (dest_result.parent_offset == -1 && dest_result.parent_block_num == -1 && dest_result.extra_info == JUST_ROOT)){
        //Special case where the dest is just a dir, we will use the same name as
        //the source file, but if that name already exists we must throw EEXIST.
        //Just do it the lazy way using already created workflow.
        PathNode *cur = source_path;
        while(cur){
            if(!cur->next){
                break;
            }
            cur = cur->next;
        }
        char* new_path_base = NULL;
        if(dest_result.softlink_path){
            new_path_base = dest_result.softlink_path;
        }else{
            new_path_base = dest_file_path;
        }

        int string_length = strlen(new_path_base) + strlen(cur->filename) + 2;
        char *new_path_string = malloc(string_length);
        strncat(new_path_string, new_path_base, strlen(new_path_base));
        strncat(new_path_string, "/", 1);
        strncat(new_path_string, cur->filename, strlen(cur->filename));
        new_path_string[string_length] = '\0';

        if(dest_path){
            destroy_path_list(dest_path);
        }
        dest_path = create_path_list(new_path_string);
        SearchResult new_result = find_dir_entry(disk, dest_path, FALSE);

        free(new_path_string);

        //Want the result to be missing file type:
        if(new_result.error_code >= 0 || new_result.extra_info != MISSING_FILE){
            free(real_file_path);
            free(dest_file_path);
            destroy_path_list(source_path);
            destroy_path_list(dest_path);
            return -new_result.error_code;
        }
        dest_result = new_result;

    }else{
        if(dest_result.extra_info == BAD_PATH){
            free(real_file_path);
            free(dest_file_path);
            destroy_path_list(source_path);
            destroy_path_list(dest_path);
            return -dest_result.error_code;

        }else if(dest_result.extra_info == MISSING_FILE){
            //This is what we want.

        }else{
            free(real_file_path);
            free(dest_file_path);
            destroy_path_list(source_path);
            destroy_path_list(dest_path);
            return -dest_result.error_code;
        }
    }

    int inode = 0;
    if(type == HARDLINK){
        //Must increase i_links_count
        struct ext2_inode *inode_obj = get_inode(disk, source_result.inode_num);
        inode_obj->i_links_count++;
    }else{
        inode = get_free_inode(disk);
        if(inode < 0){
            fprintf(stderr, "%s: error %d insufficient space.\n", argv[1], inode);
            free(real_file_path);
            free(dest_file_path);
            destroy_path_list(source_path);
            destroy_path_list(dest_path);
            return inode;
        }
        int phony_block = 0;
        create_inode(disk, inode, EXT2_S_IFLNK, 0, 1, 0, (unsigned int *) &phony_block, 1);
        update_bitmap(disk, inode, 1, INODE);

        struct ext2_group_desc *group_descriptor = get_group_descriptor(disk);
        group_descriptor->bg_free_inodes_count--;

        struct ext2_super_block* super_block = get_super_block(disk);
        super_block->s_free_inodes_count--;

        int block_id = add_block_file(disk, inode, strlen(real_file_path));
        unsigned char *data_block = (disk + EXT2_BLOCK_SIZE * block_id);
        memcpy(data_block, real_file_path, strlen(real_file_path));
        data_block[strlen(real_file_path) + 1] = '\0';
    }

    int parent_inode_num;
    if(dest_result.parent_block_num < 0 || dest_result.parent_offset < 0){
        //Then the parent is the root.
        parent_inode_num = EXT2_ROOT_INO;
    }else{
        struct ext2_dir_entry *parent_dir_entry = (struct ext2_dir_entry *)(disk + EXT2_BLOCK_SIZE * dest_result.parent_block_num + dest_result.parent_offset);
        parent_inode_num = parent_dir_entry->inode;
    }
    //Traverse down path to get new directory name.
    PathNode *cur = dest_path;
    while(cur){
        if(!cur->next){
            break;
        }
        cur = cur->next;
    }

    int len = strlen(cur->filename);
    int dir_result;
    if(type == HARDLINK){
        dir_result = create_dir_entry(disk, parent_inode_num, source_result.inode_num, len, source_result.file_type, cur->filename);
    }else{
        dir_result = create_dir_entry(disk, parent_inode_num, inode, len, EXT2_FT_SYMLINK, cur->filename);
    }

    if(dir_result == -ENOSPC){
        int create_result = add_block(disk, parent_inode_num);
        if(create_result < 0){
            fprintf(stderr, "%s: error %d insufficient space.\n", argv[1], create_result);
            free(real_file_path);
            free(dest_file_path);
            destroy_path_list(source_path);
            destroy_path_list(dest_path);
            return -create_result;
        }else{
            if(type == HARDLINK){
                dir_result = create_dir_entry(disk, parent_inode_num, source_result.inode_num, len, source_result.file_type, cur->filename);
            }else{
                dir_result = create_dir_entry(disk, parent_inode_num, inode, len, EXT2_FT_SYMLINK, cur->filename);
            }
        }
    }

    save_image(disk);
    free(real_file_path);
    free(dest_file_path);
    destroy_path_list(source_path);
    destroy_path_list(dest_path);

    return 0;
}
