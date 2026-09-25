#include "game_installer.h"
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <hdd-ioctl.h>
#include <fileio.h>
#include <errno.h>
#include <delaythread.h>
#include "ui.h"

#define HDL_MAGIC           0xdeadfeed
#define HDL_FS_MAGIC        0x1337
#define HDL_GAME_DATA_OFF   0x800   /* sectors */
#define SECTOR_SIZE         512
#define COPY_BUF_SECTORS    128
#define DISC_TYPE_DVD       0x14
#define DISC_TYPE_CD        0x12

static unsigned char copy_buf[COPY_BUF_SECTORS * SECTOR_SIZE]
    __attribute__((aligned(64)));

static unsigned char xfer_buf[sizeof(hddAtaTransfer_t) +
                               COPY_BUF_SECTORS * SECTOR_SIZE]
    __attribute__((aligned(64)));

/* Extract game ID from ISO SYSTEM.CNF */
static int iso_get_game_id(const char *iso_path, char *id_out, int max)
{
    static unsigned char sector[2048] __attribute__((aligned(64)));
    int fd;
    unsigned int root_lba, root_size, cnf_lba, cnf_size;
    unsigned int offset;

    fd = fileXioOpen(iso_path, FIO_O_RDONLY, 0);
    if (fd < 0) return fd;

    /* PVD at sector 16 */
    fileXioLseek(fd, 16 * 2048, SEEK_SET);
    fileXioRead(fd, sector, 2048);

    if (sector[0] != 1 || memcmp(sector+1, "CD001", 5) != 0) {
        fileXioClose(fd);
        return -1;
    }

    root_lba  = sector[158] | (sector[159]<<8) | (sector[160]<<16) | (sector[161]<<24);
    root_size = sector[166] | (sector[167]<<8) | (sector[168]<<16) | (sector[169]<<24);

    /* Search root directory for SYSTEM.CNF */
    cnf_lba = cnf_size = 0;
    {
        unsigned int s, sectors = (root_size + 2047) / 2048;
        for (s = 0; s < sectors && !cnf_lba; s++) {
            char name[129];
            fileXioLseek(fd, (root_lba + s) * 2048, SEEK_SET);
            fileXioRead(fd, sector, 2048);
            offset = 0;
            while (offset < 2048) {
                int reclen = sector[offset];
                int namelen;
                unsigned int lba, sz;
                if (!reclen) break;
                namelen = sector[offset+32];
                lba = sector[offset+2]|(sector[offset+3]<<8)|
                      (sector[offset+4]<<16)|(sector[offset+5]<<24);
                sz  = sector[offset+10]|(sector[offset+11]<<8)|
                      (sector[offset+12]<<16)|(sector[offset+13]<<24);
                if (namelen > 0 && namelen <= 128) {
                    int i;
                    memcpy(name, sector+offset+33, namelen);
                    name[namelen] = '\0';
                    for (i = 0; name[i]; i++)
                        if (name[i] == ';') { name[i] = '\0'; break; }
                    if (strcasecmp(name, "SYSTEM.CNF") == 0) {
                        cnf_lba = lba; cnf_size = sz;
                    }
                }
                offset += reclen;
            }
        }
    }

    if (!cnf_lba) { fileXioClose(fd); return -2; }

    if (cnf_size > 2048) cnf_size = 2048;
    fileXioLseek(fd, cnf_lba * 2048, SEEK_SET);
    fileXioRead(fd, sector, cnf_size);
    fileXioClose(fd);

    sector[cnf_size] = '\0';
    {
        char *boot = (char*)strstr((char*)sector, "BOOT2");
        char *eq, *sl;
        int i;
        if (!boot) boot = (char*)strstr((char*)sector, "BOOT");
        if (!boot) return -3;
        eq = strchr(boot, '=');
        if (!eq) return -3;
        eq++;
        while (*eq == ' ') eq++;
        sl = strrchr(eq, '\\');
        if (!sl) sl = strrchr(eq, '/');
        if (sl) eq = sl + 1;
        for (i = 0; *eq && *eq != ';' && *eq != '\r' && *eq != '\n' &&
                    *eq != ' ' && i < max-1; eq++, i++)
            id_out[i] = *eq;
        id_out[i] = '\0';
        return i > 0 ? 0 : -4;
    }
}

