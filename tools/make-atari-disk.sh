#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Create a DOS-partitioned FAT16 IDE image that EmuTOS can mount without a
# third-party hard-disk driver. No loop devices or root privileges are needed.

set -euo pipefail

usage() {
    echo "Usage: $0 OUTPUT [CONTENT_DIR] [SIZE_MB]" >&2
    echo "  SIZE_MB defaults to 64 and must be between 8 and 512." >&2
    exit 2
}

[ "$#" -ge 1 ] && [ "$#" -le 3 ] || usage

output=$1
content_dir=${2:-}
size_mb=${3:-64}
label=${PISTORM_DISK_LABEL:-PISTORM}

case "$size_mb" in
    ''|*[!0-9]*) usage ;;
esac
[ "$size_mb" -ge 8 ] && [ "$size_mb" -le 512 ] || usage
[ ${#label} -le 11 ] || { echo "Disk label must be at most 11 characters." >&2; exit 2; }

[ ! -e "$output" ] || {
    echo "Refusing to overwrite existing disk image: $output" >&2
    exit 1
}

if [ -n "$content_dir" ] && [ ! -d "$content_dir" ]; then
    echo "Content directory does not exist: $content_dir" >&2
    exit 1
fi

mkfs_fat=$(command -v mkfs.fat || command -v mkdosfs || true)
[ -n "$mkfs_fat" ] || {
    echo "mkfs.fat not found; install dosfstools." >&2
    exit 1
}
command -v python3 >/dev/null || { echo "python3 not found." >&2; exit 1; }
command -v dd >/dev/null || { echo "dd not found." >&2; exit 1; }
if [ -n "$content_dir" ]; then
    command -v mcopy >/dev/null || { echo "mcopy not found; install mtools." >&2; exit 1; }
fi

mkdir -p "$(dirname "$output")"
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/pistorm-disk.XXXXXX")
partition=$tmpdir/partition.img
cleanup() { rm -rf "$tmpdir"; }
trap cleanup EXIT

sector_size=512
total_sectors=$((size_mb * 1024 * 1024 / sector_size))
partition_start=1
partition_sectors=$((total_sectors - partition_start))

# mkfs.fat -A adjusts logical sector/cluster sizes to stay within TOS's FAT16
# limits. Keep the filesystem on a whole track boundary so mtools and Atari
# software agree on its geometry; any remainder stays unused in the partition.
track_size=32
clusters=$((partition_sectors / 2))
while [ "$clusters" -gt 32765 ]; do
    clusters=$((clusters / 2))
    track_size=$((track_size * 2))
done
filesystem_sectors=$((partition_sectors / track_size * track_size))

# Build a PC/DOS MBR. EmuTOS understands this partition table directly. The
# disk geometry is 16 heads x 32 sectors, matching the common Hatari image
# layout and the geometry synthesized by PiStorm's IDE emulation.
python3 - "$output" "$total_sectors" "$partition_start" "$partition_sectors" <<'PY'
import struct
import sys

path = sys.argv[1]
total_sectors = int(sys.argv[2])
start = int(sys.argv[3])
count = int(sys.argv[4])
heads = 16
sectors_per_track = 32

def chs(lba):
    cylinder = lba // (heads * sectors_per_track)
    if cylinder > 1023:
        return bytes((0xFE, 0xFF, 0xFF))
    head = (lba // sectors_per_track) % heads
    sector = (lba % sectors_per_track) + 1
    return bytes((head, (sector & 0x3F) | ((cylinder >> 2) & 0xC0), cylinder & 0xFF))

mbr = bytearray(512)
struct.pack_into("<H", mbr, 0x0B, 512)
mbr[0x0D] = 2
struct.pack_into("<H", mbr, 0x0E, 1)
mbr[0x15] = 0xF8
struct.pack_into("<H", mbr, 0x18, sectors_per_track)
struct.pack_into("<H", mbr, 0x1A, heads)
struct.pack_into("<I", mbr, 0x20, total_sectors)
mbr[0x24] = 0x80

entry = 0x1BE
mbr[entry] = 0x80
mbr[entry + 1:entry + 4] = chs(start)
mbr[entry + 4] = 0x04 if total_sectors < 65536 else 0x06
mbr[entry + 5:entry + 8] = chs(start + count - 1)
struct.pack_into("<II", mbr, entry + 8, start, count)
mbr[0x1FE:0x200] = b"\x55\xAA"

with open(path, "wb") as disk:
    disk.write(mbr)
    disk.truncate(total_sectors * 512)
PY

truncate -s "$((filesystem_sectors * sector_size))" "$partition"
"$mkfs_fat" -A -F 16 -i 50495354 -n "$label" "$partition" >/dev/null

if [ -n "$content_dir" ]; then
    shopt -s nullglob dotglob
    content=("$content_dir"/*)
    shopt -u nullglob dotglob
    if [ "${#content[@]}" -gt 0 ]; then
        MTOOLS_NO_VFAT=1 mcopy -i "$partition" -spm "${content[@]}" ::/
    fi
fi

dd if="$partition" of="$output" bs="$sector_size" seek="$partition_start" \
    conv=notrunc,sparse status=none

echo "Created $output (${size_mb} MB, FAT16 label $label)."
