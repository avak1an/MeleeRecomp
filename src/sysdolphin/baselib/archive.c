#include "archive.h"
#ifdef TARGET_PC
#include <pc_hsd_swap.h>
#endif

#include <string.h>

#include <dolphin/os.h>

static inline void Locate(HSD_Archive* archive)
{
    u32 i;
    u32* ptr;

    for (i = 0; i < archive->header.nb_reloc; i++) {
        ptr = (u32*) (archive->data + archive->reloc_info[i].offset);
        *ptr += (u32) archive->data;
    }
}

#ifdef TARGET_PC
/* Archives are big-endian on disc. On a little-endian host the parts the
 * parser itself reads are swapped in place before parsing: the header, the
 * relocation/public/extern tables, and every pointer slot named by the
 * relocation table. Other fields are swapped by the code that consumes
 * them (see pc/README.md, "Endianness"). */
static u32 pc_bswap32(u32 v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

static void pc_archive_swap(u8* src, size_t file_size)
{
    u32* header = (u32*) src;
    u32 i, offset, nb_reloc, nb_public, nb_extern, data_size;
    u32* table;

    /* Already host order (e.g. parsed twice)? Then leave it alone. */
    if (header[0] == file_size) {
        return;
    }
    /* A freshly loaded file: any "already swapped" records inside its
     * memory belong to a previous file at the same address. */
    pc_swap_forget_range(src, file_size);
    for (i = 0; i < 6; i++) {
        header[i] = pc_bswap32(header[i]);
    }
    data_size = header[1];
    nb_reloc = header[2];
    nb_public = header[3];
    nb_extern = header[4];
    offset = sizeof(HSD_ArchiveHeader) + data_size;
    table = (u32*) (src + offset);
    for (i = 0; i < nb_reloc + 2 * nb_public + 2 * nb_extern; i++) {
        table[i] = pc_bswap32(table[i]);
    }
    for (i = 0; i < nb_reloc; i++) {
        u32* slot = (u32*) (src + sizeof(HSD_ArchiveHeader) + table[i]);
        *slot = pc_bswap32(*slot);
    }
}
#endif

s32 HSD_ArchiveParse(HSD_Archive* archive, u8* src, size_t file_size)
{
    u32 offset;

    if (archive == NULL) {
        return -1;
    }

#ifdef TARGET_PC
    pc_archive_swap(src, file_size);
#endif
    memset(archive, 0, sizeof(HSD_Archive));
    archive->flags |= 1;
    memcpy(archive, src, sizeof(HSD_ArchiveHeader));

    if (archive->header.file_size != file_size) {
        OSReport("HSD_ArchiveParse: byte-order mismatch! Please check data "
                 "format %x %x\n",
                 archive->header.file_size, file_size);
        return -1;
    }

    offset = sizeof(HSD_ArchiveHeader);
    if (archive->header.data_size != 0) { // Body Size
        archive->data = src + sizeof(HSD_ArchiveHeader);
        offset = archive->header.data_size + sizeof(HSD_ArchiveHeader);
    }
    if (archive->header.nb_reloc != 0) { // Relocation Size
        archive->reloc_info =
            (HSD_ArchiveRelocationInfo*) ((uintptr_t) src + offset);
        offset = offset +
                 archive->header.nb_reloc * sizeof(HSD_ArchiveRelocationInfo);
    }
    if (archive->header.nb_public != 0) { // Root Size
        archive->public_info =
            (HSD_ArchivePublicInfo*) ((uintptr_t) src + offset);
        offset =
            offset + archive->header.nb_public * sizeof(HSD_ArchivePublicInfo);
    }
    if (archive->header.nb_extern != 0) { // XRef Size
        archive->extern_info =
            (HSD_ArchiveExternInfo*) ((uintptr_t) src + offset);
        offset =
            offset + archive->header.nb_extern * sizeof(HSD_ArchiveExternInfo);
    }
    if (offset < archive->header.file_size) { // File Size
        archive->symbols = (char*) ((uintptr_t) src + offset);
    }

    archive->top_ptr = (void*) src;
    Locate(archive);

    return 0;
}

void* HSD_ArchiveGetPublicAddress(HSD_Archive* archive, const char* symbols)
{
    u32 i;

    for (i = 0; i < archive->header.nb_public; i++) {
        int comparison =
            strcmp(archive->symbols + archive->public_info[i].symbol, symbols);

        if (comparison == 0) {
            // If both strings are equal, we've found the node
            return archive->data + archive->public_info[i].offset;
        }
    }

    return NULL;
}

char* HSD_ArchiveGetExtern(HSD_Archive* archive, int offset)
{
    if (offset < 0 || archive->header.nb_extern <= (unsigned) offset) {
        return NULL;
    }

    return archive->symbols + archive->extern_info[offset].symbol;
}

void HSD_ArchiveLocateExtern(HSD_Archive* archive, const char* symbols,
                             void* addr)
{
    uintptr_t next;
    uintptr_t offset = -1;
    u32 i;

    for (i = 0; i < archive->header.nb_extern; i++) {
        int comparison =
            strcmp(symbols, archive->symbols + archive->extern_info[i].symbol);

        if (comparison == 0) {
            offset = archive->extern_info[i].offset;
            break;
        }
    }

    if (offset == -1U) {
        return;
    }

    while (offset != -1U && offset < archive->header.data_size) {
        next = *(uintptr_t*) ((uintptr_t) archive->data + offset);
        *(u32*) ((uintptr_t) archive->data + offset) = (uintptr_t) addr;
        offset = next;
    }
}
