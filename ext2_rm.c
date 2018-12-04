#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include "ext2.h"
#include "ext2_helper_modified.h"
#include <time.h>

unsigned char *disk;
unsigned char *block_bit_map;
unsigned char *inode_bit_map;
struct ext2_inode *inode_table;
int table_size;
struct ext2_super_block *sb;
struct ext2_group_desc *gd;
int table_start;
int dblock_size;
 
/* Need to check if all dirs on path exists. */
int check_path(char* path){
    /* Modify the origin input. */
    if(path[0] == '.' && path[1] == '/'){
        path = &path[2];
    } else if(path[0] == '/'){
        path = &path[1];
    }
    int length = strlen(path);
    char path_to_check[length + 1];
    memset(path_to_check, '\0', sizeof(path_to_check));
    int level, res;
    strncpy(path_to_check, path, length);

    /* Remove the trailing back slashes*/
    int finished = 0;
    length = length - 1;
    while(! finished){
        if(path_to_check[length] == '/'){
            path_to_check[length] = '\0';
            length -= 1;
        } else {
            finished = 1;
        }
    }
    /* Handle the special case. */
    if(strcmp(path_to_check, "lost+found") == 0){
        return -1;
    }
    length = strlen(path_to_check) - 1;
    
    /* Retrive the last directory name to be created. */
    /* Special case when want to create a dir at the root.*/
    finished = 0;
    while( !finished && length >= 1){
        if(path_to_check[length] == '/'){
            length -= 1;
            finished = 1;
        } else {
            length -= 1;
        }
    }

    /* If length is 0, want to return the inode number of the root.*/
    if(length == 0){
        return 2;
    }
    level = compute_level(path_to_check) - 1;
    res = cd(&(path_to_check[0]), gd->bg_inode_table, level, 'd');
    return res;
}

 
 /* A helper function that checks whether the input is valid. */
int check_syntax(char* path){
    if(path[0] == '.' && path[1] == '/'){
        path = &path[2];
    } else if(path[0] == '/'){
        path = &path[1];
    }
    int length = strlen(path);
    char path_to_check[length + 1];
    memset(path_to_check, '\0', sizeof(path_to_check));
    strncpy(path_to_check, path, length);
    if(strlen(path_to_check) == 0){
        return -1;
    }
    length = strlen(path_to_check);
    
    /* Then check if the name is too long. */
    int finished = 0;
    length = length - 1;
    while(! finished){
        if(path_to_check[length] == '/'){
            path_to_check[length] = '\0';
            length -= 1;
        } else {
            finished = 1;
        }
    }

    length = strlen(path_to_check) - 1;
    finished = 0;
    while( !finished && length >= 1){
        if(path_to_check[length] == '/'){
            path_to_check[length] = '\0';
            length -= 1;
            finished = 1;
        } else {
            length -= 1;
        }
    }
    if(strlen(path_to_check) - length + 1 > EXT2_NAME_LEN){
        return -1;
    }
    return 0;
}
 
/* A function checks if there already exists a file in here. */
int check_duplicate(char* path, char type){
    if(path[0] == '.' && path[1] == '/'){
        path = &path[2];
    } else if (path[0] == '/'){
        path = &path[1];
    }
    int length = strlen(path);
    int level, res;
    char path_to_check[length];
    memset(path_to_check, '\0', sizeof(path_to_check));
    strncpy(path_to_check, path, length);

    /* Remove the trailing back slashes*/
    int finished = 0;
    length = length - 1;
    while(! finished && length >= 1){
        if(path_to_check[length] == '/'){
            path_to_check[length] = '\0';
            length -= 1;
        } else {
            finished = 1;
        }
    }
    
    level = compute_level(path_to_check);
    res = cd(&(path_to_check[0]), gd->bg_inode_table, level, type);
    if(res < 0 && type != 'd'){
        res = cd(&(path_to_check[0]), gd->bg_inode_table, level, 'l');
    }
    return res;
}

/* A helper function, given a path, return the name of the last not null string.*/
void get_name(char** path){
    int finished = 0;
    int length = strlen(*path) - 1;
    
    if((*path)[0] == '.' && (*path)[1] == '/'){
        *path = &(*path)[2];
    } else if ((*path)[0] == '/'){
        *path = &(*path)[1];
    }
    
    if(strlen(*path) <= 2){
        if(strcmp(*path, ".") == 0 || strcmp(*path, "..") == 0){
            return;
        }
    }
    
    /* Retrive the following '/' */
    while(! finished){
        if((*path)[length] == '/'){
            (*path)[length] = '\0';
            length -= 1;
        } else {
            finished = 1;
        }
    }
    
    /* Retrive the last directory name to be created. */
    finished = 0;
    while( !finished && length > 0){
        if((*path)[length] == '/'){
            (*path) = &(*path)[length + 1];
            finished = 1;
        } else {
            length -= 1;
        }
    }
}

/*
 * A helper function to free the entry block.
 */
