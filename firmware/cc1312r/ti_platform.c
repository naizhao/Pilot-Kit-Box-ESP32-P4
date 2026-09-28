#include <stdbool.h>
#include <stddef.h>

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_ints.h)

#include <ti/drivers/Power.h>
#include <ti/drivers/power/PowerCC26X2.h>
#include <ti/drivers/rf/RF.h>

/* The RF and Power drivers normally receive these two objects from SysConfig.
 * This board has no TI LaunchPad BSP and only uses the RF client, so keeping the
 * two documented platform objects explicit makes the binary reproducible and
 * avoids inventing unused GPIO/UART driver instances. */
extern void PowerCC26XX_standbyPolicy(void);
extern bool PowerCC26XX_calibrate(unsigned int);

const PowerCC26X2_Config PowerCC26X2_config = {
    .enablePolicy = false,
    .policyInitFxn = NULL,
    .policyFxn = PowerCC26XX_standbyPolicy,
    .calibrateFxn = PowerCC26XX_calibrate,
    .calibrateRCOSC_LF = true,
    .calibrateRCOSC_HF = true,
    .enableTCXOFxn = NULL,
};

const RFCC26XX_HWAttrsV2 RFCC26XX_hwAttrs = {
    /* NoRTOS reserves LEVEL7 for its SwiP scheduler; HwiP rejects any other
     * LEVEL7 interrupt, which otherwise leaves RF power-up without an HWI. */
    .hwiPriority = INT_PRI_LEVEL6,
    /* Match TI SysConfig's NoRTOS default: RF must not starve priority-0 system work. */
    .swiPriority = 0,
    .xoscHfAlwaysNeeded = true,
    .globalCallback = NULL,
    .globalEventMask = 0,
};
