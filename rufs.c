/*
 *  Copyright (C) 2023 CS416 Rutgers CS
 *	Tiny File System
 *	File:	rufs.c
 *
 *  Richard Li
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <libgen.h>
#include <limits.h>
#include <stdarg.h>

#include "block.h"
#include "rufs.h"

char diskfile_path[PATH_MAX];

#define MAX_NAME_LEN 256
#define INODECOUNT (BLOCK_SIZE / sizeof(struct inode))

static bitmap_t bBitMap;
static bitmap_t iBitMap;

static struct superblock *sBlock;

// int setFree(unsigned char * structure){
// 	for(int i=0;i<(BLOCK_SIZE *8);i++){
// 		if(get_bitmap(structure,i)==0){
// 			set_bitmap(structure,i);
// 			return i;
// 		}
// 	}
// 	return 0;
// }

// logging function for debugging
static void rufs_debug(const char *fmt, ...) {

    va_list argp;

    va_start(argp, fmt);
    fprintf(stdout, "(DEBUG) ");

    vfprintf(stdout, fmt, argp);
    va_end(argp);

    fprintf(stdout, " \n");

}

/* 
 * Get available inode number from bitmap
 */
int get_avail_ino() {

	int inode_num;

    //rufs_debug("Searching for available inode");

    // Step 1: Read the inode bitmap from disk
    if (!bio_read(sBlock->i_bitmap_blk, iBitMap)) {
        rufs_debug("Failed to read inode bitmap from disk");
        return -EIO;
    }

    // Step 2: Traverse the inode bitmap to find a free inode
    for (inode_num = 0; inode_num < MAX_INUM; inode_num++) {
        if (!get_bitmap(iBitMap, inode_num)) {
            // Step 3: Mark the inode as used in the bitmap
            set_bitmap(iBitMap, inode_num);
            if (!bio_write(sBlock->i_bitmap_blk, iBitMap)) {
                rufs_debug("Failed to write inode bitmap to disk");
                return -EIO;
            }

            //rufs_debug("Allocated inode %d", inode_num);
            return inode_num;
        }
    }

    //rufs_debug("No available inode found");
    return -ENOSPC; // No space available
}

/* 
 * Get available data block number from bitmap
 */
int get_avail_blkno() {

	int block_num;

    //rufs_debug("Searching for available data block");

    // Step 1: Read the data block bitmap from disk
    if (!bio_read(sBlock->d_bitmap_blk, bBitMap)) {
        rufs_debug("Failed to read data block bitmap from disk");
        return -EIO;
    }

    // Step 2: Traverse the data block bitmap to find a free block
    for (block_num = 0; block_num < MAX_DNUM; block_num++) {
        if (!get_bitmap(bBitMap, block_num)) {
            // Step 3: Mark the block as used in the bitmap
            set_bitmap(bBitMap, block_num);
            if (!bio_write(sBlock->d_bitmap_blk, bBitMap)) {
                rufs_debug("Failed to write data block bitmap to disk");
                return -EIO;
            }

            // Calculate and return the absolute block number
            int absolute_block_num = block_num + sBlock->d_start_blk;
            //rufs_debug("Allocated data block %d (absolute block %d)", block_num, absolute_block_num);
            return absolute_block_num;
        }
    }

    //rufs_debug("No available data block found");
    return -ENOSPC; // No space available
}

/* 
 * inode operations
 */
int readi(uint16_t ino, struct inode *inode) {

	struct inode *block_buffer;

	int inode_num;
	int offset;

	//rufs_debug("Writing inode %d to disk", ino);

	block_buffer = malloc(BLOCK_SIZE);
	if (!block_buffer) {
		rufs_debug("Unable to allocate memory for block buffer");
		return -ENOMEM;
	}

    // Step 1: Get the inode's on-disk block number
    inode_num = sBlock->i_start_blk + ((ino * sizeof(struct inode)) / BLOCK_SIZE);

    // Step 2: Get offset of the inode in the inode on-disk block
	offset = ino % INODECOUNT;

    // Step 3: Read the block from disk and then copy into inode structure
	bio_read(inode_num, block_buffer);
	*inode = block_buffer[offset];
	free(block_buffer);
	return 0;

}

