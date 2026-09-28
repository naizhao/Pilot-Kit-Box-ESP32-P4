#pragma once

#include <ti/drivers/rf/RF.h>
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/rf_common_cmd.h)
#include DeviceFamily_constructPath(driverlib/rf_prop_cmd.h)

extern RF_Mode g_uat_rf_mode;
extern rfc_CMD_PROP_RADIO_DIV_SETUP_t g_uat_radio_setup;
extern rfc_CMD_FS_t g_uat_fs;
extern rfc_CMD_PROP_RX_ADV_t g_uat_rx;
