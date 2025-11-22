#include "bootstrap.h"
#include "configure.h"
#include "disk.h"
#include "log.h"
#include "network.h"
#include "state.h"
#include "system_utils.h"
#include "ui.h"

static void show_log_location(void)
{
    char message[256];
    snprintf(message, sizeof(message), "Logs are stored at %s", log_get_path());
    ui_message("Installer Log", message);
}

int main(void)
{
    if (geteuid() != 0) {
        fprintf(stderr, "%s requires root privileges. Please run as root or via sudo.\n", INSTALLER_NAME);
        return 1;
    }

    InstallerState state;
    installer_state_init(&state);
    ensure_directory(INSTALL_CACHE_DIR, 0755);

    if (log_init(INSTALL_LOG_PATH) != 0) {
        char fallback[PATH_MAX];
        snprintf(fallback, sizeof(fallback), "%s/installer.log", INSTALL_CACHE_DIR);
        if (log_init(fallback) != 0) {
            fprintf(stderr, "Unable to initialize logging. Exiting.\n");
            return 1;
        }
    }

    if (ui_init() != 0) {
        fprintf(stderr, "Unable to initialize terminal UI.\n");
        log_close();
        return 1;
    }

    bool running = true;
    while (running) {
        char subtitle[256];
        snprintf(subtitle, sizeof(subtitle),
                 "Disk:%s | Net:%s | Stage3:%s | Boot:%s",
                 state.disk_prepared ? "ready" : "pending",
                 state.network_configured ? "ready" : "pending",
                 state.stage3_ready ? "ready" : "pending",
                 state.bootloader_installed ? "installed" : "pending");

        const char *items[] = {
            "Disk preparation",
            "Network configuration",
            "Bootstrap Gentoo (stage3/Portage)",
            "Configure and install system",
            "Show installer log path",
            "Exit installer",
        };

        int choice = ui_menu(INSTALLER_NAME, subtitle, items, 6, 0);
        if (choice < 0) {
            break;
        }

        switch (choice) {
        case 0:
            disk_workflow(&state);
            break;
        case 1:
            network_workflow(&state);
            break;
        case 2:
            bootstrap_workflow(&state);
            break;
        case 3:
            configure_workflow(&state);
            break;
        case 4:
            show_log_location();
            break;
        case 5:
            running = false;
            break;
        default:
            break;
        }
    }

    ui_message("Goodbye", "Installer exiting. Remember to unmount /mnt/gentoo before rebooting.");
    ui_shutdown();
    log_close();
    return 0;
}