int writei(uint16_t ino, struct inode *inode) {

	struct inode *block_buffer;

	int inode_num;
	int offset;

	block_buffer = malloc(BLOCK_SIZE);
	if (!block_buffer)
		return -ENOMEM;

	// Step 1: Get the block number where this inode resides on disk
    inode_num = sBlock->i_start_blk + ((ino * sizeof(struct inode)) / BLOCK_SIZE);

	// Step 2: Get the offset in the block where this inode resides on disk
	offset = ino % INODECOUNT;

	// Step 3: Write inode to disk 
	bio_read(inode_num, block_buffer);

	block_buffer[offset] = *inode;

	bio_write(inode_num, block_buffer);

	free(block_buffer);
	return 0;
}


/* 
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {
	
	struct inode dir_inode;
    struct dirent *entry_block;

	int entry_index;

    //rufs_debug("Searching for file '%s' in directory inode %d", fname, ino);

    // Step 1: Call readi() to get the inode using ino (inode number of current directory)
    if (readi(ino, &dir_inode) < 0) {
        rufs_debug("Failed to read directory inode %d", ino);
        return -ENOENT;
    }

    // Step 2: Get data block of current directory from inode
    entry_block = malloc(BLOCK_SIZE);
    if (!entry_block) {
        rufs_debug("Unable to allocate memory for directory entries");
        return -ENOMEM;
    }

    // Step 3: Read directory's data block and check each directory entry.
	int i;
    for (i = 0; i < 16 && dir_inode.direct_ptr[i]; i++) {
        bio_read(dir_inode.direct_ptr[i], entry_block);

        // Step 4: Check each directory entry in the block
        for (entry_index = 0; entry_index < (BLOCK_SIZE / sizeof(struct dirent)); entry_index++) {
            struct dirent *entry = &entry_block[entry_index];
            //If the name matches, then copy directory entry to dirent structure
            if (entry->valid && strncmp(entry->name, fname, name_len) == 0) {
                *dirent = *entry;
                free(entry_block);
                return 0;
            }
        }
    }

    free(entry_block);
    return -ENOENT;
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {	

	struct dirent *entry_block;

	int entry_index;

    //rufs_debug("Adding file '%s' with inode %d to directory inode %d", fname, f_ino, dir_inode.ino);

	// Step 2: Check if fname (directory name) is already used in other entries
    struct dirent existing_entry;
    if (dir_find(dir_inode.ino, fname, name_len, &existing_entry) == 0) {
        rufs_debug("File '%s' already exists in directory inode %d", fname, dir_inode.ino);
        return -EEXIST;
    }

	// Step 1: Read dir_inode's data block and check each directory entry of dir_inode
    entry_block = malloc(BLOCK_SIZE);
    if (!entry_block) {
        rufs_debug("Unable to allocate memory for directory entries");
        return -ENOMEM;
    }

	int i;
    for (i = 0; i < 16; i++) {
        if (!dir_inode.direct_ptr[i]) {
            // Allocate a new data block if needed
            dir_inode.direct_ptr[i] = get_avail_blkno();
            dir_inode.size += BLOCK_SIZE;
            dir_inode.link++;
            writei(dir_inode.ino, &dir_inode);
        }

        // Step 3: Add directory entry in dir_inode's data block and write to disk
        bio_read(dir_inode.direct_ptr[i], entry_block);

        for (entry_index = 0; entry_index < (BLOCK_SIZE / sizeof(struct dirent)); entry_index++) {
            struct dirent *entry = &entry_block[entry_index];
            if (!entry->valid) {
                // Add the new directory entry
                entry->ino = f_ino;
                entry->valid = 1;
                strncpy(entry->name, fname, name_len);
                entry->name[name_len] = '\0';
                entry->len = name_len;

                bio_write(dir_inode.direct_ptr[i], entry_block);
                free(entry_block);
                return 0;
            }
        }
    }

    free(entry_block);
    return -ENOSPC; // No space available in the directory
}

int dir_remove(struct inode dir_inode, const char *fname, size_t name_len) {

	// Step 1: Read dir_inode's data block and checks each directory entry of dir_inode
	
	// Step 2: Check if fname exist

	// Step 3: If exist, then remove it from dir_inode's data block and write to disk

	return 0;
}

/* 
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {

    // Step 1: Resolve the path name, walk through path, and finally, find its inode.
	// Note: You could either implement it in a iterative way or recursive way
	struct dirent dir_entry = {0};

	char *current_component;
	char *holder;
    char *path_copy;

    // Check if the path is root and resolve immediately
    if (!strcmp(path, "/")) {
        return readi(0, inode); // Directly read the root inode
    }

    // Duplicate the path for tokenization
    path_copy = strdup(path);
    if (!path_copy) {
        return -ENOMEM; // Memory allocation failed
    }

    holder = path_copy;
    // Tokenize the path and traverse each component
    while ((current_component = strsep(&path_copy, "/")) != NULL) {
        if (*current_component) {
            if (dir_find(dir_entry.ino, current_component, strlen(current_component), &dir_entry) < 0) {
                free(holder); // Clean up duplicated string
                return -ENOENT; // Component not found
            }
        }
    }

    free(holder);
    // The final directory entry should point to the resolved inode
    return readi(dir_entry.ino, inode);
}


// void fillBlock(unsigned char * block){
// 	int numInodes = BLOCK_SIZE/sizeof(struct inode);
// 	struct inode * start = (struct inode *)block;
// 	struct inode * inod = (struct inode *) malloc(sizeof(struct inode));
// 	for(int i=0;i<numInodes;i++){
// 		memcpy(&start[i],inod,sizeof(struct inode));  
// 	}
// 	free(inod);
// }

// void writeToDisk(int a, int b, int isInodeTable){
// 	unsigned char buff[BLOCK_SIZE];
// 	memset(buff,0,sizeof(BLOCK_SIZE));
// 	if(isInodeTable == 1){
// 		fillBlock(buff);
// 	}
// 	for(int i= a;i<b;i++){
// 		bio_write(i,buff);
// 	}
// }
// void fillRoot(int a){
// 	unsigned char buff[BLOCK_SIZE];
//     bio_read(a, buff); // Reads the first block of the inode table into buff
//     struct inode *start = (struct inode *)buff;

//     // Initialize root inode
//     start[0].ino = 0;                    // Root inode number
//     start[0].valid = 1;                  // Mark as valid
//     start[0].type = 1;                   // Directory type
//     start[0].size = 2 * sizeof(struct dirent); // Size: two entries ('.' and '..')
//     start[0].link = 2;                   // Link count for root directory (self + parent)

//     int root_block = get_avail_blkno(); // Allocate a data block for root entries
//     if (root_block == -1) {
//         rufs_debug("Error: No available block for root directory");
//         start[0].direct_ptr[0] = 0; // Set to 0 to prevent invalid block accesses
//         return; // Fail initialization gracefully
//     }
//     start[0].direct_ptr[0] = root_block;

//     for (int i = 1; i < 16; i++)         // Initialize remaining direct pointers to 0
//         start[0].direct_ptr[i] = 0;

//     // Initialize default "." and ".." entries for the root directory
//     unsigned char root_block_data[BLOCK_SIZE] = {0};
//     struct dirent *entries = (struct dirent *)root_block_data;

//     entries[0].ino = 0;                  // Self
//     entries[0].valid = 1;
//     strncpy(entries[0].name, ".", sizeof(entries[0].name));

//     entries[1].ino = 0;                  // Parent (self for root)
//     entries[1].valid = 1;
//     strncpy(entries[1].name, "..", sizeof(entries[1].name));

//     if (bio_write(start[0].direct_ptr[0], root_block_data) < 0) {
//         rufs_debug("Error: Failed to write root directory data block %d", start[0].direct_ptr[0]);
//     }

//     if (bio_write(a, buff) < 0) {
//         rufs_debug("Error: Failed to write root inode to block %d", a);
//     }

//     rufs_debug("Root directory initialized: direct_ptr[0]=%d", start[0].direct_ptr[0]);

// }


/* 
 * Make file system
 */
