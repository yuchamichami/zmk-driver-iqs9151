/* Compile test: a real DT driver instance, with or without reset-gpios. */
#include <zephyr/device.h>
#include <zephyr/kernel.h>
int main(void) {
    return device_is_ready(DEVICE_DT_GET(DT_NODELABEL(test_trackpad))) ? 0 : -1;
}
