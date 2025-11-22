#include "disk.h"

#include <dirent.h>
#include <fcntl.h>

#define GPT_TYPE_EFI "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
#define GPT_TYPE_LINUX "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
#define GPT_TYPE_SWAP "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F"
#define GPT_TYPE_LVM "E6D6D379-F507-44C2-A23C-238F2A3DF928"

#define MBR_TYPE_LINUX "83"
#define MBR_TYPE_SWAP "82"
#define MBR_TYPE_LVM "8e"

#define LABEL_BOOT "LIBERO_BOOT"
#define LABEL_EFI "LIBERO_EFI"
#define LABEL_ROOT "LIBERO_ROOT"
#define LABEL_SWAP "LIBERO_SWAP"

typedef struct {
    char name[64];
    char path[PATH_MAX];
    char model[128];
    long size_mb;
} DiskInfo;

typedef struct {
    const char *role;
    const char *label;
    const char *gpt_type;
    const char *mbr_type;
    int part_number;
    char size_spec[32];
    char device[PATH_MAX];
} PartitionSpec;

#define MAX_PARTITION_SPECS 8

static int is_usable_disk(const char *name)
{
    const char *skip_prefixes[] = {"loop", "ram", "fd", NULL};
    for (int i = 0; skip_prefixes[i]; ++i) {
        size_t len = strlen(skip_prefixes[i]);
        if (strncmp(name, skip_prefixes[i], len) == 0) {
            return 0;
        }
    }
    return 1;
}

static long read_long_from_file(const char *path, long fallback)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return fallback;
    }
    char buffer[128];
    if (!fgets(buffer, sizeof(buffer), f)) {
        fclose(f);
        return fallback;
    }
    fclose(f);
    return strtoll(buffer, NULL, 10);
}

static void trim_whitespace(char *s)
{
    if (!s) {
        return;
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    while (*s && isspace((unsigned char)*s)) {
        memmove(s, s + 1, strlen(s));
    }
}

static bool copy_string_checked(char *dest, size_t dest_len, const char *src)
{
    if (!dest || dest_len == 0) {
        return false;
    }
    if (!src) {
        dest[0] = '\0';
        return true;
    }

    size_t src_len = strlen(src);
    if (src_len >= dest_len) {
        memcpy(dest, src, dest_len - 1);
        dest[dest_len - 1] = '\0';
        return false;
    }

    memcpy(dest, src, src_len + 1);
    return true;
}

static bool copy_with_ellipsis(char *dest, size_t dest_len, const char *src)
{
    if (!dest || dest_len == 0) {
        return false;
    }
    if (!src) {
        dest[0] = '\0';
        return true;
    }

    size_t src_len = strlen(src);
    if (src_len < dest_len) {
        memcpy(dest, src, src_len + 1);
        return true;
    }

    if (dest_len < 4) {
        dest[0] = '\0';
        return false;
    }

    size_t keep = dest_len - 4;
    memcpy(dest, src, keep);
    memcpy(dest + keep, "...", 4);
    return false;
}

static int ensure_parent_dir(const char *path)
{
    if (!path || !path[0]) {
        return -1;
    }

    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) {
        return 0;
    }
    *slash = '\0';
    if (dir[0] == '\0') {
        return 0;
    }
    return ensure_directory(dir, 0755);
}

static PartitionSpec *plan_add_partition(PartitionSpec *plan, size_t *count, int number,
                                         const char *role, const char *size_spec,
                                         const char *label, const char *gpt_type, const char *mbr_type)
{
    if (!plan || !count || *count >= MAX_PARTITION_SPECS) {
        return NULL;
    }
    PartitionSpec *spec = &plan[*count];
    spec->role = role;
    spec->label = label;
    spec->gpt_type = gpt_type;
    spec->mbr_type = mbr_type;
    spec->part_number = number;
    spec->device[0] = '\0';
    if (size_spec && size_spec[0]) {
        snprintf(spec->size_spec, sizeof(spec->size_spec), "%s", size_spec);
    } else {
        spec->size_spec[0] = '\0';
    }
    (*count)++;
    return spec;
}

