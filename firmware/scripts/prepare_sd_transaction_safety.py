#!/usr/bin/env python3
"""Generate a build-local SDMMC driver preserving transaction failures."""
import argparse
import hashlib
from pathlib import Path

EXPECTED_SHA256 = "fe02408164a826eeeacb463d38730741b2a3675cb1fd9afcc11cabe264914a2c"
CACHE_OLD = """    if (cmdinfo->data) {
        ret = esp_cache_msync((void *)cmdinfo->data, cache_sync_len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        if (ret != ESP_OK) {
            goto out;
        }
    }
"""
CACHE_NEW = """    // DMA reads (card -> RAM) require invalidation, DMA writes do not.
    // Cleanup must not turn a transfer timeout into a successful transaction.
    if (cmdinfo->data && (cmdinfo->flags & SCF_CMD_READ)) {
        esp_err_t cache_ret = esp_cache_msync((void *)cmdinfo->data, cache_sync_len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        if (ret == ESP_OK && cmdinfo->error == ESP_OK) ret = cache_ret;
    }
"""
CONTEXT = "    sd_host_sdmmc_slot_t *slot_ctx = __containerof(slot, sd_host_sdmmc_slot_t, drv);\n"
ADMISSION = """    if (!pk_sd_transaction_allowed(slot_ctx->slot_id, cmdinfo->data != NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
"""
BLOCK_TRANSACTION = """    bool block_data = cmdinfo->data != NULL &&
        (cmdinfo->opcode == MMC_READ_BLOCK_SINGLE || cmdinfo->opcode == MMC_READ_BLOCK_MULTIPLE ||
         cmdinfo->opcode == MMC_WRITE_BLOCK_SINGLE || cmdinfo->opcode == MMC_WRITE_BLOCK_MULTIPLE);
"""
CARD_ERROR = "    esp_err_t card_error = ESP_OK;\n"
START_OLD = """    ret = sd_host_slot_start_command(slot_ctx, hw_cmd, cmdinfo->arg);
    if (ret != ESP_OK) {
        goto out;
    }
"""
START_NEW = START_OLD.replace("        goto out;", "        card_error = ret;\n        goto out;")
APP_STATE = "    slot_ctx->ctlr->is_app_cmd = (ret == ESP_OK && cmdinfo->opcode == MMC_APP_CMD);\n"
CAPTURE_CARD_ERROR = """    // Capture only card/controller completion, before cache and PM cleanup.
    // Parameter/cache failures before starting DMA are not removal events.
    card_error = ret != ESP_OK ? ret : cmdinfo->error;
"""
ADMISSION = BLOCK_TRANSACTION + ADMISSION.replace("cmdinfo->data != NULL", "block_data")
LOCK_TAKE = "    xSemaphoreTake(slot_ctx->ctlr->mutex, portMAX_DELAY);\n"
LOCK_ADMISSION = """    // A queued command can have passed admission before the previous transfer
    // failed. Recheck after owning the host mutex, before starting any DMA.
    if (!pk_sd_transaction_allowed(slot_ctx->slot_id, block_data)) {
        ret = ESP_ERR_INVALID_STATE;
        goto out;
    }
"""
PM_ACQUIRE_OLD = '        ESP_GOTO_ON_ERROR(esp_pm_lock_acquire(slot_ctx->ctlr->pm_lock), out, TAG, "acquire pm_lock failed");\n'
PM_ACQUIRE_NEW = PM_ACQUIRE_OLD + "        pm_lock_acquired = true;\n"
PM_OLD = '        ESP_RETURN_ON_ERROR(esp_pm_lock_release(slot_ctx->ctlr->pm_lock), TAG, "release pm_lock failed");\n'
PM_NEW = """        esp_err_t pm_ret = esp_pm_lock_release(slot_ctx->ctlr->pm_lock);
        if (ret == ESP_OK && cmdinfo->error == ESP_OK) ret = pm_ret;
"""
RELEASE = "    xSemaphoreGive(slot_ctx->ctlr->mutex);\n"
NOTIFY = """    pk_sd_transaction_complete(slot_ctx->slot_id, cmdinfo->data != NULL,
                               card_error);
"""
NOTIFY = NOTIFY.replace("cmdinfo->data != NULL", "block_data")


def prepare_driver(source: Path, destination: Path) -> None:
    source, destination = Path(source), Path(destination)
    if source.resolve() == destination.resolve():
        raise ValueError("SDMMC overlay destination must not overwrite the SDK driver")
    raw = source.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != EXPECTED_SHA256:
        raise ValueError(f"Unsupported ESP-IDF SDMMC driver SHA256 {digest}; expected {EXPECTED_SHA256}. Review transaction cleanup before updating the overlay.")
    text = raw.decode("utf-8")
    # CONTEXT also exists in other public functions. Only replace inside this
    # transaction body; every other anchor must be unique within that body.
    start = text.index("esp_err_t sd_host_slot_sdmmc_do_transaction(")
    prefix, transaction = text[:start], text[start:]
    replacements = ((CONTEXT, CONTEXT + ADMISSION + CARD_ERROR + '#if CONFIG_PM_ENABLE\n    bool pm_lock_acquired = false;\n#endif\n'),
                    (CACHE_OLD, CACHE_NEW), (PM_ACQUIRE_OLD, PM_ACQUIRE_NEW),
                    (START_OLD, START_NEW), (APP_STATE, CAPTURE_CARD_ERROR + APP_STATE),
                    (LOCK_TAKE, LOCK_TAKE + LOCK_ADMISSION),
                    (PM_OLD, PM_NEW), (RELEASE, NOTIFY + RELEASE))
    for old, new in replacements:
        if transaction.count(old) != 1:
            raise ValueError(f"SDMMC overlay anchor must occur exactly once: {old.strip()}")
        transaction = transaction.replace(old, new, 1)
    # Only release a PM reference this call actually acquired.
    release_start = transaction.index("out:\n")
    cleanup = transaction[release_start:]
    condition = "if (slot_ctx->ctlr->pm_lock)"
    if cleanup.count(condition) != 1:
        raise ValueError("SDMMC PM cleanup anchor must occur exactly once")
    transaction = transaction[:release_start] + cleanup.replace(condition, "if (pm_lock_acquired)", 1)
    generated = (prefix + '#include "pk_sd_transaction_safety.h"\n' + transaction).encode("utf-8")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or destination.read_bytes() != generated:
        destination.write_bytes(generated)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        prepare_driver(args.source, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"SDMMC transaction overlay: {error}\n")


if __name__ == "__main__":
    main()
