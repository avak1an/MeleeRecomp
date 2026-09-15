/**
 * @file extract_fonts.c
 * Build-time tool: pulls the two font atlases the game's C sources include
 * as byte tables (`sysdolphin/baselib/sislib_font.inc`, `debug_font.inc`)
 * out of the original main.dol inside a GALE01 disc image, so the PC build
 * does not need the GameCube build (configure.py + ninja) to have run.
 *
 *   extract_fonts <GALE01.iso> <output include dir>
 *
 * The tables are the same bytes dtk writes for the GameCube build (see
 * config/GALE01/config.yml, "extract").
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_(p) _mkdir(p)
#else
#define mkdir_(p) mkdir(p, 0755)
#endif

static uint32_t be32(const unsigned char* p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static const struct {
    const char* name;
    uint32_t address, size;
} tables[] = {
    { "sislib_font.inc", 0x8040CD40u, 0x23E00u }, /* HSD_SisLib_FontAtlas */
    { "debug_font.inc", 0x804088B8u, 0x1C00u },   /* HSD_DebugFontAtlas */
};

static int mkdir_p(const char* path)
{
    char buf[1024];
    size_t i, n = strlen(path);
    if (n >= sizeof(buf)) {
        return 0;
    }
    memcpy(buf, path, n + 1);
    for (i = 1; i < n; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            buf[i] = '\0';
            mkdir_(buf);
            buf[i] = '/';
        }
    }
    mkdir_(buf);
    return 1;
}

int main(int argc, char** argv)
{
    FILE* iso;
    unsigned char header[0x440], dol[0x100];
    uint32_t dol_offset;
    size_t t;

    if (argc != 3) {
        fprintf(stderr, "usage: extract_fonts <GALE01.iso> <output include dir>\n");
        return 2;
    }
    iso = fopen(argv[1], "rb");
    if (iso == NULL) {
        fprintf(stderr, "extract_fonts: cannot open %s\n", argv[1]);
        return 1;
    }
    if (fread(header, 1, sizeof(header), iso) != sizeof(header) || memcmp(header, "GALE01", 6) != 0) {
        fprintf(stderr, "extract_fonts: %s is not a GALE01 disc image\n", argv[1]);
        return 1;
    }
    dol_offset = be32(header + 0x420);
    if (_fseeki64(iso, dol_offset, SEEK_SET) != 0 || fread(dol, 1, sizeof(dol), iso) != sizeof(dol)) {
        fprintf(stderr, "extract_fonts: cannot read the DOL header\n");
        return 1;
    }
    for (t = 0; t < sizeof(tables) / sizeof(tables[0]); t++) {
        int s, found = 0;
        char path[1024];
        FILE* out;
        unsigned char* data;
        uint32_t i;
        for (s = 0; s < 18 && !found; s++) { /* 7 text + 11 data sections */
            uint32_t off = be32(dol + s * 4), addr = be32(dol + 0x48 + s * 4), size = be32(dol + 0x90 + s * 4);
            if (size == 0 || tables[t].address < addr || tables[t].address + tables[t].size > addr + size) {
                continue;
            }
            data = (unsigned char*) malloc(tables[t].size);
            if (data == NULL || _fseeki64(iso, (int64_t) dol_offset + off + (tables[t].address - addr), SEEK_SET) != 0 ||
                fread(data, 1, tables[t].size, iso) != tables[t].size)
            {
                fprintf(stderr, "extract_fonts: cannot read %s\n", tables[t].name);
                return 1;
            }
            found = 1;
        }
        if (!found) {
            fprintf(stderr, "extract_fonts: %s not found in the DOL (address %08x)\n", tables[t].name,
                    tables[t].address);
            return 1;
        }
        snprintf(path, sizeof(path), "%s/sysdolphin/baselib", argv[2]);
        mkdir_p(path);
        snprintf(path, sizeof(path), "%s/sysdolphin/baselib/%s", argv[2], tables[t].name);
        out = fopen(path, "wb");
        if (out == NULL) {
            fprintf(stderr, "extract_fonts: cannot write %s\n", path);
            return 1;
        }
        for (i = 0; i < tables[t].size; i++) {
            fprintf(out, "0x%02X, %s", data[i], (i % 16 == 15 || i + 1 == tables[t].size) ? "\n" : "");
        }
        fclose(out);
        free(data);
        printf("extract_fonts: wrote %s (%u bytes)\n", path, tables[t].size);
    }
    fclose(iso);
    return 0;
}