static PartitionSpec *find_partition_spec(PartitionSpec *plan, size_t count, const char *role)
{
    if (!plan || !role) {
        return NULL;
    }
    for (size_t i = 0; i < count; ++i) {
        if (plan[i].role && strcmp(plan[i].role, role) == 0) {
            return &plan[i];
        }
    }
    return NULL;
}

static const char *role_mountpoint(const char *role)
{
    if (!role) {
        return "?";
    }
    if (strcmp(role, "efi") == 0) {
        return "/boot/efi";
    }
    if (strcmp(role, "boot") == 0) {
        return "/boot";
    }
    if (strcmp(role, "root") == 0) {
        return "/";
    }
    if (strcmp(role, "swap") == 0) {
        return "swap";
    }
    return role;
}

static void summarize_partition_plan(const char *disk,
                                     long disk_mb,
                                     const PartitionSpec *plan,
                                     size_t count,
                                     char *buffer,
                                     size_t len)
{
    if (!buffer || len == 0) {
        return;
    }
    buffer[0] = '\0';

    char header[256];
    snprintf(header, sizeof(header),
             "Target: %s (%ld MB)\n\n%-6s %-6s %-12s %-12s %-12s\n%-6s %-6s %-12s %-12s %-12s\n",
             disk ? disk : "<unknown>", disk_mb,
             "Role", "Part#", "Size", "Mount", "Label",
             "-----", "-----", "------------", "------------", "------------");
    strncat(buffer, header, len - strlen(buffer) - 1);

    for (size_t i = 0; i < count; ++i) {
        const PartitionSpec *spec = &plan[i];
        const char *size_text = spec->size_spec[0] ? spec->size_spec : "rest-of-disk";
        const char *label = spec->label ? spec->label : "";

        char line[256];
        snprintf(line, sizeof(line), "%-6s %-6d %-12s %-12s %-12s\n",
                 spec->role ? spec->role : "?",
                 spec->part_number,
                 size_text,
                 role_mountpoint(spec->role),
                 label);
        strncat(buffer, line, len - strlen(buffer) - 1);
        if (strlen(buffer) + 1 >= len) {
            break;
        }
    }
}

static void migrate_cache_file(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path || strcmp(old_path, new_path) == 0) {
        return;
    }
    if (access(old_path, R_OK) != 0) {
        return;
    }

    (void)ensure_parent_dir(new_path);

    if (rename(old_path, new_path) == 0) {
        return;
    }

    if (errno == EXDEV) {
        if (copy_file_simple(old_path, new_path) == 0) {
            unlink(old_path);
            return;
        }
    }

    log_error("Failed to move cache file from %s to %s: %s", old_path, new_path, strerror(errno));
}