/* Get partition start LBA via stat */
static int get_partition_lba(const char *part_path, u32 *lba_out)
{
    iox_stat_t st;
    int r = fileXioGetStat(part_path, &st);
    if (r < 0) return r;
    *lba_out = st.private_0;
    return 0;
}

/* Create HDL game data partition */
static int create_hdl_partition(const char *disc_id, u32 size_mb)
{
    char cmd[128];
    int fd;
    snprintf(cmd, sizeof(cmd), "hdd0:__.%s,,,%luM,HDL",
             disc_id, (unsigned long)size_mb);
    fd = fileXioOpen(cmd, FIO_O_RDWR | FIO_O_CREAT, 0);
    if (fd < 0) return fd;
    return fileXioClose(fd);
}

/* Write HDL header at sector 0x800 from partition start */
static int write_hdl_header(const char *disc_id, const char *title,
                             u32 size_kb, u32 disc_type)
{
    static unsigned char hdr[1024] __attribute__((aligned(64)));
    hddAtaTransfer_t *xfer = (hddAtaTransfer_t *)xfer_buf;
    char part_path[64];
    u32 lba;
    int r;

    snprintf(part_path, sizeof(part_path), "hdd0:__.%s", disc_id);
    r = get_partition_lba(part_path, &lba);
    if (r < 0) return r;

    memset(hdr, 0, sizeof(hdr));
    *(u32*)(hdr+0x00) = HDL_MAGIC;
    *(u32*)(hdr+0x04) = HDL_FS_MAGIC;
    strncpy((char*)(hdr+0x08), title, 159);
    strncpy((char*)(hdr+0xac), disc_id, 59);
    *(u32*)(hdr+0xec) = disc_type;
    *(u32*)(hdr+0xf0) = 1;           /* num_partitions */
    *(u32*)(hdr+0xf4) = 0;           /* part_offset MB */
    *(u32*)(hdr+0xf8) = lba + HDL_GAME_DATA_OFF;
    *(u32*)(hdr+0xfc) = size_kb;

    xfer->lba  = lba + HDL_GAME_DATA_OFF;
    xfer->size = 2;
    memcpy(xfer->data, hdr, 1024);

    return fileXioDevctl("hdd0:", HDIOC_WRITESECTOR, xfer,
                         sizeof(hddAtaTransfer_t) + 1024, NULL, 0);
}

/* Copy ISO sectors to HDL partition */
static int copy_iso_to_hdl(const char *iso_path, const char *disc_id,
                            u32 size_mb,
                            game_progress_callback_t progress,
                            void *ctx)
{
    hddAtaTransfer_t *xfer = (hddAtaTransfer_t *)xfer_buf;
    char part_path[64];
    u32 lba, sector_cur, sectors_total;
    int iso_fd, r = 0;

    snprintf(part_path, sizeof(part_path), "hdd0:__.%s", disc_id);
    r = get_partition_lba(part_path, &lba);
    if (r < 0) return r;

    iso_fd = fileXioOpen(iso_path, FIO_O_RDONLY, 0);
    if (iso_fd < 0) return iso_fd;

    sectors_total = (u32)((u64)size_mb * 1024 * 1024 / SECTOR_SIZE)
                    - HDL_GAME_DATA_OFF;
    sector_cur = 0;

    while (sector_cur < sectors_total) {
        u32 count = sectors_total - sector_cur;
        int n;
        if (count > COPY_BUF_SECTORS) count = COPY_BUF_SECTORS;
        n = fileXioRead(iso_fd, copy_buf, count * SECTOR_SIZE);
        if (n <= 0) { r = n < 0 ? n : -EIO; break; }
        count = (u32)n / SECTOR_SIZE;
        xfer->lba  = lba + HDL_GAME_DATA_OFF + sector_cur;
        xfer->size = count;
        memcpy(xfer->data, copy_buf, count * SECTOR_SIZE);
        r = fileXioDevctl("hdd0:", HDIOC_WRITESECTOR, xfer,
                          sizeof(hddAtaTransfer_t) + count * SECTOR_SIZE,
                          NULL, 0);
        if (r < 0) break;
        sector_cur += count;
        if (progress)
            progress(sector_cur, sectors_total, ctx);
    }

    fileXioClose(iso_fd);
    return r;
}

