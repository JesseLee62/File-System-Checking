#include <stdio.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <stdbool.h>

#include "types.h"
#include "fs.h"

char bitval[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
#define BLOCK_SIZE (BSIZE)
#define BCHECK(bitmap, bn) ((*(bitmap + bn / 8)) & (bitval[bn % 8]))

void badInode(struct dinode *inodes, int num_inodes);
void badAddress(struct dinode *inodes, int num_inodes, char *addr);
void badRootDir(struct dinode *inodes, struct dirent *RootEntry);
void badRefCnt(struct dinode *inodes, struct dirent *RootEntry, int num_inodes);
void dirExtraLinks(struct dinode *inodes, struct dirent *RootEntry, int num_inodes);
void badDirContent(struct dinode *inodes, int num_inodes, char *addr);
void unmarkbit(char *bmap, uint bn);
void markbit(char *bmap, uint bn);
void badfreebitmap(struct dinode *inodes, int num_inodes, char *addr, char *bitmap);
void badinusebitmap(struct dinode *inodes, struct superblock *sb, char *addr, char *bitmap, char *bitmap_copy, int metablocks);
void badUsedInode(struct dinode *inodes, int num_inodes, char *ibitmap, struct dirent *rootEntry);
void badAddrUse(struct dinode *inodes, int num_inodes, char *addr, char *bitmap);

int main(int argc, char *argv[])
{
    // int r;
    int i, n, fsfd;
    char *addr;
    struct dinode *dip;
    struct superblock *sb;
    struct dirent *de;

    if (argc < 2)
    {
        fprintf(stderr, "Usage: fcheck <file_system_image>.\n");
        exit(1);
    }

    fsfd = open(argv[1], O_RDONLY);
    if (fsfd < 0)
    {
        fprintf(stderr, "image not found.\n");
        perror(argv[1]);
        exit(1);
    }

    /* Dont hard code the size of file. Use fstat to get the size */
    addr = mmap(NULL, 524248, PROT_READ, MAP_PRIVATE, fsfd, 0);
    if (addr == MAP_FAILED)
    {
        perror("mmap failed");
        exit(1);
    }
    /* read the super block */
    sb = (struct superblock *)(addr + 1 * BLOCK_SIZE);
    printf("fs size %d, no. of blocks %d, no. of inodes %d \n", sb->size, sb->nblocks, sb->ninodes);

    /* read the inodes */
    dip = (struct dinode *)(addr + IBLOCK((uint)0) * BLOCK_SIZE);
    printf("begin addr %p, begin inode %p , offset %ld \n", addr, dip, (char *)dip - addr);

    // read root inode
    printf("Root inode  size %d links %d type %d \n", dip[ROOTINO].size, dip[ROOTINO].nlink, dip[ROOTINO].type);

    // get the address of root dir
    de = (struct dirent *)(addr + (dip[ROOTINO].addrs[0]) * BLOCK_SIZE);

    // print the entries in the first block of root dir

    n = dip[ROOTINO].size / sizeof(struct dirent);
    for (i = 0; i < n; i++, de++)
    {
        printf(" inum %d, name %s ", de->inum, de->name);
        printf("inode  size %d links %d type %d \n", dip[de->inum].size, dip[de->inum].nlink, dip[de->inum].type);
    }

    // rule1
    badInode(dip, sb->ninodes);
    // rule2
    badAddress(dip, sb->ninodes, addr);
    // rule3
    de = (struct dirent *)(addr + (dip[ROOTINO].addrs[0]) * BLOCK_SIZE);
    badRootDir(dip, de);

    // Rule: 4, 5, 6, 7, 8
    // data prep
    /* calculate the needed for bitmap*/
    char *bitmap;
    int bitmapblocks = sb->size / BPB + 1;
    int metablocks = sb->ninodes / IPB + 3 + bitmapblocks;
    bitmap = (char *)(addr + (IBLOCK((uint)0) * BLOCK_SIZE) + (sb->ninodes / IPB + 1) * BLOCK_SIZE);
    char *bitmap_copy;
    bitmap_copy = (char *)malloc(bitmapblocks * BLOCK_SIZE);
    memcpy(bitmap_copy, bitmap, bitmapblocks * BLOCK_SIZE); // We create two copy of bitmap in order to compare repetitive use of address

    /* calculate inode bitmap for further reference*/
    int inode_byte_needed = sb->ninodes / 8 + 1;
    char *ibitmap = (char *)malloc(inode_byte_needed);
    for (i = 0; i < sb->ninodes; i++)
    {
        if (dip[i].type == 0)
            unmarkbit(ibitmap, i);
        else
            markbit(ibitmap, i);
    }
    char *ibitmap_copy = (char *)malloc(inode_byte_needed);
    memcpy(ibitmap_copy, ibitmap, inode_byte_needed);

    // rule 4
    badDirContent(dip, sb->ninodes, addr);

    // rule 5
    badfreebitmap(dip, sb->ninodes, addr, bitmap);

    // rule 6
    badinusebitmap(dip, sb, addr, bitmap, bitmap_copy, metablocks);

    // rule 9, 10
    if (true)
    {
        for (i = 0; i < sb->ninodes; i++)
        {
            if (dip[i].type == T_DIR)
            {
                // For each directory, get its entries
                struct dirent *dirEntries = (struct dirent *)(addr + dip[i].addrs[0] * BLOCK_SIZE);

                // Call badUsedInode for each directory
                badUsedInode(dip, sb->ninodes, ibitmap, dirEntries);
            }
        }
    }

    // rule 7, 8
    badAddrUse(dip, sb->ninodes, addr, bitmap_copy);

    // rule 11
    badRefCnt(dip, de, sb->ninodes);

    // rule 12
    dirExtraLinks(dip, de, sb->ninodes);

    exit(0);
}

// 1. Each inode is either unallocated or one of the valid types (T_FILE, T_DIR, T_DEV). If not, print ERROR: bad inode.
void badInode(struct dinode *inodes, int num_inodes)
{
    int i;
    for (i = 0; i < num_inodes; i++)
    {
        if (inodes[i].type != 0 && (inodes[i].type != T_FILE && inodes[i].type != T_DIR && inodes[i].type != T_DEV))
        {
            fprintf(stderr, "ERROR: bad inode.\n");
            exit(1);
        }
    }
}
// 2. For in-use inodes, each block address that is used by the inode is valid (points to a valid data block address within the image).
//    If the direct block is used and is invalid, print ERROR: bad direct address in inode.; if the indirect block is in use and is invalid, print ERROR: bad indirect address in inode.
void badAddress(struct dinode *inodes, int num_inodes, char *addr)
{
    int i;
    for (i = 0; i < num_inodes; i++)
    {
        if (inodes[i].type != 0)
        {
            int j;
            // direct blocks
            for (j = 0; j < NDIRECT; j++)
            {
                if (inodes[i].addrs[j] != 0 && (inodes[i].addrs[j] < 2 || inodes[i].addrs[j] >= BSIZE))
                {
                    fprintf(stderr, "ERROR: bad direct address in inode.\n");
                    exit(1);
                }
            }

            // level1 INDIRECT block
            if (inodes[i].addrs[NDIRECT] != 0)
            {
                uint *indirectBlock = (uint *)(addr + inodes[i].addrs[NDIRECT] * BLOCK_SIZE);
                for (j = 0; j < NINDIRECT; j++)
                {
                    if (indirectBlock[j] != 0 && (indirectBlock[j] < 2 || indirectBlock[j] >= BSIZE))
                    {
                        fprintf(stderr, "ERROR: bad indirect address in inode.\n");
                        exit(1);
                    }
                }
            }
        }
    }
}
// 3. Root directory exists, its inode number is 1, and the parent of the root directory is itself. If not, print ERROR: root directory does not exist.
void badRootDir(struct dinode *inodes, struct dirent *RootEntry)
{

    if (RootEntry->inum != 1)
    {
        fprintf(stderr, "ERROR: root directory does not exist.\n");
        exit(1);
    }

    struct dirent *RootReverseEntry = RootEntry + 1; // Supposed to be Root->.. addr
    if (strcmp(RootReverseEntry->name, "..") != 0 || RootReverseEntry->inum != 1)
    {
        fprintf(stderr, "ERROR: root directory does not exist.\n");
        exit(1);
    }
}

// 4. Each directory contains . and .. entries, and the . entry points to the directory itself. If not, print ERROR: directory not properly formatted.
void badDirContent(struct dinode *inodes, int num_inodes, char *addr)
{
    int i, j;

    for (i = 0; i < num_inodes; i++)
    {
        if (inodes[i].type == T_DIR) // Only check file_type = Dir
        {
            bool dot = false;
            bool dotdot = false;
            struct dirent *de;

            for (j = 0; j < NDIRECT; j++)
            {
                if (inodes[i].addrs[j] == 0)
                    continue;

                de = (struct dirent *)(addr + inodes[i].addrs[j] * BLOCK_SIZE);
                int entriesPerBlock = BLOCK_SIZE / sizeof(struct dirent);

                for (int k = 0; k < entriesPerBlock; k++, de++)
                {
                    if (de->inum == 0)
                        continue; // Skip unused entries

                    // Check for '.' and '..'
                    if (strcmp(de->name, ".") == 0)
                    {
                        dot = true;
                        if (de->inum != i)
                        { // '.' should point to the directory itself
                            fprintf(stderr, "ERROR: directory not properly formatted.\n");
                            exit(1);
                        }
                    }
                    else if (strcmp(de->name, "..") == 0)
                    {
                        dotdot = true;
                    }
                }
            }

            // Check indirect block if present
            if (inodes[i].addrs[NDIRECT] != 0)
            {
                uint *indirectBlock = (uint *)(addr + inodes[i].addrs[NDIRECT] * BLOCK_SIZE);
                for (int j = 0; j < NINDIRECT; j++)
                {
                    if (indirectBlock[j] == 0)
                        continue;

                    de = (struct dirent *)(addr + indirectBlock[j] * BLOCK_SIZE);
                    int entriesPerBlock = BLOCK_SIZE / sizeof(struct dirent);

                    for (int k = 0; k < entriesPerBlock; k++, de++)
                    {
                        if (de->inum == 0)
                            continue;

                        if (strcmp(de->name, ".") == 0)
                        {
                            dot = true;
                            if (de->inum != i)
                            {
                                fprintf(stderr, "ERROR: directory not properly formatted.\n");
                                exit(1);
                            }
                        }
                        else if (strcmp(de->name, "..") == 0)
                        {
                            dotdot = true;
                        }
                    }
                }
            }
            if (!dot || !dotdot)
            {
                fprintf(stderr, "ERROR: directory not properly formatted.\n");
                exit(1);
            }
        }
    }
}

// helper function: unmark the bit used by block number
void unmarkbit(char *bmap, uint bn)
{
    bmap = (bmap + bn / 8);
    *bmap = (*bmap) & (~bitval[bn % 8]);
}

// helper function: mark the bit used by block number
void markbit(char *bmap, uint bn)
{
    bmap = (bmap + bn / 8);
    *bmap = (*bmap) | (bitval[bn % 8]);
}

// 5.
void badfreebitmap(struct dinode *inodes, int num_inodes, char *addr, char *bitmap)
{
    int i = 0;
    // Loop over all inodes
    for (i = 0; i < num_inodes; i++)
    {
        // check if the inode is of valid type
        int j;
        // DIRECT
        for (j = 0; j < NDIRECT + 1; j++)
        {
            if (inodes[i].addrs[j] != 0)
            {

                // if the bn is valid, check the bitmap is set
                if (!BCHECK(bitmap, inodes[i].addrs[j]))
                {
                    fprintf(stderr, "ERROR: address used by inode but marked free in bitmap.\n");
                    exit(1);
                }
            }
        }
    }

    // level 1 INDIRECT
    if (inodes[i].addrs[NDIRECT] != 0)
    {
        int k;
        uint *indirectBlock = (uint *)(addr + inodes[i].addrs[NDIRECT] * BLOCK_SIZE);
        for (k = 0; k < NINDIRECT; k++, indirectBlock++)
        {
            if (*indirectBlock != 0)
            {
                // if the bn is valid, check the bitmap is set
                if (!BCHECK(bitmap, *indirectBlock))
                {
                    fprintf(stderr, "ERROR: address used by inode but marked free in bitmap.\n");
                    exit(1);
                }
            }
        }
    }
}

// 6.
void badinusebitmap(struct dinode *inodes, struct superblock *sb, char *addr, char *bitmap, char *bitmap_copy, int metablocks)
{

    int i;
    // Loop over all inodes
    for (i = 0; i < sb->ninodes; i++)
    {

        // direct blocks
        int j;
        for (j = 0; j < NDIRECT + 1; j++)
        {
            if (inodes[i].addrs[j] != 0)
            {

                // if the block nums are valid, check if the bitmap is set correctly
                if (BCHECK(bitmap_copy, inodes[i].addrs[j]))
                    unmarkbit(bitmap_copy, inodes[i].addrs[j]);
            }
        }

        // level1 INDIRECT blocks
        if (inodes[i].addrs[NDIRECT] != 0)
        {
            int k;
            uint *indirectBlock = (uint *)(addr + inodes[i].addrs[NDIRECT] * BLOCK_SIZE);
            for (k = 0; k < NINDIRECT; k++, indirectBlock++)
            {
                if (*indirectBlock != 0)
                {
                    // if the block nums are valid, check if the bitmap is set correctly
                    if (BCHECK(bitmap_copy, *indirectBlock))
                        unmarkbit(bitmap_copy, *indirectBlock);
                }
            }
        }
    }

    // Check leftover bit not flipped/checked by unmarkbit during traversal
    for (i = metablocks; i < sb->size; i++)
        if (BCHECK(bitmap_copy, i))
        {
            fprintf(stderr, "ERROR: bitmap marks block in use but it is not in use.\n");
            exit(1);
        }
}

// 7. 8.
void badAddrUse(struct dinode *inodes, int num_inodes, char *addr, char *bitmap)
{
    // Loop over all inodes
    int i = 0;
    for (i = 0; i < num_inodes; i++)
    {
        // check if the inode is of valid type
        int j;
        // direct blocks
        for (j = 0; j < NDIRECT + 1; j++)
        {
            if (inodes[i].addrs[j] != 0)
            {

                // if the block nums are valid, check if the bitmap is set correctly
                if (BCHECK(bitmap, inodes[i].addrs[j]))
                    unmarkbit(bitmap, inodes[i].addrs[j]);
                else
                {
                    fprintf(stderr, "ERROR: direct address used more than once.\n");
                    exit(1);
                }
            }
        }

        // level1 INDIRECT blocks
        if (inodes[i].addrs[NDIRECT] != 0)
        {
            int k;
            uint *indirectBlock = (uint *)(addr + inodes[i].addrs[NDIRECT] * BLOCK_SIZE);
            for (k = 0; k < NINDIRECT; k++, indirectBlock++)
            {
                if (*indirectBlock != 0)
                {
                    // if the block nums are valid, check if the bitmap is set correctly
                    if (BCHECK(bitmap, *indirectBlock))
                        unmarkbit(bitmap, *indirectBlock);
                    else
                    {
                        fprintf(stderr, "ERROR: indirect address used more than once.\n");
                        exit(1);
                    }
                }
            }
        }
    }
}

// 9. 10
void badUsedInode(struct dinode *inodes, int num_inodes, char *ibitmap, struct dirent *rootEntry)
{
    bool *inodeReferred = (bool *)calloc(num_inodes, sizeof(bool)); // To track if inodes are referred in directories

    // Loop through all directory entries to mark referred inodes
    for (int i = 0; i < num_inodes; i++)
    {
        if (inodes[i].type == T_DIR)
        {
            // Traverse directory content
            struct dirent *de = (struct dirent *)(rootEntry + i); // Assuming rootEntry points to directory entries
            for (int j = 0; j < inodes[i].size / sizeof(struct dirent); j++, de++)
            {
                if (de->inum > 0 && de->inum < num_inodes)
                {
                    inodeReferred[de->inum] = true;
                }
            }
        }
    }

    // Check for Requirement
    for (int i = 0; i < num_inodes; i++)
    {
        if (!BCHECK(ibitmap, i))
        { // If inode is marked in use
            if (inodeReferred[i])
            {
                fprintf(stderr, "ERROR: inode referred to in directory but marked free.\n");
                exit(1);
            }
        }
        else
        { // If inode is not marked in use
            if (!inodeReferred[i])
            {
                fprintf(stderr, "ERROR: inode marked use but not found in a directory.\n");
                exit(1);
            }
        }
    }

    free(inodeReferred); // Free the allocated memory
}

// 11. Reference counts (number of links) for regular files match the number of times file is referred to in directories (i.e., hard links work correctly). If not, print ERROR: bad reference count for file.
// testcases : badrefcnt, badrefcnt2
void badRefCnt(struct dinode *inodes, struct dirent *RootEntry, int num_inodes)
{
    int i;
    for (i = 0; i < num_inodes; i++)
    {
        int j;
        if (inodes[i].type == T_FILE) // only for regular files
        {
            int count = 0;

            // count the number of times file is referred to in directories
            for (j = 0; j < num_inodes; j++)
            {
                if (RootEntry[j].inum == i)
                {
                    count++;
                }
            }

            // check whether match
            if (count != inodes[i].nlink)
            {
                fprintf(stderr, "ERROR: bad reference count for file.\n");
                exit(1);
            }
        }
    }
}

// 12. No extra links allowed for directories (each directory only appears in one other directory). If not, print ERROR: directory appears more than once in file system.
// testcase : dironce
void dirExtraLinks(struct dinode *inodes, struct dirent *RootEntry, int num_inodes)
{
    int i;
    for (i = 0; i < num_inodes; i++)
    {
        int j;
        if (inodes[i].type == T_DIR) // only for directories
        {
            int count = 0;

            // count the number of times directory appears in other directories
            for (j = 0; j < num_inodes; j++)
            {
                if (RootEntry[j].inum == i)
                {
                    count++;
                }
            }

            // check whether appears more than one link
            if (count > 1)
            {
                fprintf(stderr, "ERROR: directory appears more than once in file system.\n");
                exit(1);
            }
        }
    }
}