static DiskInfo *collect_disks(size_t *out_count)
{
    DIR *dir = opendir("/sys/block");
    if (!dir) {
        log_error("Unable to open /sys/block: %s", strerror(errno));
        return NULL;
    }

    size_t capacity = 8;
    size_t count = 0;
    DiskInfo *disks = calloc(capacity, sizeof(DiskInfo));
    if (!disks) {
        closedir(dir);
        return NULL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (!is_usable_disk(entry->d_name)) {
            continue;
        }

        char size_path[PATH_MAX];
        snprintf(size_path, sizeof(size_path), "/sys/block/%s/size", entry->d_name);
        long sectors = read_long_from_file(size_path, -1);
        if (sectors <= 0) {
            continue;
        }
        long size_mb = (sectors * 512L) / (1024L * 1024L);
        if (size_mb <= 0) {
            continue;
        }

        char model_path[PATH_MAX];
        snprintf(model_path, sizeof(model_path), "/sys/block/%s/device/model", entry->d_name);
        char model[128] = {0};
        FILE *mf = fopen(model_path, "r");
        if (mf) {
            if (fgets(model, sizeof(model), mf)) {
                trim_whitespace(model);
            }
            fclose(mf);
        } else {
            snprintf(model, sizeof(model), "Generic");
        }

        if (count == capacity) {
            capacity *= 2;
            DiskInfo *tmp = realloc(disks, capacity * sizeof(DiskInfo));
            if (!tmp) {
                free(disks);
                closedir(dir);
                return NULL;
            }
            disks = tmp;
        }

        if (!copy_string_checked(disks[count].name, sizeof(disks[count].name), entry->d_name)) {
            log_error("Skipping disk with long name: %s", entry->d_name);
            continue;
        }
        snprintf(disks[count].path, sizeof(disks[count].path), "/dev/%s", entry->d_name);
        snprintf(disks[count].model, sizeof(disks[count].model), "%s", model);
        disks[count].size_mb = size_mb;
        count++;
    }

    closedir(dir);
    if (out_count) {
        *out_count = count;
    }
    return disks;
}

static void human_size(long size_mb, char *buffer, size_t len)
{
    if (size_mb > 4096) {
        double size_gb = size_mb / 1024.0;
        snprintf(buffer, len, "%.1f GB", size_gb);
    } else {
        snprintf(buffer, len, "%ld MB", size_mb);
    }
}

static int deactivate_swap_for_disk(const char *disk)
{
    FILE *f = fopen("/proc/swaps", "r");
    if (!f) {
        return 0;
    }

    char line[PATH_MAX + 64];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0;
    }

    int rc = 0;
    size_t disk_len = strlen(disk);
    while (fgets(line, sizeof(line), f)) {
        char entry[PATH_MAX] = {0};
        if (sscanf(line, "%s", entry) != 1) {
            continue;
        }
        if (strncmp(entry, disk, disk_len) != 0) {
            continue;
        }
        if (run_command("swapoff %s", entry) != 0) {
            rc = -1;
        }
    }

    fclose(f);
    return rc;
}

static int deactivate_disk_usage(const char *disk)
{
    if (!disk || !disk[0]) {
        return -1;
    }

    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "lsblk -nrpo NAME,TYPE,MOUNTPOINT %s", disk);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        log_error("Unable to inspect disk usage for %s: %s", disk, strerror(errno));
        return -1;
    }

    char line[PATH_MAX * 2];
    int rc = 0;
    while (fgets(line, sizeof(line), fp)) {
        char name[PATH_MAX] = {0};
        char type[32] = {0};
        char mountpoint[PATH_MAX] = {0};

        int fields = sscanf(line, "%s %31s %s", name, type, mountpoint);
        if (fields < 2) {
            continue;
        }
        if (strcmp(type, "disk") == 0) {
            continue;
        }

        bool is_swap = (fields == 3 && strcmp(mountpoint, "[SWAP]") == 0);
        bool is_mounted = (fields == 3 && mountpoint[0] && strcmp(mountpoint, "-") != 0);

        if (is_swap) {
            if (run_command("swapoff %s", name) != 0) {
                rc = -1;
            }
        } else if (is_mounted) {
            if (run_command("umount -f %s", name) != 0) {
                rc = -1;
            }
        }

        if (strcmp(type, "crypt") == 0) {
            const char *mapper = strrchr(name, '/');
            mapper = mapper ? mapper + 1 : name;
            if (run_command("cryptsetup close %s", mapper) != 0) {
                rc = -1;
            }
        } else if (strcmp(type, "lvm") == 0) {
            if (run_command("lvchange -an %s", name) != 0) {
                rc = -1;
            }
        }
    }

    pclose(fp);
    if (deactivate_swap_for_disk(disk) != 0) {
        rc = -1;
    }
    return rc;
}

