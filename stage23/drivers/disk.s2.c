#include <stdint.h>
#include <stddef.h>
#include <drivers/disk.h>
#include <lib/libc.h>
#if defined (bios)
#  include <lib/real.h>
#elif defined (uefi)
#  include <efi.h>
#endif
#include <lib/blib.h>
#include <lib/print.h>
#include <mm/pmm.h>

#if defined(bios)

struct bios_drive_params {
    uint16_t buf_size;
    uint16_t info_flags;
    uint32_t cyl;
    uint32_t heads;
    uint32_t sects;
    uint64_t lba_count;
    uint16_t bytes_per_sect;
    uint32_t edd;
} __attribute__((packed));

struct dap {
    uint16_t size;
    uint16_t count;
    uint16_t offset;
    uint16_t segment;
    uint64_t lba;
};

static struct dap *dap = NULL;

#define XFER_BUF_SIZE 16384
static void *xfer_buf = NULL;

bool disk_read_sectors(struct volume *volume, void *buf, uint64_t block, size_t count) {
    if (count * volume->sector_size > XFER_BUF_SIZE)
        panic("XFER");

    if (xfer_buf == NULL)
        xfer_buf = conv_mem_alloc_aligned(XFER_BUF_SIZE, 16);

    if (dap == NULL) {
        dap = conv_mem_alloc(sizeof(struct dap));
        dap->size = 16;
    }

    dap->count = count;

    dap->segment = rm_seg(xfer_buf);
    dap->offset  = rm_off(xfer_buf);
    dap->lba     = block;

    struct rm_regs r = {0};
    r.eax = 0x4200;
    r.edx = volume->drive;
    r.esi = (uint32_t)rm_off(dap);
    r.ds  = rm_seg(dap);

    rm_int(0x13, &r, &r);

    if (r.eflags & EFLAGS_CF) {
        int ah = (r.eax >> 8) & 0xff;
        switch (ah) {
            case 0x0c:
                return false;
            default:
                panic("Disk error %x. Drive %x, LBA %x.",
                      ah, volume->drive, dap->lba);
        }
    }

    if (buf != NULL)
        memcpy(buf, xfer_buf, count * volume->sector_size);

    return true;
}

void disk_create_index(void) {
    size_t volume_count = 0;

    for (uint8_t drive = 0x80; drive; drive++) {
        struct rm_regs r = {0};
        struct bios_drive_params drive_params;

        r.eax = 0x4800;
        r.edx = drive;
        r.ds  = rm_seg(&drive_params);
        r.esi = rm_off(&drive_params);

        drive_params.buf_size = sizeof(struct bios_drive_params);

        rm_int(0x13, &r, &r);

        if (r.eflags & EFLAGS_CF)
            continue;

        print("Found BIOS drive %x\n", drive);
        print(" ... %X total %u-byte sectors\n",
              drive_params.lba_count, drive_params.bytes_per_sect);

        struct volume block = {0};

        block.drive = drive;
        block.sector_size = drive_params.bytes_per_sect;
        block.first_sect = 0;
        block.sect_count = drive_params.lba_count;

        // The medium could not be present (e.g.: CD-ROMs)
        // Do a test run to see if we can actually read it
        if (!disk_read_sectors(&block, NULL, 0, 1)) {
            print(" ... Ignoring drive...\n");
            continue;
        }

        volume_count++;

        for (int part = 0; ; part++) {
            struct volume p = {0};
            int ret = part_get(&p, &block, part);

            if (ret == END_OF_TABLE || ret == INVALID_TABLE)
                break;
            if (ret == NO_PARTITION)
                continue;

            volume_count++;
        }
    }

    volume_index = ext_mem_alloc(sizeof(struct volume) * volume_count);

    for (uint8_t drive = 0x80; drive; drive++) {
        struct rm_regs r = {0};
        struct bios_drive_params drive_params;

        r.eax = 0x4800;
        r.edx = drive;
        r.ds  = rm_seg(&drive_params);
        r.esi = rm_off(&drive_params);

        drive_params.buf_size = sizeof(struct bios_drive_params);

        rm_int(0x13, &r, &r);

        if (r.eflags & EFLAGS_CF)
            continue;

        struct volume *block = ext_mem_alloc(sizeof(struct volume));

        block->drive = drive;
        block->partition = -1;
        block->sector_size = drive_params.bytes_per_sect;
        block->first_sect = 0;
        block->sect_count = drive_params.lba_count;

        // The medium could not be present (e.g.: CD-ROMs)
        // Do a test run to see if we can actually read it
        if (!disk_read_sectors(block, NULL, 0, 1)) {
            continue;
        }

        volume_index[volume_index_i++] = block;

        if (gpt_get_guid(&block->guid, block)) {
            block->guid_valid = true;
        }

        for (int part = 0; ; part++) {
            struct volume *p = ext_mem_alloc(sizeof(struct volume));
            int ret = part_get(p, block, part);

            if (ret == END_OF_TABLE || ret == INVALID_TABLE)
                break;
            if (ret == NO_PARTITION)
                continue;

            volume_index[volume_index_i++] = p;
        }
    }
}

#endif

#if defined (uefi)

struct volume *disk_volume_from_efi_handle(EFI_HANDLE *efi_handle) {
    struct volume *ret = NULL;

