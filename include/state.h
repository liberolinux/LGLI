#ifndef LIBERO_INSTALLER_STATE_H
#define LIBERO_INSTALLER_STATE_H

#include "common.h"

typedef enum {
    ARCH_I486 = 0,
    ARCH_I686
} GentooArch;

typedef enum {
    BOOTMODE_LEGACY = 0,
    BOOTMODE_UEFI
} BootMode;

typedef enum {
    FS_EXT4 = 0,
    FS_XFS,
    FS_BTRFS
} FilesystemType;

typedef struct InstallerState {
    GentooArch arch;
    BootMode boot_mode;
    FilesystemType root_fs;
    bool use_luks;
    bool use_lvm;
    bool disk_prepared;
    bool network_configured;
    bool stage3_ready;
    bool bootloader_installed;

    char install_root[PATH_MAX];
    char target_disk[PATH_MAX];
    char disk_model[128];
    long disk_size_mb;
    long swap_size_mb;

    char boot_partition[PATH_MAX];
    char efi_partition[PATH_MAX];
    char root_partition[PATH_MAX];
    char swap_partition[PATH_MAX];
    char root_mapper[PATH_MAX];
    char swap_mapper[PATH_MAX];
    char vg_name[64];
    char luks_name[64];

    char hostname[64];
    char timezone[64];
    char keymap[64];
    char locale[64];
    char lang[64];
    char root_password[128];
    bool create_user;
    char username[64];
    char user_password[128];

    char network_interface[64];
    bool network_dhcp;
    char static_ip[64];
    int static_prefix;
    char static_gateway[64];
    char static_dns[128];

    char mirror_url[MIRROR_URL_MAX];
    char stage3_url[REMOTE_URL_MAX];
    char stage3_digest_url[REMOTE_URL_MAX];
    char stage3_local[PATH_MAX];
    char stage3_digest_local[PATH_MAX];
    char portage_url[REMOTE_URL_MAX];
    char portage_local[PATH_MAX];
} InstallerState;

void installer_state_init(InstallerState *state);
const char *arch_to_string(GentooArch arch);
const char *boot_mode_to_string(BootMode mode);
const char *fs_to_string(FilesystemType fs);
int installer_state_cache_dir(const InstallerState *state, bool prefer_install_root, char *buffer, size_t len);
void installer_state_set_cache_dir(InstallerState *state, const char *cache_dir);

#endif /* LIBERO_INSTALLER_STATE_H */