static void log_fs_probe(const char *device)
{
    if (!device || !device[0]) {
        return;
    }

    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "blkid -o export %s", device);
    char output[512];
    if (capture_command(cmd, output, sizeof(output)) == 0 && output[0]) {
        log_info("blkid export for %s:\n%s", device, output);
    } else {
        log_error("blkid probe failed for %s", device);
    }
}

static int select_disk(InstallerState *state)
{
    size_t count = 0;
    DiskInfo *disks = collect_disks(&count);
    if (!disks || count == 0) {
        ui_message("Disk Detection", "No suitable disks were detected.");
        free(disks);
        return -1;
    }

    char **items = calloc(count, sizeof(char *));
    for (size_t i = 0; i < count; ++i) {
        char size_buf[32];
        human_size(disks[i].size_mb, size_buf, sizeof(size_buf));
        char line[256];
        snprintf(line, sizeof(line), "%s - %s (%s)", disks[i].path, size_buf, disks[i].model);
        items[i] = strdup(line);
    }

    int choice = ui_menu("Select Target Disk", "Choose the disk that will be erased for Gentoo installation",
                         (const char **)items, count, 0);
    if (choice >= 0) {
        snprintf(state->target_disk, sizeof(state->target_disk), "%s", disks[choice].path);
        snprintf(state->disk_model, sizeof(state->disk_model), "%s", disks[choice].model);
        state->disk_size_mb = disks[choice].size_mb;
        state->disk_prepared = false;
        ui_message("Disk Selected", state->target_disk);
    }

    for (size_t i = 0; i < count; ++i) {
        free(items[i]);
    }
    free(items);
    free(disks);
    return (choice >= 0) ? 0 : -1;
}

static int choose_boot_mode(InstallerState *state)
{
    const char *items[] = {"Legacy BIOS (msdos)", "UEFI (GPT)"};
    int selected = (state->boot_mode == BOOTMODE_UEFI) ? 1 : 0;
    int choice = ui_menu("Boot Mode", "Select partition table and boot method", items, 2, selected);
    if (choice == 0) {
        state->boot_mode = BOOTMODE_LEGACY;
    } else if (choice == 1) {
        state->boot_mode = BOOTMODE_UEFI;
    }
    return (choice >= 0) ? 0 : -1;
}

static int choose_root_fs(InstallerState *state)
{
    const char *items[] = {"ext4", "xfs", "btrfs"};
    int choice = ui_menu("Root Filesystem", "Select filesystem for /", items, 3, state->root_fs);
    if (choice >= 0) {
        state->root_fs = (FilesystemType)choice;
    }
    return (choice >= 0) ? 0 : -1;
}

static int configure_swap(InstallerState *state)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%ld", state->swap_size_mb);
    if (ui_prompt_input("Swap Size", "Enter swap size in MB (0 to disable)", buffer, sizeof(buffer), buffer, false) != 0) {
        return -1;
    }
    long value = strtol(buffer, NULL, 10);
    if (value < 0) {
        value = 0;
    }
    state->swap_size_mb = value;
    return 0;
}

static int format_partition(const char *device, const char *type, const char *label)
{
    const char *fs_label = (label && label[0]) ? label : "LIBERO";
    if (strcmp(type, "boot") == 0) {
        return run_command("mkfs.ext2 -F -L %s %s", fs_label, device);
    }
    if (strcmp(type, "efi") == 0) {
        return run_command("mkfs.vfat -F32 -n %s %s", fs_label, device);
    }
    if (strcmp(type, "swap") == 0) {
        if (run_command("mkswap -L %s %s", fs_label, device) != 0) {
            return -1;
        }
        return run_command("swapon %s", device);
    }
    return -1;
}