int rufs_mkfs() {

	struct inode *root_inode;
    struct dirent *root_dir;

	// Call dev_init() to initialize (Create) Diskfile
    //rufs_debug("Initializing disk at path: %s", diskfile_path);
    dev_init(diskfile_path);

	// write superblock information
    sBlock = malloc(BLOCK_SIZE);
    if (!sBlock) {
        rufs_debug("Unable to allocate memory for superblock");
        return -ENOMEM;
    }
    *sBlock = (struct superblock){
        .magic_num = MAGIC_NUM,
        .max_inum = MAX_INUM,
        .max_dnum = MAX_DNUM,
        .i_bitmap_blk = 1,
        .d_bitmap_blk = 2,
        .i_start_blk = 3,
        .d_start_blk = 3 + (sizeof(struct inode) * MAX_INUM / BLOCK_SIZE),
    };

    //rufs_debug("Writing superblock to disk");
    bio_write(0, sBlock);

	// initialize inode bitmap
    // initialize data block bitmap
    iBitMap = calloc(1, BLOCK_SIZE);
    bBitMap = calloc(1, BLOCK_SIZE);
    if (!iBitMap || !bBitMap) {
        rufs_debug("Unable to allocate memory for bitmaps");
        return -ENOMEM;
    }

	// update bitmap information for root directory
    set_bitmap(iBitMap, 0);
    set_bitmap(bBitMap, 0);
    bio_write(sBlock->i_bitmap_blk, iBitMap);
    bio_write(sBlock->d_bitmap_blk, bBitMap);
    //rufs_debug("Root inode and data block marked as used in bitmaps");

	// update inode for root directory
    root_inode = malloc(BLOCK_SIZE);
    if (!root_inode) {
        rufs_debug("Unable to allocate memory for root inode");
        return -ENOMEM;
    }
    memset(root_inode, 0, sizeof(struct inode));

    root_inode->ino = 0;
    root_inode->valid = 1;
    root_inode->size = 2 * sizeof(struct dirent);  // For "." and ".."
    root_inode->type = 1;
    root_inode->link = 2;  // "." and ".."
    root_inode->direct_ptr[0] = sBlock->d_start_blk;

	int i;
    for (i = 1; i < 16; i++) root_inode->direct_ptr[i] = 0;

    bio_write(sBlock->i_start_blk, root_inode);

    // root directory entries
    root_dir = malloc(BLOCK_SIZE);
    if (!root_dir) {
        rufs_debug("Unable to allocate memory for root directory");
        return -ENOMEM;
    }
    memset(root_dir, 0, BLOCK_SIZE);

    root_dir[0] = (struct dirent){.ino = 0, .valid = 1, .name = ".", .len = 1};
    root_dir[1] = (struct dirent){.ino = 0, .valid = 1, .name = "..", .len = 2};

    bio_write(sBlock->d_start_blk, root_dir);

    // free allocated memory
    free(root_inode);
    free(root_dir);

    //rufs_debug("Filesystem successfully created");
    return 0;
}


