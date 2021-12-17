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

#define BLOCK_SIZE (BSIZE)
#define DEPB (BSIZE / sizeof(struct dirent))
char bitarray[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
#define CHECKSETBIT(bitmapblocks, blockaddr) ((*(bitmapblocks + blockaddr / 8)) & (bitarray[blockaddr % 8]))

//From mkfs.c
// convert to intel byte order
ushort
xshort(ushort x)
{
  ushort y;
  uchar *a = (uchar *)&y;
  a[0] = x;
  a[1] = x >> 8;
  return y;
}

void check_dir(char *fileAddr, struct dinode *inode, int inum);
void rule9To12(struct superblock *superBlk, char *fileAddr);
void rule6(struct superblock *superBlk, char *fileAddr, uint firstdatablock, char *inodeblocks, char *bitmapblocks);

int main(int argc, char *argv[])
{
  int fsfd;
  char *fileAddr;
  //struct dinode *diskInodePtr;
  struct superblock *superBlock;
  //struct dirent *dirEntry;
  uint inodeblocksno;
  uint bitmapblocksno;
  uint firstdatablock;
  char *inodeblocks;
  char *bitmapblocks;
  //char *datablocks;

  //error if arguments is less than 2.
  if (argc < 2)
  {
    fprintf(stderr, "Usage: fcheck <file_system_image>\n");
    exit(1);
  }

  fsfd = open(argv[1], O_RDONLY);
  if (fsfd < 0)
  {
    //error opening file
    printf("image not found.");
    exit(1);
  }

  // Use fstat to get the info the our fsfd for upcoming mmap
  struct stat fst;
  if (fstat(fsfd, &fst))
  {
    printf("fstat failed");
    exit(1);
  }

  fileAddr = mmap(NULL, fst.st_size, PROT_READ, MAP_PRIVATE, fsfd, 0);
  if (fileAddr == MAP_FAILED)
  {
    printf("mmap failed");
    exit(1);
  }

  //----------Get File System Info--------------
  /*
  *   BLOCK_SIZE  = block size
  *   IBLOCK(i)   = block containing inode i
  *   ROOTINO     = root i-number
  */
  /* read the super block */
  superBlock = (struct superblock *)(fileAddr + 1 * BLOCK_SIZE);

  /* read the inodes */
  //diskInodePtr = (struct dinode *)(fileAddr + IBLOCK((uint)0) * BLOCK_SIZE);

  // get the address of root dir
  //dirEntry = (struct dirent *)(fileAddr + (diskInodePtr[ROOTINO].addrs[0]) * BLOCK_SIZE);

  //number of blocks to store inode.
  inodeblocksno = (superBlock->ninodes / (IPB)) + 1;
  //number of blocks to store bitmap.
  bitmapblocksno = (superBlock->size / (BPB)) + 1;
  //inode blocks start address.
  inodeblocks = (char *)(fileAddr + 2 * BLOCK_SIZE);
  //bitmap blocks start address.
  bitmapblocks = (char *)(inodeblocks + inodeblocksno * BLOCK_SIZE);
  //data blocks start address.
  //datablocks = (char *)(bitmapblocks + bitmapblocksno * BLOCK_SIZE);
  //logical block number of first data block.
  firstdatablock = inodeblocksno + bitmapblocksno + 2;
  //direct addresses count.
  uint directAddrs[superBlock->ninodes];
  memset(directAddrs, 0, sizeof(uint) * superBlock->ninodes);
  //indirect addresses count.
  uint indirectAddrs[superBlock->ninodes];
  memset(indirectAddrs, 0, sizeof(uint) * superBlock->ninodes);

  //-------------------Check main 12 errors----------------------//
  struct dinode *inode;
  inode = (struct dinode *)(inodeblocks);
  int i;
  int j;
  for (j = 0; j < superBlock->ninodes; j++, inode++)
  {
    if (inode->type == 0)
    {
      continue;
    }
    //Rule - 1
    if (inode->type != T_FILE && inode->type != T_DIR && inode->type != T_DEV)
    {
      fprintf(stderr, "ERROR: bad inode.\n");
      exit(1);
    }

    // Rule 2, 5
    uint blockaddr;
    uint *idirectblockaddr;
    for (i = 0; i <= NDIRECT; i++)
    {
      blockaddr = inode->addrs[i];
      if (blockaddr == 0)
      {
        continue;
      }
      // Rule - 2a
      if (blockaddr < 0 || blockaddr >= superBlock->size)
      {
        fprintf(stderr, "ERROR: bad direct address in inode.\n");
        exit(1);
      }
      // Rule - 5
      if (!CHECKSETBIT(bitmapblocks, blockaddr))
      {
        fprintf(stderr, "ERROR: address used by inode but marked free in bitmap.\n");
        exit(1);
      }
      if (i == NDIRECT)
      {
        idirectblockaddr = (uint *)(fileAddr + blockaddr * BLOCK_SIZE);
        for (i = 0; i < NINDIRECT; i++, idirectblockaddr++)
        {
          blockaddr = *idirectblockaddr;
          if (blockaddr == 0)
          {
            continue;
          }
          //Rule - 2b
          if (blockaddr < 0 || blockaddr >= superBlock->size)
          {
            fprintf(stderr, "ERROR: bad indirect address in inode.\n");
            exit(1);
          }
          // Rule - 5
          if (!CHECKSETBIT(bitmapblocks, blockaddr))
          {
            fprintf(stderr, "ERROR: address used by inode but marked free in bitmap.\n");
            exit(1);
          }
        }
      }
    }

    //Rule - 3, 4
    if (j == 1)
    {
      //if inode 1 type is not directory, then error.
      if (inode->type != T_DIR)
      {
        fprintf(stderr, "ERROR: root directory does not exist.\n");
        exit(1);
      }
      check_dir(fileAddr, inode, 1);
    }

    //rule 4 for inode!=1
    if (j != 1 && inode->type == T_DIR)
    {
      check_dir(fileAddr, inode, j);
    }
  }

  //Rule 7
  struct dinode *inode2;
  inode2 = (struct dinode *)(inodeblocks);
  int k;
  for (k = 0; j < superBlock->ninodes; k++, inode2++)
  {
    if (inode2->type == 0)
    {
      continue;
    }
    uint blockaddr;
    for (k = 0; k <= NDIRECT; k++)
    {
      blockaddr = inode2->addrs[k];
      if (blockaddr == 0)
      {
        continue;
      }
      directAddrs[blockaddr - firstdatablock]++;
    }
  }

   //Rule 8
  struct dinode *inode3;
  uint *idirectblockaddr;
  inode3 = (struct dinode *)(inodeblocks);
  for (k = 0; j < superBlock->ninodes; k++, inode3++)
  {
    uint blockaddr=inode3->addrs[NDIRECT];
    for (k = 0; k < NINDIRECT; k++, idirectblockaddr++)
    {
          blockaddr = *idirectblockaddr;
          if (blockaddr == 0)
          {
            continue;
          }
          indirectAddrs[blockaddr - firstdatablock]++;
    }
  }



  //Rule 9-12
  rule9To12(superBlock, fileAddr);
  rule6(superBlock, fileAddr, firstdatablock, inodeblocks, bitmapblocks);

  for (k = 0; k < superBlock->nblocks; k++)
  {
    Rule - 7
    if (directAddrs[k] > 1)
    {
      fprintf(stderr, "ERROR: direct address used more than once.\n");
      exit(1);
    }
    Rule - 8
    if (indirectAddrs[k] > 1)
    {
      fprintf(stderr, "ERROR: indirect address used more than once.\n");
      exit(1);
    }
  }

  exit(0);
}

void rule9To12(struct superblock *superBlk, char *fileAddr)
{
  struct dinode *diskInodePtr;
  struct dirent *dirEntry;
  diskInodePtr = (struct dinode *)(fileAddr + IBLOCK((uint)0) * BLOCK_SIZE);

  //maintain an inode Actual reference count ( values >= 0 )
  int inodeActualRefCt[superBlk->ninodes];
  //maintain an inode Declared reference count ( -1 :not-used , >=0 :in-use & reference count)
  int inodeClaimedRefCt[superBlk->ninodes];
  int inodeTypes[superBlk->ninodes];

  //initialize inodeActualRefCt
  int inodeNum = 0;
  int n;
  for (inodeNum = 0; inodeNum < superBlk->ninodes; inodeNum++)
  {
    inodeActualRefCt[inodeNum] = 0;
    inodeTypes[inodeNum] = 0;
  }

  for (inodeNum = 0; inodeNum <= superBlk->ninodes; inodeNum++)
  {
    struct dinode curInode = diskInodePtr[inodeNum];
    //verify their types
    //TODO these maybe shouldn't be hardcoded
    //Dir=1 , File=2 , SpecialDevice=3
    //check bad inode type here

    //if in-use,
    if (curInode.type == 0)
    {
      continue;
    }
    //update inode Declared reference count
    inodeClaimedRefCt[inodeNum] = curInode.nlink;
    inodeTypes[inodeNum] = curInode.type;
    //if also a directory
    if (curInode.type == 1)
    {
      //make sure . and .. are there, and . is itself
      //ERROR-4 if not
      //update inode Actual reference count for each reference to the inode
      int blockNum = 0;
      dirEntry = (struct dirent *)(fileAddr + (curInode.addrs[blockNum]) * BLOCK_SIZE);
      // print the entries in the first block of root dir
      n = curInode.size / sizeof(struct dirent); // n = number of directory entries in directory
      //printf("directory entires %d and DEPB %lu\n", n, DEPB);
      int formatStatus = -1;
      int j = 0;
      for (j = 0; j < n; j++, dirEntry++)
      {
        // if we have left a block, go to the next one
        if (j >= DEPB * (blockNum + 1))
        {
          blockNum++;
          // direct, else it's indirect
          if (blockNum <= NDIRECT)
          {
            dirEntry = (struct dirent *)(fileAddr + (curInode.addrs[blockNum]) * BLOCK_SIZE);
          }
          else
          {
            //TODO make this work to look at indirect blocks!
            exit(0);
            dirEntry = (struct dirent *)(fileAddr + (curInode.addrs[blockNum]) * BLOCK_SIZE);
          }
        }

        // Track that . and .. are there, and . is itself
        if (strcmp(dirEntry->name, ".") == 0)
        {
          //this should add inodeNum
          formatStatus += dirEntry->inum;
        }
        else if (strcmp(dirEntry->name, "..") == 0)
        {
          formatStatus++;
        }
        else
        {
          // update inode Actual reference count for each reference to the inode
          // we don't count . and .. as links
          if (dirEntry->inum != 0)
          {
            inodeActualRefCt[dirEntry->inum]++;
          }
        }
      }
    }
  }

  // Verify the Actual and Claimed reference counts
  for (inodeNum = 0; inodeNum < superBlk->ninodes; inodeNum++)
  {

    // If in-use, >0 Actual References
    if (inodeTypes[inodeNum] != 0 && inodeActualRefCt[inodeNum] <= 0 && inodeNum != 1)
    {
      // ERROR-9 for marked in use, but no reference found
      fprintf(stderr, "ERROR: inode marked use but not found in a directory.");
      exit(1);
    }

    // If Actual Reference >0, it is marked in-use
    if (inodeTypes[inodeNum] == 0 && inodeActualRefCt[inodeNum] > 0)
    {
      // ERROR-10 for marked unused, but reference(s) exist
      fprintf(stderr, "ERROR: inode referred to in directory but marked free.");
      exit(1);
    }

    // for regular files: Claimed References == Actual References
    if (inodeTypes[inodeNum] == 2 && inodeClaimedRefCt[inodeNum] != inodeActualRefCt[inodeNum])
    {
      //ERROR-11 Claimed reference count != Actual reference count
      fprintf(stderr, "ERROR: bad reference count for file.");
      exit(1);
    }

    // for directories: Declared References == Actual References == 1 (with exception of root)
    if ((inodeNum != 1 && inodeTypes[inodeNum] == 1) && (inodeActualRefCt[inodeNum] != 1))
    {
      //ERROR-12 Directory (non-root) references !=1 or has discrepency in reference count
      fprintf(stderr, "ERROR: directory appears more than once in file system.");
      exit(1);

      //NOTE technically this doesn't check that directory claimed reference counts are equal to actual
    }
  }
}

//rule 3 and rule 4
//rule 3: Root directory exists, its inode number is 1, and the parent of the root directory is itself.
//rule 4: Each directory contains . and .. entries, and the . entry points to the directory itself.
void check_dir(char *fileAddr, struct dinode *inode, int inum)
{
  int i, j, pfound, cfound;
  uint blockaddr;
  struct dirent *de;
  //cfound for '.', pfound for '..'
  pfound = cfound = 0;

  for (i = 0; i < NDIRECT; i++)
  {
    blockaddr = inode->addrs[i];
    if (blockaddr == 0)
      continue;

    de = (struct dirent *)(fileAddr + blockaddr * BLOCK_SIZE);
    for (j = 0; j < DPB; j++, de++)
    {

      //rule 4
      //if "." not point to itself, then error.
      if (!cfound && strcmp(".", de->name) == 0)
      {
        cfound = 1;
        if (de->inum != inum)
        {
          fprintf(stderr, "ERROR: directory not properly formatted.\n");
          exit(1);
        }
      }

      //rule 3
      //if inode 1's parent if not itself or if inode's parent is itself but inode number is not 1, then error.
      if (!pfound && strcmp("..", de->name) == 0)
      {
        pfound = 1;
        if ((inum != 1 && de->inum == inum) || (inum == 1 && de->inum != inum))
        {
          fprintf(stderr, "ERROR: root directory does not exist.\n");
          exit(1);
        }
      }

      if (pfound && cfound)
        break;
    }

    if (pfound && cfound)
      break;
  }
  //rule 4
  //if "." or ".." not found, then error.
  if (!pfound || !cfound)
  {
    fprintf(stderr, "ERROR: directory not properly formatted.\n");
    exit(1);
  }
}

void rule6(struct superblock *superBlk, char *fileAddr, uint firstdatablock, char *inodeblocks, char *bitmapblocks){
    struct dinode *inode;
    int k;
    int used_data_blocks[superBlk->nblocks];
    uint blockaddr;
    memset(used_data_blocks, 0, superBlk->nblocks*sizeof(int));
    
    inode=(struct dinode*)(inodeblocks);
    for(k=0; k<superBlk->ninodes; k++, inode++){
        if(inode->type==0){
            continue;
        }
        
    int i, j;
    uint blockaddr2;
    uint *indirect;
    
    for(i=0; i<(NDIRECT+1); i++){
        blockaddr2=inode->addrs[i];
        if(blockaddr2==0){
            continue;
        }
        
        used_data_blocks[blockaddr2-firstdatablock]=1;
        
        //check inode's indirect address
        if(i==NDIRECT){
            indirect=(uint *)(fileAddr+blockaddr2*BLOCK_SIZE);
            for(j=0; j<NINDIRECT; j++, indirect++){
                blockaddr2=*(indirect);
                if(blockaddr2==0){
                    continue;
                }
                
                used_data_blocks[blockaddr2-firstdatablock]=1;
            }
        }
    }

    }
    
    for(k=0; k<superBlk->nblocks; k++){
        blockaddr=(uint)(k+firstdatablock);
        if(used_data_blocks[k]==0 && CHECKSETBIT(bitmapblocks, blockaddr)){
            fprintf(stderr,"ERROR: bitmap marks block in use but it is not in use.\n");
            exit(1);
        }
    }
}