    EFI_GUID disk_io_guid = DISK_IO_PROTOCOL;
    EFI_GUID block_io_guid = BLOCK_IO_PROTOCOL;
    EFI_DISK_IO *disk_io = NULL;
    EFI_BLOCK_IO *block_io = NULL;

    uefi_call_wrapper(gBS->HandleProtocol, 3, efi_handle, &disk_io_guid,
                      &disk_io);
    uefi_call_wrapper(gBS->HandleProtocol, 3, efi_handle, &block_io_guid,
                      &block_io);

    uint64_t signature = BUILD_ID;
    uint64_t orig;

    uefi_call_wrapper(disk_io->ReadDisk, 5, disk_io, block_io->Media->MediaId, 0,
                      sizeof(uint64_t), &orig);
    uefi_call_wrapper(disk_io->WriteDisk, 5, disk_io, block_io->Media->MediaId, 0,
                      sizeof(uint64_t), &signature);

    for (size_t i = 0; i < volume_index_i; i++) {
        uint64_t compare;

        EFI_DISK_IO *cur_disk_io = NULL;
        EFI_BLOCK_IO *cur_block_io = NULL;

        uefi_call_wrapper(gBS->HandleProtocol, 3, volume_index[i]->efi_handle,
                          &disk_io_guid, &cur_disk_io);
        uefi_call_wrapper(gBS->HandleProtocol, 3, volume_index[i]->efi_handle,
                          &block_io_guid, &cur_block_io);

        uefi_call_wrapper(cur_disk_io->ReadDisk, 5, cur_disk_io,
                          cur_block_io->Media->MediaId,
                          0 +
                          volume_index[i]->first_sect * volume_index[i]->sector_size,
                          sizeof(uint64_t), &compare);

        if (compare == signature) {
            ret = volume_index[i];
            break;
        }
    }

    uefi_call_wrapper(disk_io->WriteDisk, 5, disk_io, block_io->Media->MediaId, 0,
                      sizeof(uint64_t), &orig);

    return ret;
}

bool disk_read_sectors(struct volume *volume, void *buf, uint64_t block, size_t count) {
    EFI_STATUS status;

    EFI_GUID block_io_guid = BLOCK_IO_PROTOCOL;
    EFI_BLOCK_IO *block_io = NULL;

    status = uefi_call_wrapper(gBS->HandleProtocol, 3, volume->efi_handle,
                               &block_io_guid, &block_io);

    status = uefi_call_wrapper(block_io->ReadBlocks, 5, block_io,
                               block_io->Media->MediaId,
                               block, count * volume->sector_size, buf);

    if (status != 0) {
        return false;
    }

    return true;
}

void disk_create_index(void) {
    EFI_STATUS status;

    size_t volume_count = 0;

    EFI_GUID block_io_guid = BLOCK_IO_PROTOCOL;
    EFI_HANDLE *handles = NULL;
    UINTN handles_size = 0;

    uefi_call_wrapper(gBS->LocateHandle, 5, ByProtocol, &block_io_guid,
                      NULL, &handles_size, handles);

    handles = ext_mem_alloc(handles_size);

    uefi_call_wrapper(gBS->LocateHandle, 5, ByProtocol, &block_io_guid,
                      NULL, &handles_size, handles);

    for (size_t i = 0; i < handles_size / sizeof(EFI_HANDLE); i++) {
        struct volume block = {0};

        EFI_BLOCK_IO *block_io = NULL;

        status = uefi_call_wrapper(gBS->HandleProtocol, 3, handles[i],
                                   &block_io_guid, &block_io);

        if (status != 0 || block_io == NULL || block_io->Media->LastBlock == 0)
            continue;

        if (block_io->Media->LogicalPartition)
            continue;

        volume_count++;

        block.efi_handle = handles[i];
        block.sector_size = block_io->Media->BlockSize;
        block.first_sect = 0;
        block.sect_count = block_io->Media->LastBlock + 1;

        for (int part = 0; ; part++) {
            struct volume trash = {0};
            int ret = part_get(&trash, &block, part);

            if (ret == END_OF_TABLE || ret == INVALID_TABLE)
                break;
            if (ret == NO_PARTITION)
                continue;

            volume_count++;
        }
    }

    volume_index = ext_mem_alloc(sizeof(struct volume) * volume_count);

    size_t drives_counter = 0x80;

    for (size_t i = 0; i < handles_size / sizeof(EFI_HANDLE); i++) {
        EFI_BLOCK_IO *drive = NULL;

        status = uefi_call_wrapper(gBS->HandleProtocol, 3, handles[i],
                                   &block_io_guid, &drive);

        if (status != 0 || drive == NULL || drive->Media->LastBlock == 0)
            continue;

        if (drive->Media->LogicalPartition)
            continue;

        struct volume *block = ext_mem_alloc(sizeof(struct volume));

        block->drive = drives_counter++;
        block->efi_handle = handles[i];
        block->partition = -1;
        block->sector_size = drive->Media->BlockSize;
        block->first_sect = 0;
        block->sect_count = drive->Media->LastBlock + 1;

        volume_index[volume_index_i++] = block;

        for (int part = 0; ; part++) {
            struct volume *p = ext_mem_alloc(sizeof(struct volume));
            int ret = part_get(p, block, part);

            if (ret == END_OF_TABLE || ret == INVALID_TABLE)
                break;
            if (ret == NO_PARTITION)
                continue;

            volume_index[volume_index_i++] = p;
        }
    }
}

#endif