/* 
 * FUSE file operations
 */
static void *rufs_init(struct fuse_conn_info *conn) {

	// Step 1a: If disk file is not found, call mkfs
    if (dev_open(diskfile_path) < 0) {
        rufs_debug("Disk file not found. Creating new filesystem...");
        rufs_mkfs();
        return NULL;
    }

    // Step 1b: If disk file is found, just initialize in-memory data structures
    // and read superblock from disk
    sBlock = malloc(BLOCK_SIZE);
    iBitMap = malloc(BLOCK_SIZE);
    bBitMap = malloc(BLOCK_SIZE);

    if (!sBlock || !iBitMap || !bBitMap) {
        rufs_debug("Failed to allocate memory for in-memory structures");
        return NULL;
    }

    if (!bio_read(0, sBlock) || !bio_read(sBlock->i_bitmap_blk, iBitMap) || !bio_read(sBlock->d_bitmap_blk, bBitMap)) {
        rufs_debug("Failed to load filesystem metadata and bitmaps from disk");
        return NULL;
    }

    //rufs_debug("Filesystem successfully initialized from existing disk file");
    return NULL;
}

static void rufs_destroy(void *userdata) {

	//rufs_debug("Destroying RUFS and freeing memory");

	// Step 1: De-allocate in-memory data structures
    if (sBlock) {
        free(sBlock);
        rufs_debug("Freed superblock");
    }
    if (iBitMap) {
        free(iBitMap);
        rufs_debug("Freed inode bitmap");
    }
    if (bBitMap) {
        free(bBitMap);
        rufs_debug("Freed data bitmap");
    }

	// Step 2: Close diskfile
    dev_close();
    //rufs_debug("Disk file closed");
}