static int format_root(const InstallerState *state, const char *label)
{
    const char *device = state->root_mapper[0] ? state->root_mapper : state->root_partition;
    const char *fs_label = (label && label[0]) ? label : LABEL_ROOT;
    switch (state->root_fs) {
    case FS_EXT4:
        return run_command("mkfs.ext4 -F -L %s %s", fs_label, device);
    case FS_XFS:
        return run_command("mkfs.xfs -f -L %s %s", fs_label, device);
    case FS_BTRFS:
        return run_command("mkfs.btrfs -f -L %s %s", fs_label, device);
    default:
        return -1;
    }
}

static int prompt_passphrase(char *buffer, size_t len)
{
    char pass1[128];
    char pass2[128];
    if (ui_prompt_input("Disk Encryption", "Enter LUKS passphrase", pass1, sizeof(pass1), "", true) != 0) {
        return -1;
    }
    if (ui_prompt_input("Disk Encryption", "Confirm LUKS passphrase", pass2, sizeof(pass2), "", true) != 0) {
        return -1;
    }
    if (strcmp(pass1, pass2) != 0) {
        ui_message("Passphrase Mismatch", "Passphrases did not match.");
        return -1;
    }
    snprintf(buffer, len, "%s", pass1);
    memset(pass1, 0, sizeof(pass1));
    memset(pass2, 0, sizeof(pass2));
    return 0;
}

static int handle_encryption(InstallerState *state)
{
    if (!state->use_luks) {
        snprintf(state->root_mapper, sizeof(state->root_mapper), "%s", state->root_partition);
        return 0;
    }

    char pass[128];
    if (prompt_passphrase(pass, sizeof(pass)) != 0) {
        return -1;
    }

    char key_file[] = "/tmp/libero-luks.keyXXXXXX";
    int fd = mkstemp(key_file);
    if (fd < 0) {
        ui_message("Encryption", "Unable to create temporary key file.");
        return -1;
    }
    size_t pass_len = strlen(pass);
    ssize_t written = write(fd, pass, pass_len);
    if (written < 0 || (size_t)written != pass_len) {
        ui_message("Encryption", "Unable to write temporary key file.");
        close(fd);
        unlink(key_file);
        memset(pass, 0, sizeof(pass));
        return -1;
    }
    close(fd);
    chmod(key_file, 0600);
    memset(pass, 0, sizeof(pass));

    int rc = run_command("cryptsetup luksFormat --type luks1 --batch-mode --key-file %s %s",
                         key_file, state->root_partition);
    if (rc == 0) {
        rc = run_command("cryptsetup open --key-file %s %s %s",
                         key_file, state->root_partition, state->luks_name);
    }
    unlink(key_file);

    if (rc != 0) {
        return -1;
    }
    snprintf(state->root_mapper, sizeof(state->root_mapper), "/dev/mapper/%s", state->luks_name);
    return 0;
}

static int handle_lvm(InstallerState *state)
{
    if (!state->use_lvm) {
        snprintf(state->root_mapper, sizeof(state->root_mapper), state->root_mapper[0] ? state->root_mapper : state->root_partition);
        return 0;
    }

    const char *pv = state->root_mapper[0] ? state->root_mapper : state->root_partition;
    if (run_command("pvcreate %s", pv) != 0) {
        return -1;
    }
    if (run_command("vgcreate %s %s", state->vg_name, pv) != 0) {
        return -1;
    }

    if (state->swap_size_mb > 0) {
        if (run_command("lvcreate -n swap -L %ldM %s", state->swap_size_mb, state->vg_name) != 0) {
            return -1;
        }
        snprintf(state->swap_mapper, sizeof(state->swap_mapper), "/dev/%s/swap", state->vg_name);
    } else {
        state->swap_mapper[0] = '\0';
    }

    if (run_command("lvcreate -n root -l 100%%FREE %s", state->vg_name) != 0) {
        return -1;
    }
    snprintf(state->root_mapper, sizeof(state->root_mapper), "/dev/%s/root", state->vg_name);
    return 0;
}

