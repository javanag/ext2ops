#include "helper.h"

unsigned char *disk;

int main(int argc, char **argv) {
    if(argc != 4) {
        fprintf(stderr, "Usage: %s <image file name> <path to source file> <path to dest>\n", argv[0]);
        exit(1);
    }
    disk = load_image(argv[1]);
    if(!disk){
        perror("Failed to open disk image.");
        exit(1);
    }

    //Try to open file in local machine, if it doesnt work then quit (ENOENT)
    //Also quit if the destination has a bad path, (same conds as mkdir) (ENOENT)
    //If dest file already exists -> EEXIST
    int source_file_descriptor = open(argv[2], O_RDONLY);
    if(source_file_descriptor < 0){
        fprintf(stderr, "%s: error %d unable to open source file.\n", argv[2], ENOENT);
        return ENOENT;
    }

    PathNode *path = create_path_list(argv[3]);
    SearchResult result = find_dir_entry(disk, path, FALSE);

    if(result.error_code >= 0 && result.file_type != EXT2_FT_DIR){
        //fprintf(stderr, "%s: error %d file already exists.\n", path_string+1, EEXIST);
        destroy_path_list(path);
        return EEXIST;

    }else if((result.error_code >= 0 && result.file_type == EXT2_FT_DIR) || (result.parent_offset == -1 && result.parent_block_num == -1 && result.extra_info == JUST_ROOT)){
        //Special case where the dest is just a dir, we will use the same name as
        //the source file, but if that name already exists we must throw EEXIST.
        //Just do it the lazy way using already created workflow.
        char *source_copy = strdup(argv[2]);
        PathNode *source_path = create_path_list(argv[2]);
        PathNode *cur = source_path;
        while(cur){
            if(!cur->next){
                break;
            }
            cur = cur->next;
        }

        char* new_path_base = NULL;
        if(result.softlink_path){
            new_path_base = result.softlink_path;
        }else{
            new_path_base = argv[3];
        }

        int string_length = strlen(new_path_base) + strlen(cur->filename) + 2;
        char *new_path_string = malloc(string_length);
        strncat(new_path_string, new_path_base, strlen(new_path_base));
        strncat(new_path_string, "/", 1);
        strncat(new_path_string, cur->filename, strlen(cur->filename));
        new_path_string[string_length] = '\0';

        if(path){
            destroy_path_list(path);
        }
        path = create_path_list(new_path_string);
        SearchResult new_result = find_dir_entry(disk, path, FALSE);

        destroy_path_list(source_path);
        free(source_copy);
        free(new_path_string);

        //Want the result to be missing file type:
        if(new_result.error_code >= 0 || new_result.extra_info != MISSING_FILE){
            destroy_path_list(path);
            return -new_result.error_code;
        }
        result = new_result;

    }else{
        if(result.extra_info == BAD_PATH){
            //fprintf(stderr, "%s: error %d bad path given.\n", path_string+1, result.error_code);
            destroy_path_list(path);
            return -result.error_code;
        }else if(result.extra_info == MISSING_FILE){
            //This is what we want.
        }else{
            destroy_path_list(path);
            return -result.error_code;
        }
    }

    int inode = get_free_inode(disk);
    if(inode < 0){
        fprintf(stderr, "%s: error %d insufficient space.\n", argv[1], inode);
        destroy_path_list(path);
        return inode;
    }
    //Create an inode for the new file, start it out at size 0, link 1, and no blocks.
    int phony_block = 0;
    create_inode(disk, inode, EXT2_S_IFREG, 0, 1, 0, (unsigned int *) &phony_block, 1);
    //Maybe move the following into create inode func?
    update_bitmap(disk, inode, 1, INODE);
    struct ext2_group_desc *group_descriptor = get_group_descriptor(disk);
    group_descriptor->bg_free_inodes_count--;

    struct ext2_super_block* super_block = get_super_block(disk);
    super_block->s_free_inodes_count--;

    unsigned char* buffer[EXT2_BLOCK_SIZE];
    int bytes_read = 0, cursor = 0;
    //error check following:
    int block_id = add_block_file(disk, inode, 0);
    struct ext2_inode *inode_obj = get_inode(disk, inode);
    int extra_block = FALSE;
    int file_size = 0;
    while((bytes_read = read(source_file_descriptor, buffer, EXT2_BLOCK_SIZE)) > 0){
        file_size += bytes_read;
    }
    //Check if there are enough blocks for this file:
    int blocks_required = 0;
    if(file_size % EXT2_BLOCK_SIZE == 0){
        blocks_required = file_size / EXT2_BLOCK_SIZE;
    }else{
        blocks_required = (file_size / EXT2_BLOCK_SIZE) + 1;
    }
    if(blocks_required > 11){
        blocks_required++;
    }
    if(super_block->s_free_blocks_count <= blocks_required || group_descriptor->bg_free_blocks_count <= blocks_required){
        fprintf(stderr, "%s: error %d insufficient space.\n", argv[1], -ENOSPC);
        destroy_path_list(path);
        return -ENOSPC;
    }
    //Rewind to beginning of file to do reading.
    lseek(source_file_descriptor, 0, SEEK_SET);
    while((bytes_read = read(source_file_descriptor, buffer, EXT2_BLOCK_SIZE)) > 0){
        if(bytes_read < 0){
            fprintf(stderr, "%s: error reading source file.\n", argv[2]);
            destroy_path_list(path);
            exit(1);
        }
        unsigned char *data_block = (disk + EXT2_BLOCK_SIZE * block_id);
        if(cursor + bytes_read >= EXT2_BLOCK_SIZE){
            //First, copy the portion that fits in the current block:
            memcpy(data_block + cursor, buffer, (bytes_read - ((cursor + bytes_read) % EXT2_BLOCK_SIZE)));
            //Fetch new block, reset cursor, copy what remains for new block:
            block_id = add_block_file(disk, inode, bytes_read);
            if((cursor + bytes_read) % EXT2_BLOCK_SIZE == 0){
                extra_block = TRUE;
            }else{
                memcpy(data_block, buffer + (bytes_read - ((cursor + bytes_read) % EXT2_BLOCK_SIZE)), (cursor + bytes_read) % EXT2_BLOCK_SIZE);
                extra_block = FALSE;
            }
            cursor = (cursor + bytes_read) % EXT2_BLOCK_SIZE;
        }else{
            //Just copy and update cursor:
            extra_block = FALSE;
            memcpy(data_block + cursor, buffer, bytes_read);
            cursor += bytes_read;
            inode_obj->i_size += bytes_read;
        }
    }

    if(extra_block){
        remove_last_block(disk, inode);
    }

    int parent_inode_num;
    if(result.parent_block_num < 0 || result.parent_offset < 0){
        //Then the parent is the root.
        parent_inode_num = EXT2_ROOT_INO;
    }else{
        struct ext2_dir_entry *parent_dir_entry = (struct ext2_dir_entry *)(disk + EXT2_BLOCK_SIZE * result.parent_block_num + result.parent_offset);
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
    int dir_result = create_dir_entry(disk, parent_inode_num, inode, strlen(cur->filename), EXT2_FT_REG_FILE, cur->filename);
    if(dir_result == -ENOSPC){
        int create_result = add_block(disk, parent_inode_num);
        if(create_result < 0){
            fprintf(stderr, "%s: error %d insufficient space.\n", argv[1], create_result);
            destroy_path_list(path);
            return -create_result;
        }else{
            create_dir_entry(disk, parent_inode_num, inode, strlen(cur->filename), EXT2_FT_REG_FILE, cur->filename);
        }
    }

    save_image(disk);
    destroy_path_list(path);

    return 0;
}