static int rufs_getattr(const char *path, struct stat *stbuf) {

	struct inode target_inode;

	// Step 1: call get_node_by_path() to get inode from path
    if (get_node_by_path(path, 0, &target_inode)) {
        return -ENOENT;
    }

	// Step 2: fill attribute of file into stbuf from inode
    *stbuf = target_inode.vstat;
    return 0;
}

static int rufs_opendir(const char *path, struct fuse_file_info *fi) {

	struct inode directory_inode;
	// Step 1: Call get_node_by_path() to get inode from path
	int result = get_node_by_path(path, 0, &directory_inode);

	if (result != 0) {
	    // Step 2: If not find, return -1
    	return -1;
	} else {
    	return result;
	}

}

static int rufs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

	struct inode dir_inode;
    struct dirent *entry_buffer;

    size_t num_direct_ptrs;
    size_t dirents_in_block;

    int block_idx;
	int entry_idx;

    //rufs_debug("Reading directory contents for path: %s", path);

	// Step 1: Call get_node_by_path() to get inode from path
    if (get_node_by_path(path, 0, &dir_inode) < 0) {
        rufs_debug("Directory %s not found", path);
        return -ENOENT;
    }

    // calculate the number of direct pointers 
    num_direct_ptrs = sizeof(dir_inode.direct_ptr) / sizeof(dir_inode.direct_ptr[0]);
    // calculate the number of dirent structures that fit in one block
    dirents_in_block = BLOCK_SIZE / sizeof(struct dirent);

    entry_buffer = malloc(BLOCK_SIZE);
    if (!entry_buffer) {
        rufs_debug("Unable to allocate memory for directory entry buffer");
        return -ENOMEM;
    }

	// Step 2: Read directory entries from its data blocks, and copy them to filler
    for (block_idx = 0; block_idx < num_direct_ptrs; block_idx++) {
        if (dir_inode.direct_ptr[block_idx] == 0) {
            continue;
        }

        // Read the data block containing directory entries
        if (bio_read(dir_inode.direct_ptr[block_idx], entry_buffer) < 0) {
            rufs_debug("Failed to read directory data block %d", dir_inode.direct_ptr[block_idx]);
            continue;
        }

        for (entry_idx = 0; entry_idx < dirents_in_block; entry_idx++) {
            struct dirent *current_entry = &entry_buffer[entry_idx];

            if (current_entry->valid) {
                struct inode file_inode;

                if (readi(current_entry->ino, &file_inode) < 0) {
                    rufs_debug("Failed to read inode %d for entry %s", current_entry->ino, current_entry->name);
                    continue;
                }

                filler(buffer, current_entry->name, &file_inode.vstat, 0);
                //rufs_debug("Added entry: %s (inode %d)", current_entry->name, current_entry->ino);
            }
        }
    }

    free(entry_buffer);
    //rufs_debug("Finished reading directory contents for path: %s", path);

    return 0;

}


