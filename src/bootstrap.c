#include "bootstrap.h"
#include "disk.h"
#include <stdarg.h>

static int safe_format(char *buffer, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int rc = vsnprintf(buffer, size, fmt, args);
    va_end(args);
    if (rc < 0 || (size_t)rc >= size) {
        log_error("Format overflow for pattern: %s", fmt);
        return -1;
    }
    return 0;
}

static int ensure_parent_directory(const char *path)
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

static int prepare_cache_dir(const InstallerState *state, char *cache_dir, size_t len)
{
    if (installer_state_cache_dir(state, true, cache_dir, len) != 0) {
        return -1;
    }
    if (ensure_directory(cache_dir, 0755) != 0) {
        log_error("Unable to create cache directory %s", cache_dir);
        return -1;
    }
    return 0;
}

static int select_arch(InstallerState *state)
{
    const char *items[] = {"i486 (generic)", "i686 (Pentium Pro+)"};
    int choice = ui_menu("Gentoo Architecture", "Select the stage3 architecture", items, 2, state->arch);
    if (choice >= 0) {
        state->arch = (choice == 0) ? ARCH_I486 : ARCH_I686;
    }
    return (choice >= 0) ? 0 : -1;
}

static int configure_mirror(InstallerState *state)
{
    char buffer[MIRROR_URL_MAX];
    snprintf(buffer, sizeof(buffer), "%s", state->mirror_url);
    if (ui_prompt_input("Mirror URL", "Enter Gentoo mirror URL", buffer, sizeof(buffer), buffer, false) != 0) {
        return -1;
    }
    snprintf(state->mirror_url, sizeof(state->mirror_url), "%s", buffer);
    return 0;
}

static int fetch_stage3_metadata(InstallerState *state)
{
    char meta_url[REMOTE_URL_MAX];
    snprintf(meta_url, sizeof(meta_url), "%s/latest-stage3-%s-systemd.txt",
             state->mirror_url, arch_to_string(state->arch));

    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd), "wget -qO- %s", meta_url);

    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        ui_message("Stage3", "Unable to query stage3 metadata.");
        return -1;
    }

    char stage3_path[256] = {0};
    char digest_path[256] = {0};
    char line[512];
    while (fgets(line, sizeof(line), pipe)) {
        if (line[0] == '#') {
            continue;
        }
        char token[256];
        if (sscanf(line, "%255s", token) != 1) {
            continue;
        }
        if (strstr(token, ".tar.xz.DIGESTS")) {
            snprintf(digest_path, sizeof(digest_path), "%s", token);
        } else if (strstr(token, ".tar.xz") && !strstr(token, ".CONTENTS")) {
            if (!stage3_path[0]) {
                snprintf(stage3_path, sizeof(stage3_path), "%s", token);
            }
        }
        if (stage3_path[0] && digest_path[0]) {
            break;
        }
    }
    pclose(pipe);

    if (!stage3_path[0]) {
        ui_message("Stage3", "Could not parse stage3 metadata.");
        return -1;
    }

    if (!digest_path[0]) {
        snprintf(digest_path, sizeof(digest_path), "%s.DIGESTS", stage3_path);
    }

    if (safe_format(state->stage3_url, sizeof(state->stage3_url), "%s/%s", state->mirror_url, stage3_path) != 0) {
        return -1;
    }
    if (safe_format(state->stage3_digest_url, sizeof(state->stage3_digest_url), "%s/%s",
                    state->mirror_url, digest_path) != 0) {
        return -1;
    }

    const char *base_stage3 = strrchr(stage3_path, '/');
    base_stage3 = base_stage3 ? base_stage3 + 1 : stage3_path;
    const char *base_digest = strrchr(digest_path, '/');
    base_digest = base_digest ? base_digest + 1 : digest_path;

    char cache_dir[PATH_MAX];
    if (prepare_cache_dir(state, cache_dir, sizeof(cache_dir)) != 0) {
        ui_message("Stage3", "Unable to prepare cache directory on the target disk.");
        return -1;
    }
    safe_format(state->stage3_local, sizeof(state->stage3_local), "%s/%s", cache_dir, base_stage3);
    safe_format(state->stage3_digest_local, sizeof(state->stage3_digest_local), "%s/%s", cache_dir, base_digest);

    char message[MAX_MESSAGE_LEN];
    snprintf(message, sizeof(message), "Latest stage3: %s", base_stage3);
    ui_message("Stage3 Metadata", message);
    return 0;
}

