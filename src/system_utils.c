#include "system_utils.h"

#include <fcntl.h>
#include <sys/mount.h>
#include <sys/wait.h>

#include "log.h"
#include "ui.h"

bool is_path_mounted(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }

    char target[PATH_MAX];
    snprintf(target, sizeof(target), "%s", path);

    size_t len = strlen(target);
    while (len > 1 && target[len - 1] == '/') {
        target[--len] = '\0';
    }

    FILE *f = fopen("/proc/mounts", "r");
    if (!f) {
        return false;
    }

    char line[PATH_MAX * 2];
    bool mounted = false;
    while (fgets(line, sizeof(line), f)) {
        char dev[PATH_MAX];
        char mountpoint[PATH_MAX];
        if (sscanf(line, "%s %s", dev, mountpoint) != 2) {
            continue;
        }
        if (strcmp(mountpoint, target) == 0) {
            mounted = true;
            break;
        }
    }
    fclose(f);
    return mounted;
}

static void ensure_command_path(void)
{
    static bool path_set;

    if (path_set) {
        return;
    }

    const char *default_path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    const char *current = getenv("PATH");

    if (!current || !*current) {
        setenv("PATH", default_path, 1);
        path_set = true;
        return;
    }

    if (strstr(current, "/sbin") != NULL) {
        path_set = true;
        return;
    }

    char new_path[PATH_MAX];
    if (snprintf(new_path, sizeof(new_path), "%s:%s", default_path, current) >= (int)sizeof(new_path)) {
        setenv("PATH", default_path, 1);
    } else {
        setenv("PATH", new_path, 1);
    }

    path_set = true;
}

static int mkdir_p(const char *path, mode_t mode)
{
    if (!path || !*path) {
        return -1;
    }

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);

    size_t len = strlen(tmp);
    if (len == 0) {
        return -1;
    }

    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

int ensure_directory(const char *path, mode_t mode)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;
        }
        errno = ENOTDIR;
        return -1;
    }

    if (errno != ENOENT) {
        return -1;
    }

    return mkdir_p(path, mode);
}

int capture_command(const char *cmd, char *output, size_t output_len)
{
    if (!cmd || !output || output_len == 0) {
        return -1;
    }

    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        log_error("popen failed for command '%s': %s", cmd, strerror(errno));
        return -1;
    }

    if (fgets(output, (int)output_len, pipe) == NULL) {
        output[0] = '\0';
    }

    int status = pclose(pipe);
    if (!output[0]) {
        return status;
    }

    size_t len = strlen(output);
    while (len > 0 && (output[len - 1] == '\n' || output[len - 1] == '\r')) {
        output[--len] = '\0';
    }

    return status;
}

static void shorten_for_display(const char *input, char *output, size_t output_len)
{
    if (!output || output_len == 0) {
        return;
    }
    if (!input) {
        output[0] = '\0';
        return;
    }

    size_t len = strlen(input);
    if (len < output_len) {
        memcpy(output, input, len + 1);
        return;
    }

    if (output_len < 4) {
        output[0] = '\0';
        return;
    }

    size_t keep = output_len - 4;
    memcpy(output, input, keep);
    memcpy(output + keep, "...", 4);
}

static int run_formatted_command(char *buffer, size_t buffer_len, const char *fmt, va_list args)
{
    ensure_command_path();

    if (vsnprintf(buffer, buffer_len, fmt, args) >= (int)buffer_len) {
        log_error("Command too long");
        return -1;
    }

    log_info("Executing: %s", buffer);

    const char *log_path = log_get_path();
    char cmd_with_redirection[MAX_CMD_LEN * 2];

    if (log_path && *log_path) {
        if (snprintf(cmd_with_redirection, sizeof(cmd_with_redirection), "%s >> '%s' 2>&1", buffer, log_path) >= (int)sizeof(cmd_with_redirection)) {
            log_error("Redirected command too long");
            return -1;
        }
    } else {
        if (snprintf(cmd_with_redirection, sizeof(cmd_with_redirection), "%s >/dev/null 2>&1", buffer) >= (int)sizeof(cmd_with_redirection)) {
            log_error("Redirected command too long");
            return -1;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        log_error("fork() failed for %s: %s", buffer, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd_with_redirection, (char *)NULL);
        _exit(127);
    }

    char display_cmd[96];
    shorten_for_display(buffer, display_cmd, sizeof(display_cmd));

    int status = ui_wait_for_process("Running command", display_cmd, pid);
    if (status < 0) {
        int saved_errno = errno;
        log_error("Failed to wait for %s: %s", buffer, strerror(saved_errno));
        ui_error("Command Failed", "Unable to monitor child process. Check the installer log.");
        return -1;
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code != 0) {
            if (code == 127) {
                const char *path = getenv("PATH");
                log_error("Command not found: '%s' (PATH=%s)", buffer, path ? path : "(unset)");
            } else {
                log_error("Command '%s' exited with %d", buffer, code);
            }
            char message[256];
            if (code == 127) {
                snprintf(message, sizeof(message), "'%s' is not available (exit 127). See log: %s", display_cmd, log_get_path());
            } else {
                snprintf(message, sizeof(message), "'%s' failed (exit %d). See log: %s", display_cmd, code, log_get_path());
            }
            ui_error("Command Failed", message);
            return -code;
        }
        return 0;
    }

    if (WIFSIGNALED(status)) {
        log_error("Command '%s' terminated by signal %d", buffer, WTERMSIG(status));
    } else {
        log_error("Command '%s' terminated abnormally", buffer);
    }
    ui_error("Command Failed", "Process terminated unexpectedly. See the installer log for details.");
    return -1;
}