static int rufs_mkdir(const char *path, mode_t mode) {

    struct inode parent_inode, new_dir_inode;
    struct dirent *entry_buffer;
    struct stat *new_dir_stat = &new_dir_inode.vstat;

    time_t current_time;

    char *dir_copy; 
    char *base_copy;
    char *target_name;
    char *parent_path;

    int available_inode;
    int available_block;
    int check = 0;

    size_t i;

    // make copys of path strings for mathing
    dir_copy = strdup(path);
    base_copy = strdup(path);
    entry_buffer = calloc(1, BLOCK_SIZE);

    if (!base_copy || !dir_copy || !entry_buffer) {
        free(dir_copy);
        free(base_copy);
        free(entry_buffer);
        return -ENOMEM;
    }

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name
    target_name = basename(base_copy);
    parent_path = dirname(dir_copy);

    // name size check so it fits in a directory entry
    if (strlen(target_name) >= sizeof(((struct dirent *)0)->name)) {
        free(dir_copy);
        free(base_copy);
        free(entry_buffer);
        return -ENAMETOOLONG;
    }

	// Step 2: Call get_node_by_path() to get inode of parent directory
    check = get_node_by_path(parent_path, 0, &parent_inode);
    if (check) {
        free(dir_copy);
        free(base_copy);
        free(entry_buffer);
        return check;
    }

	// Step 3: Call get_avail_ino() to get an available inode number
    available_inode = get_avail_ino();
    if (available_inode < 0) {
        free(dir_copy);
        free(base_copy);
        free(entry_buffer);
        return -ENOSPC;
    }

	// Step 4: Call dir_add() to add directory entry of target directory to parent directory
    check = dir_add(parent_inode, available_inode, target_name, strlen(target_name));
    if (check) {
        free(dir_copy);
        free(base_copy);
        free(entry_buffer);
        return check;
    }

	// Step 5: Update inode for target directory
    new_dir_inode.ino = available_inode;
    new_dir_inode.valid = 1;
    new_dir_inode.link = 2; // Directories have a link count of 2 (self and parent)

    // Allocate a data block for the new directory
    available_block = get_avail_blkno();
    if (available_block < 0) {
        free(dir_copy);
        free(base_copy);
        free(entry_buffer);
        return -ENOSPC;
    }
    new_dir_inode.direct_ptr[0] = available_block;

    // Initialize remaining direct pointers to 0
    for (i = 1; i < sizeof(new_dir_inode.direct_ptr) / sizeof(new_dir_inode.direct_ptr[0]); i++) {
        new_dir_inode.direct_ptr[i] = 0;
    }

    // Initialize indirect pointers to 0
    for (i = 0; i < sizeof(new_dir_inode.indirect_ptr) / sizeof(new_dir_inode.indirect_ptr[0]); i++) {
        new_dir_inode.indirect_ptr[i] = 0;
    }

    new_dir_inode.type = 1; // set type
    
    new_dir_stat->st_mode = S_IFDIR | mode;
    new_dir_stat->st_nlink = 2; // Link count includes "." and ".."
    new_dir_stat->st_uid = getuid();
    new_dir_stat->st_gid = getgid();
    new_dir_stat->st_ino = available_inode;
    new_dir_stat->st_size = sizeof(struct dirent) * 2; // Size includes "." and ".."
    new_dir_stat->st_blocks = 1; // One block is allocated
    new_dir_stat->st_blksize = BLOCK_SIZE;
    
    time(&current_time);
    new_dir_stat->st_atime = current_time;
    new_dir_stat->st_mtime = current_time;

    memset(entry_buffer, 0, BLOCK_SIZE);
    struct dirent *entries = (struct dirent *)entry_buffer;

    // "." entry
    entries[0].ino = available_inode;
    entries[0].valid = 1;
    strncpy(entries[0].name, ".", sizeof(entries[0].name));

    // ".." entry
    entries[1].ino = parent_inode.ino;
    entries[1].valid = 1;
    strncpy(entries[1].name, "..", sizeof(entries[1].name));

    // Step 6: Call writei() to write inode to disk
    if (bio_write(available_block, entry_buffer) < 0) {
        free(dir_copy);
        free(base_copy);
        free(entry_buffer);
        return -EIO;
    }

    if (writei(available_inode, &new_dir_inode) < 0) {
        free(dir_copy);
        free(base_copy);
        free(entry_buffer);
        return -EIO;
    }

    free(dir_copy);
    free(base_copy);
    free(entry_buffer);
    return 0;
}

// static int rufs_rmdir(const char *path) {

// 	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name

// 	// Step 2: Call get_node_by_path() to get inode of target directory

// 	// Step 3: Clear data block bitmap of target directory

// 	// Step 4: Clear inode bitmap and its data block

// 	// Step 5: Call get_node_by_path() to get inode of parent directory

// 	// Step 6: Call dir_remove() to remove directory entry of target directory in its parent directory

// 	return 0;
// }

