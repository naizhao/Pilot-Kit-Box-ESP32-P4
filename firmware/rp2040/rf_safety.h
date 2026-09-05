/* rf_safety.h — F2：bias 关断 + RF 开关合法互补态，上电第一件事。 */
#pragma once
#include <stdbool.h>

typedef struct {
    int  gpio;
    bool level;
    const char *why;
} rf_safety_pin_t;

/* 纯函数（host 可测）。返回上电安全向量，*count 出条数。 */
const rf_safety_pin_t *rf_safety_boot_vector(int *count);

/* 上板应用：逐脚 init + 置位。 */
void rf_safety_apply_boot_state(void);
