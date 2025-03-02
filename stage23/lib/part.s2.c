#include <stddef.h>
#include <stdint.h>
#include <lib/part.h>
#include <drivers/disk.h>
#if defined (bios)
#  include <lib/real.h>
#endif
#include <lib/libc.h>
#include <lib/blib.h>
#include <lib/print.h>
#include <mm/pmm.h>
#include <fs/file.h>

#define BLOCK_SIZE_IN_SECTORS 8

enum {
    CACHE_NOT_READY = 0,
    CACHE_READY
};

static bool cache_block(struct volume *volume, uint64_t block) {
    if (volume->cache_status == CACHE_READY && block == volume->cached_block)
        return true;

    volume->cache_status = CACHE_NOT_READY;

    if (volume->cache == NULL)
        volume->cache =
            ext_mem_alloc(BLOCK_SIZE_IN_SECTORS * volume->sector_size);

    if (!disk_read_sectors(volume, volume->cache,
                           volume->first_sect + block * BLOCK_SIZE_IN_SECTORS,
                           BLOCK_SIZE_IN_SECTORS))
        return false;

    volume->cache_status = CACHE_READY;
    volume->cached_block = block;

    return true;
}

bool volume_read(struct volume *volume, void *buffer, uint64_t loc, uint64_t count) {
    if (volume->pxe) {
        panic("Attempted volume_read() on pxe");
    }

    uint64_t block_size = BLOCK_SIZE_IN_SECTORS * volume->sector_size;

    uint64_t progress = 0;
    while (progress < count) {
        uint64_t block = (loc + progress) / block_size;

        if (!cache_block(volume, block))
            return false;

        uint64_t chunk = count - progress;
        uint64_t offset = (loc + progress) % block_size;
        if (chunk > block_size - offset)
            chunk = block_size - offset;

        memcpy(buffer + progress, &volume->cache[offset], chunk);
        progress += chunk;
    }

    return true;
}

struct gpt_table_header {
    // the head
    char     signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t _reserved0;

    // the partitioning info
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;

    // the guid
    struct guid disk_guid;

    // entries related
    uint64_t partition_entry_lba;
    uint32_t number_of_partition_entries;
    uint32_t size_of_partition_entry;
    uint32_t partition_entry_array_crc32;
} __attribute__((packed));

struct gpt_entry {
    struct guid partition_type_guid;

    struct guid unique_partition_guid;

    uint64_t starting_lba;
    uint64_t ending_lba;

    uint64_t attributes;

    uint16_t partition_name[36];
} __attribute__((packed));

bool gpt_get_guid(struct guid *guid, struct volume *volume) {
    struct gpt_table_header header = {0};

    int sector_size = volume->sector_size;

    // read header, located after the first block
    volume_read(volume, &header, sector_size * 1, sizeof(header));

    // check the header
    // 'EFI PART'
    if (strncmp(header.signature, "EFI PART", 8))
        return false;
    if (header.revision != 0x00010000)
        return false;

    *guid = header.disk_guid;

    return true;
}

static int gpt_get_part(struct volume *ret, struct volume *volume, int partition) {
    struct gpt_table_header header = {0};

    int sector_size = volume->sector_size;

    // read header, located after the first block
    volume_read(volume, &header, sector_size * 1, sizeof(header));

    // check the header
    // 'EFI PART'
    if (strncmp(header.signature, "EFI PART", 8))
        return INVALID_TABLE;
    if (header.revision != 0x00010000)
        return INVALID_TABLE;

    // parse the entries if reached here
    if ((uint32_t)partition >= header.number_of_partition_entries)
        return END_OF_TABLE;

    struct gpt_entry entry = {0};
    volume_read(volume, &entry,
         (header.partition_entry_lba * sector_size) + (partition * sizeof(entry)),
         sizeof(entry));

    struct guid empty_guid = {0};
    if (!memcmp(&entry.unique_partition_guid, &empty_guid, sizeof(struct guid)))
        return NO_PARTITION;

#if defined (uefi)
    ret->efi_handle  = volume->efi_handle;
#endif
    ret->drive       = volume->drive;
    ret->partition   = partition;
    ret->sector_size = sector_size;
    ret->first_sect  = entry.starting_lba;
    ret->sect_count  = (entry.ending_lba - entry.starting_lba) + 1;
    ret->backing_dev = volume;

    struct guid guid;
    if (!fs_get_guid(&guid, ret)) {
        ret->guid_valid = false;
    } else {
        ret->guid_valid = true;
        ret->guid = guid;
    }

    ret->part_guid_valid = true;
    ret->part_guid = entry.unique_partition_guid;

    return 0;
}

struct mbr_entry {
	uint8_t status;
	uint8_t chs_first_sect[3];
	uint8_t type;
	uint8_t chs_last_sect[3];
	uint32_t first_sect;
	uint32_t sect_count;
} __attribute__((packed));