static int rufs_releasedir(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int rufs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {

	struct inode parent_inode;
    struct inode new_inode;
    struct stat *new_stat = &new_inode.vstat;

    char *path_dup1;
    char *path_dup2;
    char *parent_path;
    char *file_name;

    time_t current_time;

    int new_ino;
    int check;

    // make copies of the path for manipulation
    path_dup1 = strdup(path);
    path_dup2 = strdup(path);
    if (!path_dup1 || !path_dup2) {
        free(path_dup1);  // Free in case only one strdup succeeds
        free(path_dup2);
        return -ENOMEM;
    }

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name
    parent_path = dirname(path_dup1);
    file_name = basename(path_dup2);

    // check the target name so it isn't too long
    if (strlen(file_name) >= sizeof(((struct dirent *)0)->name)) {
        free(path_dup1);
        free(path_dup2);
        return -ENAMETOOLONG;
    }

	// Step 2: Call get_node_by_path() to get inode of parent directory
    check = get_node_by_path(parent_path, 0, &parent_inode);
    if (check) {
        free(path_dup1);
        free(path_dup2);
        return check;
    }

	// Step 3: Call get_avail_ino() to get an available inode number
    new_ino = get_avail_ino();
    if (new_ino < 0) {
        free(path_dup1);
        free(path_dup2);
        return -ENOSPC;
    }

	// Step 4: Call dir_add() to add directory entry of target file to parent directory
    check = dir_add(parent_inode, new_ino, file_name, strlen(file_name));
    if (check) {
        rufs_debug("%s: Failed to add %s to %s", __func__, file_name, parent_path);
        free(path_dup1);
        free(path_dup2);
        return check;
    }

	// Step 5: Update inode for target file
    new_inode.ino = new_ino;
    new_inode.valid = 1;
    new_inode.link = 1;  // Regular files have a link count of 1
    new_inode.direct_ptr[0] = get_avail_blkno();  // Allocate a new data block

    // calculate the number of elements in the direct_ptr array
    size_t num_direct_ptrs = sizeof(new_inode.direct_ptr) / sizeof(new_inode.direct_ptr[0]);
    for (size_t i = 1; i < num_direct_ptrs; i++) {
        new_inode.direct_ptr[i] = 0;
    }

    // calculate the number of elements in the indirect_ptr array
    size_t num_indirect_ptrs = sizeof(new_inode.indirect_ptr) / sizeof(new_inode.indirect_ptr[0]);
    for (size_t i = 0; i < num_indirect_ptrs; i++) {
        new_inode.indirect_ptr[i] = 0;
    }

    new_inode.type = 0;

    // update the inode's metadata
    new_stat->st_ino = new_ino;
    new_stat->st_size = 0;  // File size
    new_stat->st_blocks = 1;  // Number of allocated blocks
    new_inode.size = 0;  // Newly created files have size 0
    new_stat->st_mode = S_IFREG | mode;  // Regular file with the given mode
    new_stat->st_nlink = 1;  // Number of hard links

    time(&current_time);
    new_stat->st_gid = getgid();
    new_stat->st_uid = getuid();
    new_stat->st_atime = current_time;
    new_stat->st_mtime = current_time;

	// Step 6: Call writei() to write inode to disk
    writei(new_ino, &new_inode);

    free(path_dup1);
    free(path_dup2);
    return 0;
}

static int rufs_open(const char *path, struct fuse_file_info *fi) {

	struct inode file_inode;

	// Step 1: Call get_node_by_path() to get inode from path
	int result = get_node_by_path(path, 0, &file_inode);

	if (result != 0) {
	    // Step 2: If not find, return -1
    	return -1;
	} else {
    	return result;
	}

}

static int rufs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
    
    void *buff;
	struct inode file;

    int blockNumber;
	int block_offset;
	int spaceLeft;
	int byteCount = 0;

    //rufs_debug("Reading %ld bytes from file %s at offset %ld", size, path, offset);

	// Step 1: You could call get_node_by_path() to get inode from path
    if (get_node_by_path(path, 0, &file) < 0) {
        rufs_debug("File not found at path %s", path);
        return -ENOENT;
    }

    buff = malloc(BLOCK_SIZE);
    if (!buff) {
        rufs_debug("Unable to allocate memory for block buffer");
        return -ENOMEM;
    }

	// Step 2: Based on size and offset, read its data blocks from disk
    while (byteCount < size && offset < file.size) {
        // calculate the block number and offset within the block
        blockNumber = file.direct_ptr[offset / BLOCK_SIZE];
        block_offset = offset % BLOCK_SIZE;
        spaceLeft = BLOCK_SIZE - block_offset;

        // read the block from disk
        bio_read(blockNumber, buff);

	    // Step 3: copy the correct amount of data from offset to buffer
        int copy_size = (size - byteCount < spaceLeft) ? (size - byteCount) : spaceLeft;
        memcpy(buffer + byteCount, buff + block_offset, copy_size);

        byteCount += copy_size;
        offset += copy_size;
    }

    free(buff);
    // rufs_debug("Read %d bytes from file %s", byteCount, path);
    // Note: this function should return the amount of bytes you copied to buffer
    return byteCount;
}

