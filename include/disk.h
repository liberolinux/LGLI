#ifndef LIBERO_INSTALLER_DISK_H
#define LIBERO_INSTALLER_DISK_H

#include "state.h"
#include "ui.h"
#include "system_utils.h"
#include "log.h"

int disk_workflow(InstallerState *state);
int disk_mount_targets(InstallerState *state);

#endif /* LIBERO_INSTALLER_DISK_H */
