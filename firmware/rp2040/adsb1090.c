/* 临时 blink 占位（Task 7）：仅验证工具链与构建链路，Task 14 换正式实现。 */
#include "pico/stdlib.h"

int main(void)
{
    const uint LED = PICO_DEFAULT_LED_PIN;
    gpio_init(LED);
    gpio_set_dir(LED, GPIO_OUT);
    while (true) {
        gpio_put(LED, 1);
        sleep_ms(100);
        gpio_put(LED, 0);
        sleep_ms(100);
    }
}