static int mbr_get_logical_part(struct volume *ret, struct volume *extended_part,
                                int partition) {
    struct mbr_entry entry;

    size_t ebr_sector = 0;

    for (int i = 0; i < partition; i++) {
        size_t entry_offset = ebr_sector * extended_part->sector_size + 0x1ce;

        volume_read(extended_part, &entry, entry_offset, sizeof(struct mbr_entry));

        if (entry.type != 0x0f && entry.type != 0x05)
            return END_OF_TABLE;

        ebr_sector = entry.first_sect;
    }

    size_t entry_offset = ebr_sector * extended_part->sector_size + 0x1be;

    volume_read(extended_part, &entry, entry_offset, sizeof(struct mbr_entry));

    if (entry.type == 0)
        return NO_PARTITION;

#if defined (uefi)
    ret->efi_handle  = extended_part->efi_handle;
#endif
    ret->drive       = extended_part->drive;
    ret->partition   = partition + 4;
    ret->sector_size = extended_part->sector_size;
    ret->first_sect  = extended_part->first_sect + ebr_sector + entry.first_sect;
    ret->sect_count  = entry.sect_count;
    ret->backing_dev = extended_part->backing_dev;

    struct guid guid;
    if (!fs_get_guid(&guid, ret)) {
        ret->guid_valid = false;
    } else {
        ret->guid_valid = true;
        ret->guid = guid;
    }

    ret->part_guid_valid = false;

    return 0;
}

static int mbr_get_part(struct volume *ret, struct volume *volume, int partition) {
    // Check if actually valid mbr
    uint16_t hint = 0;
    volume_read(volume, &hint, 218, sizeof(uint16_t));
    if (hint != 0)
        return INVALID_TABLE;

    volume_read(volume, &hint, 444, sizeof(uint16_t));
    if (hint != 0 && hint != 0x5a5a)
        return INVALID_TABLE;

    volume_read(volume, &hint, 510, sizeof(uint16_t));
    if (hint != 0xaa55)
        return INVALID_TABLE;

    volume_read(volume, &hint, 446, sizeof(uint8_t));
    if ((uint8_t)hint != 0x00 && (uint8_t)hint != 0x80)
        return INVALID_TABLE;
    volume_read(volume, &hint, 462, sizeof(uint8_t));
    if ((uint8_t)hint != 0x00 && (uint8_t)hint != 0x80)
        return INVALID_TABLE;
    volume_read(volume, &hint, 478, sizeof(uint8_t));
    if ((uint8_t)hint != 0x00 && (uint8_t)hint != 0x80)
        return INVALID_TABLE;
    volume_read(volume, &hint, 494, sizeof(uint8_t));
    if ((uint8_t)hint != 0x00 && (uint8_t)hint != 0x80)
        return INVALID_TABLE;

    char hintc[64];
    volume_read(volume, hintc, 4, 8);
    if (memcmp(hintc, "_ECH_FS_", 8) == 0)
        return INVALID_TABLE;
    volume_read(volume, hintc, 54, 3);
    if (memcmp(hintc, "FAT", 3) == 0)
        return INVALID_TABLE;
    volume_read(volume, &hint, 1080, sizeof(uint16_t));
    if (hint == 0xef53)
        return INVALID_TABLE;

    struct mbr_entry entry;

    if (partition > 3) {
        for (int i = 0; i < 4; i++) {
            size_t entry_offset = 0x1be + sizeof(struct mbr_entry) * i;

            volume_read(volume, &entry, entry_offset, sizeof(struct mbr_entry));

            if (entry.type != 0x0f)
                continue;

            struct volume extended_part = {0};

#if defined (uefi)
            extended_part.efi_handle  = volume->efi_handle;
#endif
            extended_part.drive       = volume->drive;
            extended_part.partition   = i;
            extended_part.sector_size = volume->sector_size;
            extended_part.first_sect  = entry.first_sect;
            extended_part.sect_count  = entry.sect_count;
            extended_part.backing_dev = volume;

            return mbr_get_logical_part(ret, &extended_part, partition - 4);
        }

        return END_OF_TABLE;
    }

    size_t entry_offset = 0x1be + sizeof(struct mbr_entry) * partition;

    volume_read(volume, &entry, entry_offset, sizeof(struct mbr_entry));

    if (entry.type == 0)
        return NO_PARTITION;

#if defined (uefi)
    ret->efi_handle  = volume->efi_handle;
#endif
    ret->drive       = volume->drive;
    ret->partition   = partition;
    ret->sector_size = volume->sector_size;
    ret->first_sect  = entry.first_sect;
    ret->sect_count  = entry.sect_count;
    ret->backing_dev = volume;

    struct guid guid;
    if (!fs_get_guid(&guid, ret)) {
        ret->guid_valid = false;
    } else {
        ret->guid_valid = true;
        ret->guid = guid;
    }

    ret->part_guid_valid = false;

    return 0;
}

int part_get(struct volume *part, struct volume *volume, int partition) {
    int ret;

    ret = gpt_get_part(part, volume, partition);
    if (ret != INVALID_TABLE)
        return ret;

    ret = mbr_get_part(part, volume, partition);
    if (ret != INVALID_TABLE)
        return ret;

    return INVALID_TABLE;
}

struct volume **volume_index = NULL;
size_t volume_index_i = 0;

struct volume *volume_get_by_guid(struct guid *guid) {
    for (size_t i = 0; i < volume_index_i; i++) {
        if (volume_index[i]->guid_valid
         && memcmp(&volume_index[i]->guid, guid, 16) == 0) {
            return volume_index[i];
        }
        if (volume_index[i]->part_guid_valid
         && memcmp(&volume_index[i]->part_guid, guid, 16) == 0) {
            return volume_index[i];
        }
    }

    return NULL;
}

struct volume *volume_get_by_coord(int drive, int partition) {
    for (size_t i = 0; i < volume_index_i; i++) {
        if (volume_index[i]->drive == drive
         && volume_index[i]->partition == partition) {
            return volume_index[i];
        }
    }

    return NULL;
}
