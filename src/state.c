#include "state.h"

void installer_state_init(InstallerState *state)
{
    if (!state) {
        return;
    }

    memset(state, 0, sizeof(*state));

    state->arch = ARCH_I486;
    state->root_fs = FS_EXT4;
    state->swap_size_mb = 1024;
    state->boot_mode = (access("/sys/firmware/efi/efivars", F_OK) == 0) ? BOOTMODE_UEFI : BOOTMODE_LEGACY;
    state->use_luks = false;
    state->use_lvm = false;
    state->disk_prepared = false;
    state->network_configured = false;
    state->stage3_ready = false;
    state->bootloader_installed = false;
    state->static_prefix = 24;

    snprintf(state->install_root, sizeof(state->install_root), "%s", INSTALL_ROOT_DEFAULT);
    snprintf(state->mirror_url, sizeof(state->mirror_url), "%s", STAGE3_BASE_URL);
    snprintf(state->portage_url, sizeof(state->portage_url), "%s/%s", PORTAGE_BASE_URL, PORTAGE_SNAPSHOT_NAME);
    snprintf(state->stage3_local, sizeof(state->stage3_local), INSTALL_CACHE_DIR "/stage3.tar.xz");
    snprintf(state->stage3_digest_local, sizeof(state->stage3_digest_local), INSTALL_CACHE_DIR "/stage3.tar.xz.DIGESTS");
    snprintf(state->portage_local, sizeof(state->portage_local), INSTALL_CACHE_DIR "/%s", PORTAGE_SNAPSHOT_NAME);
    snprintf(state->vg_name, sizeof(state->vg_name), "%s", DEFAULT_VG_NAME);
    snprintf(state->luks_name, sizeof(state->luks_name), "%s", DEFAULT_LUKS_NAME);

    snprintf(state->hostname, sizeof(state->hostname), "%s", DEFAULT_HOSTNAME);
    snprintf(state->timezone, sizeof(state->timezone), "%s", DEFAULT_TIMEZONE);
    snprintf(state->keymap, sizeof(state->keymap), "%s", DEFAULT_KEYMAP);
    snprintf(state->locale, sizeof(state->locale), "%s", DEFAULT_LOCALE);
    snprintf(state->lang, sizeof(state->lang), "%s", DEFAULT_LANG);
    state->create_user = true;
    snprintf(state->username, sizeof(state->username), "%s", "libero");
    state->root_password[0] = '\0';
    state->user_password[0] = '\0';

    state->network_interface[0] = '\0';
    state->network_dhcp = true;
    state->static_ip[0] = '\0';
    state->static_gateway[0] = '\0';
    state->static_dns[0] = '\0';

    state->target_disk[0] = '\0';
    state->disk_model[0] = '\0';
    state->disk_size_mb = 0;

    state->boot_partition[0] = '\0';
    state->efi_partition[0] = '\0';
    state->root_partition[0] = '\0';
    state->swap_partition[0] = '\0';
    state->root_mapper[0] = '\0';
    state->swap_mapper[0] = '\0';

    snprintf(state->stage3_url, sizeof(state->stage3_url), "%s", "");
    snprintf(state->stage3_digest_url, sizeof(state->stage3_digest_url), "%s", "");
}

const char *arch_to_string(GentooArch arch)
{
    switch (arch) {
    case ARCH_I486:
        return "i486";
    case ARCH_I686:
        return "i686";
    default:
        return "unknown";
    }
}

const char *boot_mode_to_string(BootMode mode)
{
    switch (mode) {
    case BOOTMODE_LEGACY:
        return "Legacy BIOS (MBR)";
    case BOOTMODE_UEFI:
        return "UEFI (GPT)";
    default:
        return "Unknown";
    }
}

const char *fs_to_string(FilesystemType fs)
{
    switch (fs) {
    case FS_EXT4:
        return "ext4";
    case FS_XFS:
        return "xfs";
    case FS_BTRFS:
        return "btrfs";
    default:
        return "unknown";
    }
}

static void filename_from_path(const char *path, const char *fallback, char *out, size_t len)
{
    if (!out || len == 0) {
        return;
    }
    const char *name = fallback;
    if (path && path[0]) {
        const char *slash = strrchr(path, '/');
        name = slash ? (slash + 1) : path;
    }
    if (!name || !name[0]) {
        name = fallback;
    }
    snprintf(out, len, "%s", name ? name : "");
}

static void build_cache_path(char *dest, size_t dest_len, const char *cache_dir, const char *name)
{
    if (!dest || dest_len == 0 || !cache_dir || !cache_dir[0] || !name || !name[0]) {
        return;
    }
    size_t dir_len = strlen(cache_dir);
    size_t name_len = strlen(name);
    size_t needed = dir_len + 1 + name_len + 1; /* dir + slash + name + null */
    if (needed > dest_len) {
        dest[0] = '\0';
        return;
    }

    memcpy(dest, cache_dir, dir_len);
    dest[dir_len] = '/';
    memcpy(dest + dir_len + 1, name, name_len);
    dest[dir_len + 1 + name_len] = '\0';
}

int installer_state_cache_dir(const InstallerState *state, bool prefer_install_root, char *buffer, size_t len)
{
    if (!state || !buffer || len == 0) {
        return -1;
    }

    const bool can_use_target = prefer_install_root && state->disk_prepared && state->install_root[0];
    const char *base = can_use_target ? state->install_root : "";
    const char *suffix = INSTALL_CACHE_DIR;

    if (snprintf(buffer, len, "%s%s", base, suffix) >= (int)len) {
        return -1;
    }
    return 0;
}

void installer_state_set_cache_dir(InstallerState *state, const char *cache_dir)
{
    if (!state || !cache_dir || !cache_dir[0]) {
        return;
    }

    char stage3_name[PATH_MAX];
    char digest_name[PATH_MAX];
    char portage_name[PATH_MAX];

    filename_from_path(state->stage3_local, "stage3.tar.xz", stage3_name, sizeof(stage3_name));
    filename_from_path(state->stage3_digest_local, "stage3.tar.xz.DIGESTS", digest_name, sizeof(digest_name));
    filename_from_path(state->portage_local, PORTAGE_SNAPSHOT_NAME, portage_name, sizeof(portage_name));

    build_cache_path(state->stage3_local, sizeof(state->stage3_local), cache_dir, stage3_name);
    build_cache_path(state->stage3_digest_local, sizeof(state->stage3_digest_local), cache_dir, digest_name);
    build_cache_path(state->portage_local, sizeof(state->portage_local), cache_dir, portage_name);
}
