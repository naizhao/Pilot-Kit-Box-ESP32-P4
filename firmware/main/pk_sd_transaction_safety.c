#include "pk_sd_transaction_safety.h"
#include <stdatomic.h>

static atomic_bool s_armed;
static atomic_int s_first_error;

void pk_sd_transaction_arm(void)
{
    atomic_store_explicit(&s_armed, true, memory_order_release);
}

bool pk_sd_transaction_allowed(int slot, bool block_data)
{
    return slot != 0 || !block_data || pk_sd_transaction_fault() == ESP_OK;
}

void pk_sd_transaction_complete(int slot, bool block_data, esp_err_t error)
{
    if (slot != 0 || !block_data || error == ESP_OK ||
        !atomic_load_explicit(&s_armed, memory_order_acquire)) return;
    int expected = ESP_OK;
    atomic_compare_exchange_strong_explicit(&s_first_error, &expected, error,
                                            memory_order_acq_rel, memory_order_acquire);
}

esp_err_t pk_sd_transaction_fault(void)
{
    return atomic_load_explicit(&s_first_error, memory_order_acquire);
}