static int apply_partitioning(InstallerState *state)
{
    if (!state->target_disk[0]) {
        ui_message("Disk", "No disk selected.");
        return -1;
    }
    if (state->disk_size_mb <= 0) {
        long size = get_disk_size_mb(state->target_disk);
        if (size <= 0) {
            ui_message("Disk", "Unable to obtain disk size.");
            return -1;
        }
        state->disk_size_mb = size;
    }

    if (!ui_confirm("Partition Disk", "This will destroy all data on the selected disk. Continue?")) {
        return -1;
    }

    if (deactivate_disk_usage(state->target_disk) != 0) {
        ui_message("Disk", "Unable to release the disk. Close any mounts or LVM/LUKS mappings and try again.");
        return -1;
    }

    if (run_command("/usr/sbin/wipefs -a %s", state->target_disk) != 0) {
        return -1;
    }

    const bool use_gpt = (state->boot_mode == BOOTMODE_UEFI);
    const bool create_swap_partition = (!state->use_lvm && state->swap_size_mb > 0);

    double consumed_mb = 1.0;
    if (use_gpt) {
        consumed_mb += 512.0;
    }
    consumed_mb += 512.0;
    if (create_swap_partition) {
        consumed_mb += state->swap_size_mb;
    }

    double root_end = state->disk_size_mb - 8;
    if (root_end <= consumed_mb + 128) {
        ui_message("Partitioning", "Not enough space for root filesystem.");
        return -1;
    }

    PartitionSpec plan[MAX_PARTITION_SPECS] = {0};
    size_t plan_count = 0;
    int next_part = 1;

    if (use_gpt) {
        if (!plan_add_partition(plan, &plan_count, next_part++, "efi", "+512M",
                                LABEL_EFI, GPT_TYPE_EFI, NULL)) {
            ui_message("Partitioning", "Unable to plan EFI partition.");
            return -1;
        }
        if (!plan_add_partition(plan, &plan_count, next_part++, "boot", "+512M",
                                LABEL_BOOT, GPT_TYPE_LINUX, MBR_TYPE_LINUX)) {
            ui_message("Partitioning", "Unable to plan boot partition.");
            return -1;
        }
    } else {
        state->efi_partition[0] = '\0';
        state->boot_partition[0] = '\0';
    }

    if (create_swap_partition) {
        char swap_size[32];
        snprintf(swap_size, sizeof(swap_size), "+%ldM", state->swap_size_mb);
        if (!plan_add_partition(plan, &plan_count, next_part++, "swap", swap_size,
                                LABEL_SWAP, GPT_TYPE_SWAP, MBR_TYPE_SWAP)) {
            ui_message("Partitioning", "Unable to plan swap partition.");
            return -1;
        }
    } else {
        state->swap_partition[0] = '\0';
    }

    const char *root_gpt = state->use_lvm ? GPT_TYPE_LVM : GPT_TYPE_LINUX;
    const char *root_mbr = state->use_lvm ? MBR_TYPE_LVM : MBR_TYPE_LINUX;
    if (!plan_add_partition(plan, &plan_count, next_part++, "root", "-8M",
                            LABEL_ROOT, root_gpt, root_mbr)) {
        ui_message("Partitioning", "Unable to plan root partition.");
        return -1;
    }

    char summary[1024];
    summarize_partition_plan(state->target_disk, state->disk_size_mb, plan, plan_count, summary, sizeof(summary));
    ui_message("Partition Layout", summary);

    char script_path[] = "/tmp/libero-fdiskXXXXXX";
    int script_fd = mkstemp(script_path);
    if (script_fd < 0) {
        ui_message("Partitioning", "Unable to create script file for fdisk.");
        return -1;
    }
    for (size_t i = 0; i < plan_count; ++i) {
        PartitionSpec *spec = &plan[i];
        dprintf(script_fd, "n\n");
        if (state->boot_mode == BOOTMODE_LEGACY) {
            dprintf(script_fd, "p\n");
        }
        dprintf(script_fd, "%d\n\n%s\n", spec->part_number, spec->size_spec[0] ? spec->size_spec : "");
    }
    if (state->boot_mode == BOOTMODE_LEGACY) {
        PartitionSpec *boot_spec = find_partition_spec(plan, plan_count, "boot");
        if (boot_spec) {
            dprintf(script_fd, "a\n%d\n", boot_spec->part_number);
        }
    }
    dprintf(script_fd, "w\n");
    close(script_fd);

    if (run_command("/usr/sbin/fdisk %s < %s", state->target_disk, script_path) != 0) {
        unlink(script_path);
        ui_message("Partitioning", "fdisk failed to apply the partition layout.");
        return -1;
    }
    unlink(script_path);

    for (size_t i = 0; i < plan_count; ++i) {
        PartitionSpec *spec = &plan[i];
        const char *suffix = (isdigit((unsigned char)state->target_disk[strlen(state->target_disk) - 1]) ? "p" : "");
        int written = snprintf(spec->device, sizeof(spec->device), "%s%s%d",
                               state->target_disk, suffix, spec->part_number);
        if (written < 0 || written >= (int)sizeof(spec->device)) {
            ui_message("Partitioning", "Generated partition path is too long.");
            return -1;
        }
        if (strcmp(spec->role, "efi") == 0) {
            snprintf(state->efi_partition, sizeof(state->efi_partition), "%s", spec->device);
        } else if (strcmp(spec->role, "boot") == 0) {
            snprintf(state->boot_partition, sizeof(state->boot_partition), "%s", spec->device);
        } else if (strcmp(spec->role, "swap") == 0) {
            snprintf(state->swap_partition, sizeof(state->swap_partition), "%s", spec->device);
        } else if (strcmp(spec->role, "root") == 0) {
            snprintf(state->root_partition, sizeof(state->root_partition), "%s", spec->device);
        }
    }

    for (size_t i = 0; i < plan_count; ++i) {
        PartitionSpec *spec = &plan[i];
        const char *type = use_gpt ? spec->gpt_type : spec->mbr_type;
        if (type && type[0]) {
            if (run_command("sfdisk --part-type %s %d %s", state->target_disk, spec->part_number, type) != 0) {
                ui_message("Partitioning", "Unable to set partition type.");
                return -1;
            }
        }
    }

    if (run_command("partprobe %s", state->target_disk) != 0) {
        return -1;
    }

    if (state->boot_partition[0]) {
        if (format_partition(state->boot_partition, "boot", LABEL_BOOT) != 0) {
            return -1;
        }
    }
    if (state->efi_partition[0]) {
        if (format_partition(state->efi_partition, "efi", LABEL_EFI) != 0) {
            return -1;
        }
    }

    if (handle_encryption(state) != 0) {
        return -1;
    }
    if (handle_lvm(state) != 0) {
        return -1;
    }
    if (format_root(state, LABEL_ROOT) != 0) {
        return -1;
    }

    if (!state->use_lvm && state->swap_partition[0] && state->swap_size_mb > 0) {
        if (format_partition(state->swap_partition, "swap", LABEL_SWAP) != 0) {
            return -1;
        }
        snprintf(state->swap_mapper, sizeof(state->swap_mapper), "%s", state->swap_partition);
    } else if (state->use_lvm && state->swap_mapper[0]) {
        if (run_command("mkswap -L %s %s", LABEL_SWAP, state->swap_mapper) != 0) {
            return -1;
        }
        if (run_command("swapon %s", state->swap_mapper) != 0) {
            return -1;
        }
    }

    state->disk_prepared = false;

    ui_message("Partitioning Complete",
               "Disk partitioning and formatting finished. Use Disk preparation -> Mount target root partition before continuing.");
    return 0;
}