static int rufs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

    void *buff;
	struct inode file;

    int blockNumber;
	int spaceLeft;
	int byteCount = 0;
	int block_offset;

    //rufs_debug("Writing %ld bytes to file %s at offset %ld", size, path, offset);

	// Step 1: You could call get_node_by_path() to get inode from path
    if (get_node_by_path(path, 0, &file) < 0) {
        rufs_debug("File not found at path %s", path);
        return -ENOENT;
    }

    // Step 2: Allocate memory for a block buffer
    buff = malloc(BLOCK_SIZE);
    if (!buff) {
        rufs_debug("Unable to allocate memory for block buffer");
        return -ENOMEM;
    }

	// Step 2: Based on size and offset, read its data blocks from disk
    while (byteCount < size) {
        // Calculate the block number and offset within the block
        int block_index = (offset + byteCount) / BLOCK_SIZE;
        block_offset = (offset + byteCount) % BLOCK_SIZE;
        spaceLeft = BLOCK_SIZE - block_offset;

        // Allocate a new block if needed
        if (!file.direct_ptr[block_index]) {
            file.direct_ptr[block_index] = get_avail_blkno();
        }

        blockNumber = file.direct_ptr[block_index];

        // Read the block from disk
        bio_read(blockNumber, buff);

        // Copy data into the block
        int copy_size = (size - byteCount < spaceLeft) ? (size - byteCount) : spaceLeft;
        memcpy(buff + block_offset, buffer + byteCount, copy_size);

	    // Step 3: Write the correct amount of data from offset to disk
        bio_write(blockNumber, buff);

        byteCount += copy_size;
    }

	// Step 4: Update the inode info and write it to disk
    if (offset + byteCount > file.size) {
        file.size = offset + byteCount;
    }
	
    file.vstat.st_size = file.size;
    time(&file.vstat.st_mtime);
    writei(file.ino, &file);

    // Note: this function should return the amount of bytes you write to disk
    free(buff);
    //rufs_debug("Wrote %d bytes to file %s", byteCount, path);
    return byteCount;
}

// static int rufs_unlink(const char *path) {

// 	// Step 1: Use dirname() and basename() to separate parent directory path and target file name

// 	// Step 2: Call get_node_by_path() to get inode of target file

// 	// Step 3: Clear data block bitmap of target file

// 	// Step 4: Clear inode bitmap and its data block

// 	// Step 5: Call get_node_by_path() to get inode of parent directory

// 	// Step 6: Call dir_remove() to remove directory entry of target file in its parent directory

// 	return 0;
// }

// static int rufs_truncate(const char *path, off_t size) {
// 	// For this project, you don't need to fill this function
// 	// But DO NOT DELETE IT!
//     return 0;
// }

// static int rufs_release(const char *path, struct fuse_file_info *fi) {
// 	// For this project, you don't need to fill this function
// 	// But DO NOT DELETE IT!
// 	return 0;
// }

// static int rufs_flush(const char * path, struct fuse_file_info * fi) {
// 	// For this project, you don't need to fill this function
// 	// But DO NOT DELETE IT!
//     return 0;
// }

// static int rufs_utimens(const char *path, const struct timespec tv[2]) {
// 	// For this project, you don't need to fill this function
// 	// But DO NOT DELETE IT!
//     return 0;
// }


static struct fuse_operations rufs_ope = {
	.init		= rufs_init,
	.destroy	= rufs_destroy,

	.getattr	= rufs_getattr,
	.readdir	= rufs_readdir,
	.opendir	= rufs_opendir,
	.releasedir	= rufs_releasedir,
	.mkdir		= rufs_mkdir,
	.rmdir		= rufs_rmdir,

	.create		= rufs_create,
	.open		= rufs_open,
	.read 		= rufs_read,
	.write		= rufs_write,
	.unlink		= rufs_unlink,

	.truncate   = rufs_truncate,
	.flush      = rufs_flush,
	.utimens    = rufs_utimens,
	.release	= rufs_release
};


int main(int argc, char *argv[]) {
	int fuse_stat;

	getcwd(diskfile_path, PATH_MAX);
	strcat(diskfile_path, "/DISKFILE");

	fuse_stat = fuse_main(argc, argv, &rufs_ope, NULL);

	return fuse_stat;
}

