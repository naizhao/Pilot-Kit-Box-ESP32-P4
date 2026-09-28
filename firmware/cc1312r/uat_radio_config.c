#include "uat_radio_config.h"

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/rf_mailbox.h)
#include DeviceFamily_constructPath(rf_patches/rf_patch_cpe_prop.h)

/* SmartRF Studio 2.32.0, CC1312R Rev E, 978 MHz UAT receive profile.
 * These are factual generated register values, represented independently in
 * this RX-only firmware.  No transmit operation is present anywhere in the
 * command graph.  See RF_PARAMETERS.md for requested/quantized values. */
static uint32_t g_uat_overrides[] = {
    0x00F788D3u,
    0x4001405Du,
    0x08141131u,
    ADI_2HALFREG_OVERRIDE(0, 16, 0x8, 0x8, 17, 0x1, 0x1),
    HW_REG_OVERRIDE(0x609C, 0x002E),
    0x000488A3u,
    ADI_HALFREG_OVERRIDE(0, 5, 0xF, 0x2),
    ADI_HALFREG_OVERRIDE(0, 61, 0xF, 0x0),
    HW_REG_OVERRIDE(0x6028, 0x001A),
    0x00000343u,
    0xFFFFFFFFu,
};

RF_Mode g_uat_rf_mode = {
    .rfMode = RF_MODE_AUTO,
    .cpePatchFxn = &rf_patch_cpe_prop,
    .mcePatchFxn = 0,
    .rfePatchFxn = 0,
};

rfc_CMD_PROP_RADIO_DIV_SETUP_t g_uat_radio_setup = {
    .commandNo = CMD_PROP_RADIO_DIV_SETUP,
    .condition.rule = COND_ALWAYS,
    .modulation = {
        .modType = 1,
        .deviation = 0x04E2,
        .deviationStepSz = 0,
    },
    .symbolRate = {
        .preScale = 0x0F,
        .rateWord = 0x0A6BFB,
        .decimMode = 0,
    },
    .rxBw = 0x64,
    .preamConf = {
        .nPreamBytes = 0,
        .preamMode = 0,
    },
    .formatConf = {
        .nSwBits = 32,
        .bBitReversal = 0,
        .bMsbFirst = 1,
        .fecMode = 0,
        .whitenMode = 0,
    },
    .config = {
        .frontEndMode = 0,
        .biasMode = 1,
        .analogCfgMode = 0,
        .bNoFsPowerUp = 0,
        .bSynthNarrowBand = 0,
    },
    .txPower = 0x003F,
    .pRegOverride = g_uat_overrides,
    .centerFreq = 978,
    .intFreq = 0x0D99,
    .loDivider = 5,
};

rfc_CMD_FS_t g_uat_fs = {
    .commandNo = CMD_FS,
    .condition.rule = COND_ALWAYS,
    .frequency = 978,
    .fractFreq = 0,
    .synthConf = {
        .bTxMode = 0,
        .refFreq = 0,
    },
};

rfc_CMD_PROP_RX_ADV_t g_uat_rx = {
    .commandNo = CMD_PROP_RX_ADV,
    .startTrigger.triggerType = TRIG_NOW,
    .condition.rule = COND_NEVER,
    .pktConf = {
        .bFsOff = 0,
        /* End after every packet.  The main loop immediately reposts RX; this
         * keeps one SET_LEN decision bound to one partial entry even when RF
         * events coalesce, instead of sharing mutable length state across two
         * back-to-back packets. */
        .bRepeatOk = 0,
        .bRepeatNok = 0,
        .bUseCrc = 0,
        .bCrcIncSw = 0,
        .bCrcIncHdr = 0,
        .endType = 0,
        .filterOp = 0,
    },
    .rxConf = {
        .bAutoFlushIgnored = 0,
        .bAutoFlushCrcErr = 0,
        .bIncludeHdr = 1,
        .bIncludeCrc = 0,
        .bAppendRssi = 1,
        .bAppendTimestamp = 1,
        .bAppendStatus = 1,
    },
    /* RF Core matches the high 32 bits.  hdrConf then captures the remaining
     * low sync nibble (2/D); only after that byte does the UAT payload begin. */
    .syncWord0 = 0xEACDDA4Eu,
    .syncWord1 = 0x153225B1u,
    .maxPktLen = 0,
    .hdrConf = {
        .numHdrBits = 4,
        .lenPos = 0,
        .numLenBits = 0,
    },
    .addrConf = {0},
    .lenOffset = 0,
    .endTrigger.triggerType = TRIG_NEVER,
};