static int download_file(const char *url, const char *destination)
{
    if (!url[0] || !destination[0]) {
        return -1;
    }
    if (ensure_parent_directory(destination) != 0) {
        log_error("Unable to prepare directory for %s", destination);
        return -1;
    }
    return run_command("wget -O %s %s", destination, url);
}

static int download_stage3(InstallerState *state)
{
    state->disk_prepared = is_path_mounted(state->install_root);
    if (!state->disk_prepared) {
        ui_message("Download", "Root partition is not mounted at the install path. Use Disk preparation -> Mount target partitions, then try again.");
        return -1;
    }

    char cache_dir[PATH_MAX];
    if (prepare_cache_dir(state, cache_dir, sizeof(cache_dir)) != 0) {
        ui_message("Download", "Unable to prepare cache directory on the target disk.");
        return -1;
    }
    installer_state_set_cache_dir(state, cache_dir);

    if (!state->stage3_url[0]) {
        if (fetch_stage3_metadata(state) != 0) {
            return -1;
        }
    }
    if (download_file(state->stage3_url, state->stage3_local) != 0) {
        ui_message("Download", "Failed to download stage3 archive.");
        return -1;
    }
    if (download_file(state->stage3_digest_url, state->stage3_digest_local) != 0) {
        ui_message("Download", "Failed to download stage3 digest.");
        return -1;
    }
    ui_message("Download", "Stage3 archive and digest downloaded.");
    return 0;
}