int run_command(const char *fmt, ...)
{
    char cmd[MAX_CMD_LEN];
    va_list args;
    va_start(args, fmt);
    int rc = run_formatted_command(cmd, sizeof(cmd), fmt, args);
    va_end(args);
    return rc;
}

int shell_escape_single_quotes(const char *input, char *output, size_t output_len)
{
    if (!input || !output || output_len == 0) {
        return -1;
    }

    size_t pos = 0;
    for (size_t i = 0; input[i] != '\0'; ++i) {
        if (input[i] == '\'') {
            const char *replacement = "'\"'\"'";
            size_t replacement_len = strlen(replacement);
            if (pos + replacement_len >= output_len) {
                return -1;
            }
            memcpy(output + pos, replacement, replacement_len);
            pos += replacement_len;
        } else {
            if (pos + 1 >= output_len) {
                return -1;
            }
            output[pos++] = input[i];
        }
    }
    output[pos] = '\0';
    return 0;
}

int run_command_chroot(const char *root, const char *fmt, ...)
{
    if (!root || !fmt) {
        return -1;
    }

    char inner[MAX_CMD_LEN];
    va_list args;
    va_start(args, fmt);
    if (vsnprintf(inner, sizeof(inner), fmt, args) >= (int)sizeof(inner)) {
        va_end(args);
        log_error("Chroot command too long");
        return -1;
    }
    va_end(args);

    char escaped[(MAX_CMD_LEN * 2)];
    if (shell_escape_single_quotes(inner, escaped, sizeof(escaped)) != 0) {
        log_error("Failed to escape chroot command");
        return -1;
    }

    const size_t overhead = strlen("chroot ") + strlen(" /bin/bash -lc '") + strlen("'") + 1;
    size_t needed = strlen(root) + strlen(escaped) + overhead;
    if (needed >= MAX_CMD_LEN) {
        log_error("Chroot command too long for buffer");
        return -1;
    }

    return run_command("chroot %s /bin/bash -lc '%s'", root, escaped);
}

int chroot_run_script(const char *root, const char *script_body)
{
    if (!root || !script_body) {
        return -1;
    }

    char tmp_dir[PATH_MAX];
    snprintf(tmp_dir, sizeof(tmp_dir), "%s/tmp", root);
    if (ensure_directory(tmp_dir, 0755) != 0) {
        log_error("Unable to ensure %s", tmp_dir);
        return -1;
    }

    char script_path[PATH_MAX];
    snprintf(script_path, sizeof(script_path), "%s/tmp/libero-installer.sh", root);

    FILE *f = fopen(script_path, "w");
    if (!f) {
        log_error("Failed to create %s: %s", script_path, strerror(errno));
        return -1;
    }

    fprintf(f, "#!/bin/bash\nset -euo pipefail\n%s\n", script_body);
    fclose(f);
    chmod(script_path, 0700);

    int rc = run_command("chroot %s /bin/bash /tmp/libero-installer.sh", root);
    unlink(script_path);
    return rc;
}

int copy_file_simple(const char *source, const char *destination)
{
    FILE *src = fopen(source, "rb");
    if (!src) {
        log_error("Unable to open %s: %s", source, strerror(errno));
        return -1;
    }

    char dest_tmp[PATH_MAX];
    snprintf(dest_tmp, sizeof(dest_tmp), "%s", destination);
    FILE *dst = fopen(dest_tmp, "wb");
    if (!dst) {
        log_error("Unable to open %s: %s", destination, strerror(errno));
        fclose(src);
        return -1;
    }

    char buffer[1 << 15];
    size_t read_bytes;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, read_bytes, dst) != read_bytes) {
            log_error("Failed to write %s: %s", destination, strerror(errno));
            fclose(src);
            fclose(dst);
            return -1;
        }
    }

    fclose(src);
    fclose(dst);
    return 0;
}

int write_text_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        log_error("Failed to write %s: %s", path, strerror(errno));
        return -1;
    }
    fputs(content, f);
    fclose(f);
    return 0;
}

int append_text_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "a");
    if (!f) {
        log_error("Failed to append %s: %s", path, strerror(errno));
        return -1;
    }
    fputs(content, f);
    fclose(f);
    return 0;
}

int mount_fs(const char *device, const char *mountpoint, const char *fstype, const char *options)
{
    if (ensure_directory(mountpoint, 0755) != 0) {
        log_error("Cannot create mountpoint %s", mountpoint);
        return -1;
    }

    const char *opts = options ? options : "";
    if (mount(device, mountpoint, fstype, 0, opts) != 0) {
        log_error("Failed to mount %s on %s (type=%s opts=%s): %s", device, mountpoint, fstype, opts, strerror(errno));
        return -1;
    }

    log_info("Mounted %s on %s (%s)", device, mountpoint, fstype);
    return 0;
}

int umount_path(const char *path)
{
    if (umount2(path, MNT_FORCE) != 0) {
        log_error("Failed to unmount %s: %s", path, strerror(errno));
        return -1;
    }
    log_info("Unmounted %s", path);
    return 0;
}

int get_block_uuid(const char *device, char *buffer, size_t buffer_len)
{
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "blkid -s UUID -o value %s", device);
    int rc = capture_command(cmd, buffer, buffer_len);
    if (rc != 0 || buffer[0] == '\0') {
        log_error("Unable to read UUID for %s", device);
        return -1;
    }
    return 0;
}

long get_disk_size_mb(const char *device)
{
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "blockdev --getsize64 %s", device);
    char output[128];
    if (capture_command(cmd, output, sizeof(output)) != 0) {
        return -1;
    }
    errno = 0;
    unsigned long long bytes = strtoull(output, NULL, 10);
    if (errno != 0) {
        return -1;
    }
    return (long)(bytes / (1024ULL * 1024ULL));
}