/* Create PP partition and write EXECUTE.KELF */
static int create_pp_partition(const char *disc_id,
                                const char *elf_source)
{
    static const int pfs_fmt[1] = {8192};
    char blockdev[64], kelf_path[80];
    int fd, r;
    u64 elf_size;
    u64 copied = 0, work = 0;
    char hash[65];

    snprintf(blockdev, sizeof(blockdev), "hdd0:PP.%s", disc_id);
    snprintf(kelf_path, sizeof(kelf_path), "pfs0:/EXECUTE.KELF");

    /* Create 128MB PFS partition */
    {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "hdd0:PP.%s,,,128M,PFS", disc_id);
        fd = fileXioOpen(cmd, FIO_O_RDWR | FIO_O_CREAT, 0);
        if (fd < 0) return fd;
        fileXioClose(fd);
    }

    fileXioUmount("pfs0:");
    r = fileXioMount("pfs0:", blockdev, FIO_MT_RDWR);
    if (r < 0) {
        r = fileXioFormat("pfs:", blockdev,
                          (const char*)pfs_fmt, sizeof(pfs_fmt));
        if (r < 0) return r;
        r = fileXioMount("pfs0:", blockdev, FIO_MT_RDWR);
        if (r < 0) return r;
    }

    /* Get ELF size */
    {
        iox_stat_t st;
        r = fileXioGetStat(elf_source, &st);
        if (r < 0) { fileXioUmount("pfs0:"); return r; }
        elf_size = ((u64)st.hisize << 32) | st.size;
    }

    /* Write EXECUTE.KELF (header + ELF + padding + footer) */
    r = write_direct_kelf_verified(elf_source, kelf_path, elf_size,
                                    NULL, &copied, &work, 1,
                                    NULL, NULL, hash);
    fileXioUmount("pfs0:");
    return r;
}

/* Main game install entry point */
int game_install(const char *iso_path, const char *title,
                 const char *elf_source,
                 game_progress_callback_t progress, void *ctx)
{
    char disc_id[32] = {0};
    u32 size_mb;
    int r;
    iox_stat_t st;

    /* Get ISO size */
    r = fileXioGetStat(iso_path, &st);
    if (r < 0) return r;
    size_mb = (u32)(((u64)st.hisize << 32 | st.size) + (1024*1024-1))
              / (1024*1024);

    /* Extract game ID */
    iso_get_game_id(iso_path, disc_id, sizeof(disc_id));
    if (!disc_id[0])
        snprintf(disc_id, sizeof(disc_id), "SLUS-00000");

    /* Create HDL partition */
    r = create_hdl_partition(disc_id, size_mb);
    if (r < 0) return r;

    /* Copy ISO data */
    r = copy_iso_to_hdl(iso_path, disc_id, size_mb, progress, ctx);
    if (r < 0) return r;

    /* Write HDL header */
    r = write_hdl_header(disc_id, title, size_mb * 1024,
                          DISC_TYPE_DVD);
    if (r < 0) return r;

    /* Create PP partition with EXECUTE.KELF */
    r = create_pp_partition(disc_id, elf_source);
    return r;
}

void run_game_installer(psx_revision_t revision,
                        const system_version_result_t *version)
{
    /* TODO: implement game ISO scan and install UI */
    (void)revision;
    (void)version;
    ui_begin();
    ui_printf("GAME INSTALLER\\n\\nComing soon.\\n");
    ui_sync();
    DelayThread(2000000);
}
