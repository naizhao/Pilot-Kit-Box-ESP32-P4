#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Board microSD uses slot 0; Hosted C6 uses slot 1. The SDK hook must never
 * acquire the SD lifecycle mutex: transactions may hold the host/FAT locks.
 * block_data is true only for card block commands (CMD17/18/24/25), excluding
 * protocol capability probes. error must be a card/controller transaction
 * error, never a parameter, cache synchronization or PM cleanup result.
 * A real transfer error after mounting latches
 * until reboot. With no CD
 * pin, automatically treating a retry as a replacement card is unsafe. */
void pk_sd_transaction_arm(void);
bool pk_sd_transaction_allowed(int slot, bool block_data);
void pk_sd_transaction_complete(int slot, bool block_data, esp_err_t error);
esp_err_t pk_sd_transaction_fault(void);
