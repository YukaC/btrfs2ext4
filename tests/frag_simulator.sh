#!/bin/bash
# frag_simulator.sh - Generates highly fragmented Btrfs images for testing
# Simulate 40% fragmentation (medium extents) and 90% fragmentation (tiny extents)

set -e

create_frag_image() {
    local img_name=$1
    local size_mb=$2
    local frag_percent=$3

    echo "Creating fragmented image: $img_name (${size_mb}MB, ${frag_percent}% fragmentation)"
    
    # Create sparse file
    dd if=/dev/zero of="$img_name" bs=1M count=0 seek="$size_mb" status=none
    
    # Format Btrfs
    mkfs.btrfs -f -q "$img_name"
    
    # Mount
    mkdir -p mnt_frag
    sudo mount -o loop "$img_name" mnt_frag
    
    # Generate fragmentation by writing small blocks and deleting interleaved files
    local target_files=$(( size_mb * frag_percent / 100 * 1024 / 4 ))
    
    for i in $(seq 1 $target_files); do
        dd if=/dev/urandom of="mnt_frag/file_$i" bs=4K count=1 status=none
    done
    
    # Delete every other file to create massive free space fragmentation
    for i in $(seq 1 2 $target_files); do
        rm "mnt_frag/file_$i"
    done
    
    sudo umount mnt_frag
    rmdir mnt_frag
    echo "Done: $img_name"
}

create_frag_image "frag_40_percent.img" 512 40
create_frag_image "frag_90_percent.img" 512 90
