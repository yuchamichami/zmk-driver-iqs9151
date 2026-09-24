/* SPDX-License-Identifier: MIT */
#include <zephyr/ztest.h>
#include <zephyr/sys/byteorder.h>
#include "iqs9151_regs.h"
#include "iqs9151_test.h"

static int gpio_result;
static gpio_port_value_t gpio_level;
static int transfer_result;
static unsigned int transfers;
static uint8_t last_write[8];
static size_t last_size;
static bool emulate_sensor;
static bool defer_alp_ati;
static bool drop_fine_write;
static uint8_t registers[0x400];
static unsigned int ati_requests;


static int mock_gpio_get(const struct device *dev, gpio_port_value_t *value) {
    ARG_UNUSED(dev);
    *value = gpio_level;
    return gpio_result;
}
static struct gpio_driver_data gpio_data = {.invert = BIT(0)};
static const struct gpio_driver_config gpio_config = {.port_pin_mask = BIT(0)};
static const struct gpio_driver_api gpio_api = {.port_get_raw = mock_gpio_get};
DEVICE_DEFINE(test_gpio, "test_gpio", NULL, NULL, &gpio_data, &gpio_config,
              POST_KERNEL, 0, &gpio_api);

static int mock_transfer(const struct device *dev, struct i2c_msg *msgs,
                         uint8_t count, uint16_t addr) {
    ARG_UNUSED(dev);
    zassert_equal(addr, 0x56);
    if (emulate_sensor) {
        transfers++;
        if (transfer_result != 0) {
            return transfer_result;
        }
        uint16_t reg = sys_get_le16(msgs[0].buf);
        zassert_true(reg >= 0x1000 && reg < 0x1400);
        size_t offset = reg - 0x1000;
        if (count == 2) {
            zassert_equal(msgs[0].len, 2);
            zassert_true(msgs[1].flags & I2C_MSG_READ);
            zassert_true(offset + msgs[1].len <= sizeof(registers));
            memcpy(msgs[1].buf, registers + offset, msgs[1].len);
        } else {
            zassert_equal(count, 1);
            zassert_true(offset + msgs[0].len - 2 <= sizeof(registers));
            if (!(drop_fine_write && reg == IQS9151_ADDR_ATI_MULTIPLIERS)) {
                memcpy(registers + offset, msgs[0].buf + 2, msgs[0].len - 2);
            }
            if (reg == 0x11BC && sys_get_le16(registers + offset) == BIT(9)) {
                gpio_level = 0; /* reset resumes streaming */
                registers[0x20] = BIT(7);
            }
            if (reg == 0x11BC && (registers[offset] & 0x60) == 0x60) {
                ati_requests++;
                registers[offset] &= ~(defer_alp_ati ? 0x20 : 0x60); /* ALP waits for LP mode */
            }
        }
        return 0;
    }
    zassert_equal(count, 1);
    zassert_true(msgs[0].flags & I2C_MSG_STOP);
    zassert_true(msgs[0].len <= sizeof(last_write));
    last_size = msgs[0].len;
    memcpy(last_write, msgs[0].buf, last_size);
    transfers++;
    return transfer_result;
}
static const struct i2c_driver_api i2c_api = {.transfer = mock_transfer};
DEVICE_DEFINE(test_i2c, "test_i2c", NULL, NULL, NULL, NULL,
              POST_KERNEL, 0, &i2c_api);
static const struct gpio_dt_spec irq = {
    .port = DEVICE_GET(test_gpio), .pin = 0, .dt_flags = GPIO_ACTIVE_LOW,
};
static const struct i2c_dt_spec bus = {.bus = DEVICE_GET(test_i2c), .addr = 0x56};

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    gpio_result = 0;
    gpio_level = 0; /* physical LOW == ready */
    transfer_result = 0;
    transfers = 0;
    last_size = 0;
    emulate_sensor = false;
    defer_alp_ati = false;
    drop_fine_write = false;
    ati_requests = 0;
    memset(registers, 0, sizeof(registers));
}