int disk_workflow(InstallerState *state)
{
    while (1) {
        char subtitle[256];
        char disk_display[64];
        const char *disk_value = state->target_disk[0] ? state->target_disk : "<not set>";
        copy_with_ellipsis(disk_display, sizeof(disk_display), disk_value);
        snprintf(subtitle, sizeof(subtitle),
                 "Disk: %s | Mode: %s | FS: %s | Swap: %ld MB | LUKS: %s | LVM: %s",
                 disk_display,
                 boot_mode_to_string(state->boot_mode),
                 fs_to_string(state->root_fs),
                 state->swap_size_mb,
                 state->use_luks ? "On" : "Off",
                 state->use_lvm ? "On" : "Off");

        const char *items[] = {
            "Select target disk",
            "Select boot mode",
            "Select root filesystem",
            "Configure swap size",
            "Toggle LUKS encryption",
            "Toggle LVM support",
            "Partition and format",
            "Mount target root partition",
            "Back to main menu",
        };

        int choice = ui_menu("Disk Preparation", subtitle, items, 9, 0);
        if (choice < 0 || choice == 8) {
            return 0;
        }

        switch (choice) {
        case 0:
            select_disk(state);
            break;
        case 1:
            choose_boot_mode(state);
            break;
        case 2:
            choose_root_fs(state);
            break;
        case 3:
            configure_swap(state);
            break;
        case 4:
            state->use_luks = !state->use_luks;
            break;
        case 5:
            state->use_lvm = !state->use_lvm;
            break;
        case 6:
            apply_partitioning(state);
            break;
        case 7:
            disk_mount_targets(state);
            break;
        default:
            break;
        }
    }
}

