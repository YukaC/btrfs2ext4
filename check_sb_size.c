#include <stdio.h>
#include "include/btrfs/btrfs_structures.h"
int main() { printf("Size: %zu\n", sizeof(struct btrfs_super_block)); return 0; }
