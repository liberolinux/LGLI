#ifndef LIBERO_INSTALLER_SYSTEM_UTILS_H
#define LIBERO_INSTALLER_SYSTEM_UTILS_H

#include "common.h"

int run_command(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int run_command_chroot(const char *root, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int chroot_run_script(const char *root, const char *script_body);
int capture_command(const char *cmd, char *output, size_t output_len);
int ensure_directory(const char *path, mode_t mode);
int copy_file_simple(const char *source, const char *destination);
int write_text_file(const char *path, const char *content);
int append_text_file(const char *path, const char *content);
int mount_fs(const char *device, const char *mountpoint, const char *fstype, const char *options);
int umount_path(const char *path);
int get_block_uuid(const char *device, char *buffer, size_t buffer_len);
long get_disk_size_mb(const char *device);
int shell_escape_single_quotes(const char *input, char *output, size_t output_len);
bool is_path_mounted(const char *path);

#endif /* LIBERO_INSTALLER_SYSTEM_UTILS_H */