int free_dir_entry(int del, char* name, char d_type){
    struct ext2_dir_entry* next;
    struct ext2_dir_entry* previous;
    int read_count = 0;
    
    next = (struct ext2_dir_entry *)(disk + EXT2_BLOCK_SIZE * (del));
    
    /* Continue search while does not find */
    int check, check2;
    if(d_type == 'f' || d_type == 'l'){
        check = EXT2_FT_DIR;
        check2 = EXT2_FT_DIR;
    } else if (d_type == 'd'){
        check = EXT2_FT_REG_FILE;
        check2 = EXT2_FT_SYMLINK;
    } 
    if(strcmp(next->name, name) == 0 && next->rec_len > EXT2_NAME_LEN + 8){
        return 0;
    }
    //BREAK NAME NOT EQUALS PROBLEM
    while(strcmp(next->name, name) != 0 || (next->file_type == check || next->file_type == check2)){
        read_count += next->rec_len;
        previous = next;
        next = (struct ext2_dir_entry*)(disk + EXT2_BLOCK_SIZE * (del) + read_count);
        next->name[next->name_len] = '\0';
    }
    
    
    previous->rec_len += next->rec_len;
    
    /* Update next's rec_len is it is not the last entry. */
    if(read_count + next->rec_len != EXT2_BLOCK_SIZE){
        next->rec_len = calculate_reclen(next);
    }
    
    return 0;
}

int free_indirect_dblock(int level, int block_num, char type){
    int i;
    if(level == 2){
    // Indirect block.
        unsigned int* blocks = (unsigned int*)(disk + EXT2_BLOCK_SIZE * (block_num));
        for(i = 0; i < 256; i++){
            if(blocks[i] == 0){
                return 0;
            }
            free_map(type, blocks[i]);
        }
    } else if(level == 3){
    // Double indirect block.
        unsigned int* indir_blocks = (unsigned int*)(disk + EXT2_BLOCK_SIZE * (block_num));
        for(i = 0; i < 256; i++){
            if(indir_blocks[i] == 0){
                return 0;
            }
            unsigned int* blocks = (unsigned int*)(disk + EXT2_BLOCK_SIZE * (indir_blocks[i]));
            for(int j = 0; j < 256; j++){
               if(blocks[j] == 0){
                    return 0;
                } 
                free_map(type, blocks[j]);
            }
            
        }
    } else if (level == 4){
        int* b1 =  (int*)(disk + EXT2_BLOCK_SIZE * (block_num));
        for(i = 0; i < 256; i++){
            if(b1[i] == 0){
                return 0;
            }
            unsigned int* b2 = (unsigned int*)(disk + EXT2_BLOCK_SIZE * (b1[i]));
            for(int j = 0; j < 256; j++){
                if(b2[j] == 0){
                    return 0;
                }
                unsigned int* b3 = (unsigned int*)(disk + EXT2_BLOCK_SIZE * (b2[j]));
                for(int k = 0; k < 256; k++){
                    if(b3[k] == 0){
                        return 0;
                    } 
                    free_map(type, b3[k]);
                }
            }
            
        }
    }
    return 0;
}


int delete(int parent_inode_num, int del_file_inum, char* path, char d_type){
    struct ext2_inode* parent_inode= &inode_table[parent_inode_num - 1];
    struct ext2_inode* del_inode = &inode_table[del_file_inum - 1];
    int del_dblock;
    int i, num_blocks;
    /* Get the name of given path. */
    get_name(&path);
    num_blocks = del_inode->i_blocks/2;
    
    /* Go to the parent's directory block to delete the path. */
    /* del_dblock is the data block number that contains del file. */
    del_dblock = sen_in_inode(path, strlen(path) - 1, *parent_inode, d_type);
    if(del_dblock < 0 && d_type == 'f'){
        del_dblock = sen_in_inode(path, strlen(path) - 1, *parent_inode, 'l');
    }
    free_dir_entry(del_dblock, path, d_type);
    
    /* Decrease the count of the link of del_inode by 1. */
    del_inode -> i_links_count -= 1;
    if(del_inode -> i_links_count > 0){
        return 0;
    }
    del_inode -> i_dtime = time(NULL);
    char type;
    
    /* Want to increase the number of free blocks by 1. */
    /* Free bitmap if neceessary. */
    for(i = 0; i < num_blocks; i++){
        type = 'd';
        free_map(type, (del_inode->i_block)[i]);
        if(i >= 12){
            /* Free data blocks. */
            free_indirect_dblock(i - 10, del_inode->i_block[i], type);
            /* Free the indirect blocks. */
            free_map(type, (del_inode->i_block)[i]);
        } 
        increase_free_blocks();
    }
    if(del_inode->i_links_count == 0){
        type = 'i';
        free_map(type, del_file_inum);
        increase_free_inodes();
    }
     
    return 0;
}

int main(int argc, char **argv) {
    int parent_inode, del_file_inode;
    if(argc != 3){
        printf("Usage:<disk> <absolute path to the deleted file/link>\n");
        exit(1);
    }
    char* path = argv[2];
    init_disk(argc, argv);
    
    /* Check if input is valid. */
    if(check_syntax(path) < 0){
        return -EINVAL;
    }
    
    /* Call cd to check if path is valid. */
    parent_inode = check_path(path);
    if(parent_inode == -1){
        return -ENOENT;
    }
    
    /* Get the inode of required file/link. */
    del_file_inode = check_duplicate(path, 'f');
    if(del_file_inode < 0){
        exit(1);
    }

    /* Delete the dir_entry. */
    delete(parent_inode, del_file_inode, path , 'f');
    
    return 0;
}
 
 