static int parse_digest_hash(const char *digest_path, char *out_hash, size_t len)
{
    FILE *f = fopen(digest_path, "r");
    if (!f) {
        return -1;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == 0 || line[0] == '-') {
            continue;
        }
        if (!strstr(line, ".tar.xz") || strstr(line, ".DIGESTS") || strstr(line, ".CONTENTS")) {
            continue;
        }
        if (strstr(line, "SHA512")) {
            continue;
        }
        char hash[200];
        char filename[256];
        if (sscanf(line, "%199s %255s", hash, filename) == 2) {
            snprintf(out_hash, len, "%s", hash);
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static int verify_stage3(const InstallerState *state)
{
    char expected[200];
    if (parse_digest_hash(state->stage3_digest_local, expected, sizeof(expected)) != 0) {
        ui_message("Verification", "Unable to parse digest file.");
        return -1;
    }

    char cmd[MAX_CMD_LEN];
    if (safe_format(cmd, sizeof(cmd), "sha512sum %s", state->stage3_local) != 0) {
        return -1;
    }
    char actual[256];
    if (capture_command(cmd, actual, sizeof(actual)) != 0) {
        ui_message("Verification", "Failed to compute sha512 checksum.");
        return -1;
    }

    char *space = strchr(actual, ' ');
    if (space) {
        *space = '\0';
    }

    if (strcasecmp(expected, actual) != 0) {
        ui_message("Verification", "Stage3 checksum mismatch!");
        return -1;
    }

    ui_message("Verification", "Stage3 checksum verified.");
    return 0;
}

static int download_portage(InstallerState *state)
{
    if (!state->disk_prepared) {
        ui_message("Portage", "Prepare and mount the target disk before downloading Portage so it is stored on disk.");
        return -1;
    }

    char cache_dir[PATH_MAX];
    if (prepare_cache_dir(state, cache_dir, sizeof(cache_dir)) != 0) {
        ui_message("Portage", "Unable to prepare cache directory on the target disk.");
        return -1;
    }

    safe_format(state->portage_url, sizeof(state->portage_url), "%s/%s", PORTAGE_BASE_URL, PORTAGE_SNAPSHOT_NAME);
    safe_format(state->portage_local, sizeof(state->portage_local), "%s/%s", cache_dir, PORTAGE_SNAPSHOT_NAME);
    if (download_file(state->portage_url, state->portage_local) != 0) {
        ui_message("Portage", "Failed to download Portage snapshot.");
        return -1;
    }
    ui_message("Portage", "Portage snapshot downloaded.");
    return 0;
}

static int extract_stage3(InstallerState *state)
{
    if (!state->disk_prepared) {
        ui_message("Stage3", "Disk must be prepared and mounted before extraction.");
        return -1;
    }
    if (access(state->stage3_local, R_OK) != 0) {
        ui_message("Stage3", "Stage3 archive not downloaded yet.");
        return -1;
    }
    if (access(state->portage_local, R_OK) != 0) {
        ui_message("Portage", "Portage snapshot not downloaded yet.");
        return -1;
    }
    if (verify_stage3(state) != 0) {
        return -1;
    }

    if (run_command("tar xpf %s -C %s --xattrs-include='*.*' --numeric-owner",
                    state->stage3_local, state->install_root) != 0) {
        ui_message("Stage3", "Failed to extract stage3.");
        return -1;
    }

    if (run_command("tar xf %s -C %s/usr", state->portage_local, state->install_root) != 0) {
        ui_message("Portage", "Failed to extract Portage snapshot.");
        return -1;
    }

    state->stage3_ready = true;
    ui_message("Extraction", "Stage3 and Portage extracted.");
    return 0;
}

static int prepare_chroot(InstallerState *state)
{
    if (!state->stage3_ready) {
        ui_message("Chroot", "Stage3 must be extracted first.");
        return -1;
    }

    char dest[PATH_MAX];
    if (safe_format(dest, sizeof(dest), "%s/etc/resolv.conf", state->install_root) != 0) {
        return -1;
    }
    copy_file_simple("/etc/resolv.conf", dest);

    char path[PATH_MAX];
    safe_format(path, sizeof(path), "%s/dev", state->install_root);
    ensure_directory(path, 0755);
    safe_format(path, sizeof(path), "%s/sys", state->install_root);
    ensure_directory(path, 0755);
    safe_format(path, sizeof(path), "%s/proc", state->install_root);
    ensure_directory(path, 0755);
    safe_format(path, sizeof(path), "%s/run", state->install_root);
    ensure_directory(path, 0755);

    if (run_command("mount --rbind /dev %s/dev", state->install_root) != 0) {
        return -1;
    }
    if (run_command("mount --rbind /sys %s/sys", state->install_root) != 0) {
        return -1;
    }
    if (run_command("mount -t proc /proc %s/proc", state->install_root) != 0) {
        return -1;
    }
    if (run_command("mount --rbind /run %s/run", state->install_root) != 0) {
        return -1;
    }

    ui_message("Chroot", "Bind mounts prepared.");
    return 0;
}

int bootstrap_workflow(InstallerState *state)
{
    while (1) {
        char subtitle[256];
        char stage3_buf[64];
        const char *stage3_label = "not downloaded";
        if (state->stage3_local[0]) {
            stage3_label = state->stage3_local;
            const char *slash = strrchr(stage3_label, '/');
            if (slash && slash[1] != '\0') {
                stage3_label = slash + 1;
            }
            size_t len = strnlen(stage3_label, sizeof(stage3_buf) - 1);
            memcpy(stage3_buf, stage3_label, len);
            stage3_buf[len] = '\0';
            stage3_label = stage3_buf;
        }

        snprintf(subtitle, sizeof(subtitle),
                 "Arch: %s | Stage3: %s",
                 arch_to_string(state->arch),
                 stage3_label);

        const char *items[] = {
            "Select Gentoo architecture",
            "Configure download mirror",
            "Download stage3",
            "Download Portage snapshot",
            "Extract stage3 and Portage",
            "Prepare chroot environment",
            "Back to main menu",
        };

        int choice = ui_menu("Bootstrap Gentoo", subtitle, items, 7, 0);
        if (choice < 0 || choice == 6) {
            return 0;
        }

        switch (choice) {
        case 0:
            select_arch(state);
            break;
        case 1:
            configure_mirror(state);
            break;
        case 2:
            download_stage3(state);
            break;
        case 3:
            download_portage(state);
            break;
        case 4:
            extract_stage3(state);
            break;
        case 5:
            prepare_chroot(state);
            break;
        default:
            break;
        }
    }
}