int disk_mount_targets(InstallerState *state)
{
    if (!state) {
        return -1;
    }
    if (!state->target_disk[0]) {
        ui_message("Mount", "Select a target disk first.");
        return -1;
    }
    if (!state->root_partition[0]) {
        ui_message("Mount", "No root partition recorded. Run partitioning first.");
        return -1;
    }

    char root_device[PATH_MAX];
    snprintf(root_device, sizeof(root_device), "%s", state->root_mapper[0] ? state->root_mapper : state->root_partition);

    if (ensure_directory(state->install_root, 0755) != 0) {
        log_error("Unable to create install root directory %s: %s", state->install_root, strerror(errno));
        ui_message("Mount", "Unable to create install root directory.");
        return -1;
    }

    if (is_path_mounted(state->install_root)) {
        state->disk_prepared = true;
        log_info("Install root already mounted at %s", state->install_root);
        ui_message("Mount", "Root partition already mounted.");
        return 0;
    }

    log_info("Attempting to mount root device %s to %s as %s",
             root_device, state->install_root, fs_to_string(state->root_fs));
    log_fs_probe(root_device);

    if (mount_fs(root_device, state->install_root, fs_to_string(state->root_fs), "") != 0) {
        ui_message("Mount", "Failed to mount the root partition. Check the log for details.");
        return -1;
    }

    state->disk_prepared = true;

    char old_stage3[PATH_MAX];
    char old_digest[PATH_MAX];
    char old_portage[PATH_MAX];
    snprintf(old_stage3, sizeof(old_stage3), "%s", state->stage3_local);
    snprintf(old_digest, sizeof(old_digest), "%s", state->stage3_digest_local);
    snprintf(old_portage, sizeof(old_portage), "%s", state->portage_local);

    char cache_dir[PATH_MAX];
    if (installer_state_cache_dir(state, true, cache_dir, sizeof(cache_dir)) == 0) {
        if (ensure_directory(cache_dir, 0755) == 0) {
            installer_state_set_cache_dir(state, cache_dir);
            migrate_cache_file(old_stage3, state->stage3_local);
            migrate_cache_file(old_digest, state->stage3_digest_local);
            migrate_cache_file(old_portage, state->portage_local);
        }
    }

    log_info("Mounted root device %s on %s", root_device, state->install_root);
    ui_message("Mount", "Root partition mounted at the install root.");
    return 0;
}