ZTEST(iqs9151_profile, test_ati_register_and_byte_order) {
    zassert_ok(iqs9151_test_ati(&bus, &irq));
    const uint8_t expected[] = {0xBC, 0x11, 0x60, 0x00};
    zassert_equal(last_size, sizeof(expected));
    zassert_mem_equal(last_write, expected, sizeof(expected));
    zassert_equal(transfers, 1);
}
ZTEST(iqs9151_profile, test_gpio_error_does_not_send_i2c) {
    gpio_result = -EIO;
    zassert_equal(iqs9151_test_ati(&bus, &irq), -EIO);
    zassert_equal(transfers, 0);
}
ZTEST(iqs9151_profile, test_inactive_rdy_times_out) {
    gpio_level = BIT(0);
    zassert_equal(iqs9151_test_ready(&irq, 2), -ETIMEDOUT);
    zassert_equal(transfers, 0);
}
ZTEST(iqs9151_profile, test_active_low_rdy_is_ready) {
    zassert_ok(iqs9151_test_ready(&irq, 2));
}
ZTEST(iqs9151_profile, test_i2c_failure_is_propagated) {
    transfer_result = -EIO;
    zassert_equal(iqs9151_test_ati(&bus, &irq), -EIO);
}
ZTEST(iqs9151_profile, test_geometry_and_overlay_tables) {
    size_t size;
    const uint8_t *settings = iqs9151_test_config_block(0x1178, &size);
    zassert_true(size > 0x11EF - 0x1178);
    /* Deliberately non-default values: prove tuning reaches the wire table. */
    zassert_equal(settings[0x11CC - 0x1178], 37);
    zassert_equal(settings[0x11CD - 0x1178], 29);
    zassert_equal(settings[0x11CE - 0x1178], 9);
    zassert_equal(settings[0x11CF - 0x1178], 11);
#ifdef CONFIG_INPUT_IQS9151_TPS43_A2
    zassert_equal(settings[0x11E3 - 0x1178], 13);
    zassert_equal(settings[0x11E4 - 0x1178], 12);
    zassert_equal(sys_get_le16(settings + 0x11E6 - 0x1178), 3600);
    zassert_equal(sys_get_le16(settings + 0x11E8 - 0x1178), 3300);
    const uint8_t alp[] = {0, 0, 0, 0, 0xFE, 0x2F};
    zassert_mem_equal(settings + 0x11C6 - 0x1178, alp, sizeof(alp));
    const uint8_t expected[] = {12,11,10,9,8,7,6,5,4,3,2,1,0,
                               45,43,42,41,40,39,38,37,36,35,34,33};
    const uint8_t *map = iqs9151_test_config_block(0x1218, &size);
    zassert_equal(size, 46);
    zassert_mem_equal(map, expected, sizeof(expected));
    for (size_t i = sizeof(expected); i < size; i++) {
        zassert_equal(map[i], 0);
    }
    const uint8_t *mask = iqs9151_test_config_block(0x1246, &size);
    zassert_equal(size, 88);
    for (size_t i = 0; i < size; i++) {
        zassert_equal(mask[i], 0);
    }
#else
    zassert_equal(settings[0x11E3 - 0x1178], 12);
    zassert_equal(settings[0x11E4 - 0x1178], 13);
    zassert_equal(settings[0x11CB - 0x1178], 0x0F);
#endif
}
ZTEST(iqs9151_profile, test_reset_reloads_configuration_before_event_mode) {
    emulate_sensor = true;
    zassert_ok(iqs9151_test_restore(&bus, &irq));
    zassert_equal(ati_requests, 1);
    zassert_true(sys_get_le16(registers + 0x1BE) & BIT(8));
    zassert_equal(registers[0x1CC], 37);
    size_t size;
    const uint8_t *map = iqs9151_test_config_block(0x1218, &size);
    zassert_mem_equal(registers + 0x218, map, size);
    const uint8_t *mask = iqs9151_test_config_block(0x1246, &size);
    zassert_mem_equal(registers + 0x246, mask, size);
    zassert_equal(sys_get_le16(registers + 0x1E6), CONFIG_INPUT_IQS9151_RESOLUTION_X);
    zassert_equal(sys_get_le16(registers + 0x1E8), CONFIG_INPUT_IQS9151_RESOLUTION_Y);
}
ZTEST(iqs9151_profile, test_fine_divider_preserves_other_fields_and_survives_reset) {
    emulate_sensor = true;
    for (int reset = 0; reset < 2; reset++) {
        memset(registers, 0, sizeof(registers));
        zassert_ok(iqs9151_test_restore(&bus, &irq));
        /* Original 0x4B21 becomes 0x5121: only bits 13:9 change. */
        zassert_equal(sys_get_le16(registers + 0x17A), 0x5121);
        zassert_equal(sys_get_le16(registers + 0x196), 400);
    }
}
ZTEST(iqs9151_profile, test_fine_divider_readback_mismatch_stops_before_ati) {
    emulate_sensor = true;
    drop_fine_write = true;
    zassert_equal(iqs9151_test_restore(&bus, &irq), -EIO);
    zassert_equal(ati_requests, 0);
}
ZTEST(iqs9151_profile, test_pending_alp_does_not_block_trackpad_startup) {
    emulate_sensor = true;
    defer_alp_ati = true;
    zassert_ok(iqs9151_test_restore(&bus, &irq));
    zassert_true(sys_get_le16(registers + 0x1BC) & BIT(6));
    zassert_false(sys_get_le16(registers + 0x1BC) & BIT(5));
    zassert_true(sys_get_le16(registers + 0x1BE) & BIT(8));
}
ZTEST(iqs9151_profile, test_pending_alp_does_not_hide_trackpad_ati_error) {
    emulate_sensor = true;
    defer_alp_ati = true;
    registers[0x20] = BIT(3);
    zassert_equal(iqs9151_test_restore(&bus, &irq), -EIO);
    zassert_false(sys_get_le16(registers + 0x1BE) & BIT(8));
}
ZTEST(iqs9151_profile, test_reset_restore_stops_on_bus_failure) {
    emulate_sensor = true;
    transfer_result = -EIO;
    zassert_equal(iqs9151_test_restore(&bus, &irq), -EIO);
    zassert_equal(transfers, 1);
    zassert_equal(ati_requests, 0);
}
ZTEST(iqs9151_profile, test_ati_error_prevents_successful_restore) {
    emulate_sensor = true;
    registers[0x20] = BIT(3);
    zassert_equal(iqs9151_test_restore(&bus, &irq), -EIO);
    zassert_equal(ati_requests, 1);
    zassert_false(sys_get_le16(registers + 0x1BE) & BIT(8));
}
ZTEST(iqs9151_profile, test_host_reboot_resets_silent_sensor_without_reset_wire) {
    emulate_sensor = true;
    gpio_level = BIT(0); /* event mode with no touch: RDY HIGH indefinitely */
    zassert_ok(iqs9151_test_boot_reset(&bus, &irq));
    zassert_equal(sys_get_le16(registers + 0x1BC), BIT(9));
    zassert_true(registers[0x20] & BIT(7));
}
ZTEST_SUITE(iqs9151_profile, NULL, NULL, before, NULL, NULL);
