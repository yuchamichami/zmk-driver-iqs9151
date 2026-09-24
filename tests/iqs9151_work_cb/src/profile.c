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
static bool drop_fine_write, drop_frequency_write;
static uint8_t registers[0x400];
static unsigned int ati_requests;
#if defined(CONFIG_INPUT_IQS9151_CALIBRATION_SURVEY) || defined(CONFIG_INPUT_IQS9151_VALIDATED_CALIBRATION)
static bool survey_mock;
static uint16_t survey_base_max;
static int survey_fail_read;
static bool survey_reset, survey_candidate_error, survey_bad_count, survey_stale_ref;
static bool survey_zero_comp;
static unsigned int base_requests, candidate_requests, cell_reads, reseed_requests;
static uint16_t requested_targets[32], requested_fine[32];
static uint8_t requested_frequency[32][3];
#endif



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
#if defined(CONFIG_INPUT_IQS9151_ATI_DIAGNOSTICS) || defined(CONFIG_INPUT_IQS9151_VALIDATED_CALIBRATION)
        if (reg >= 0xA000) {
            zassert_equal(count, 2);
            zassert_true(msgs[1].flags & I2C_MSG_READ);
            uint16_t target = sys_get_le16(registers + 0x196);
            uint16_t fine = (sys_get_le16(registers + 0x17A) >> 9) & 31;
            for (size_t i = 0; i < msgs[1].len; i += 2) {
                uint16_t value = 0;
                if ((reg & 0xF000) == 0xD000) {
                    value = target ? (780 | (16 << 10)) : (31 << 10);
#if defined(CONFIG_INPUT_IQS9151_CALIBRATION_SURVEY) || defined(CONFIG_INPUT_IQS9151_VALIDATED_CALIBRATION)
                    if (survey_mock && survey_zero_comp) value &= ~0x3ff;
#endif
                } else {
                    value = target ? target : 100;
#if defined(CONFIG_INPUT_IQS9151_CALIBRATION_SURVEY) || defined(CONFIG_INPUT_IQS9151_VALIDATED_CALIBRATION)
                    if (survey_mock && survey_bad_count && target && (reg & 0xF000) == 0xA000) value = target + 51;
                    if (survey_mock && survey_stale_ref && !target && (reg & 0xF000) == 0xB000) value = 65535;
                    if (survey_mock && !target && ((reg & 0xFFF) + i) == 154 && (reg & 0xF000) == 0xA000) {
                        value = survey_base_max * fine / 6;
                    }
#endif
                }
                sys_put_le16(value, msgs[1].buf + i);
            }
#if defined(CONFIG_INPUT_IQS9151_CALIBRATION_SURVEY) || defined(CONFIG_INPUT_IQS9151_VALIDATED_CALIBRATION)
            if (survey_mock) {
                cell_reads++;
                if (survey_fail_read) return survey_fail_read;
            }
#endif
            return 0;
        }
#endif
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
            if (!(drop_fine_write && reg == IQS9151_ADDR_ATI_MULTIPLIERS) &&
                !(drop_frequency_write && reg == 0x11D8)) {
                memcpy(registers + offset, msgs[0].buf + 2, msgs[0].len - 2);
            }
            if (reg == 0x11BC && sys_get_le16(registers + offset) == BIT(9)) {
                gpio_level = 0; /* reset resumes streaming */
                registers[0x20] = BIT(7);
            }
#if defined(CONFIG_INPUT_IQS9151_CALIBRATION_SURVEY) || defined(CONFIG_INPUT_IQS9151_VALIDATED_CALIBRATION)
            if ((survey_mock || IS_ENABLED(CONFIG_INPUT_IQS9151_VALIDATED_CALIBRATION)) && reg == 0x11BC && (registers[offset] & BIT(3))) {
                registers[offset] &= ~BIT(3);
                reseed_requests++;
            }
            if ((survey_mock || IS_ENABLED(CONFIG_INPUT_IQS9151_VALIDATED_CALIBRATION)) && reg == 0x11BC && (registers[offset] & BIT(5))) {
                uint16_t target = sys_get_le16(registers + 0x196);
                zassert_true(ati_requests < ARRAY_SIZE(requested_targets));
                memcpy(requested_frequency[ati_requests], registers + 0x1D8, 3);
                requested_targets[ati_requests] = target;
                requested_fine[ati_requests] = (sys_get_le16(registers + 0x17A) >> 9) & 31;
                ati_requests++;
                if (target == 0) base_requests++; else candidate_requests++;
                registers[offset] &= ~BIT(5);
                if (survey_mock) registers[0x20] = survey_reset ? BIT(7) : ((target == 0 || survey_candidate_error) ? BIT(3) : 0);
                return 0;
            }
#endif
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
    drop_fine_write = drop_frequency_write = false;
    ati_requests = 0;
    memset(registers, 0, sizeof(registers));
#if defined(CONFIG_INPUT_IQS9151_CALIBRATION_SURVEY) || defined(CONFIG_INPUT_IQS9151_VALIDATED_CALIBRATION)
    survey_mock = false;
    survey_base_max = 700;
    survey_fail_read = 0;
    survey_reset = survey_candidate_error = survey_bad_count = survey_stale_ref = survey_zero_comp = false;
    base_requests = candidate_requests = cell_reads = reseed_requests = 0;
    memset(requested_targets, 0, sizeof(requested_targets));
    memset(requested_fine, 0, sizeof(requested_fine));
#endif
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

#ifdef CONFIG_INPUT_IQS9151_CALIBRATION_SURVEY
static void prepare_survey(void) {
    emulate_sensor = survey_mock = true;
    sys_put_le16(0x060E, registers + 0x1BE);
    sys_put_le16(0x4B21, registers + 0x17A);
    registers[0x1A0] = 50;
}
ZTEST(iqs9151_profile, test_survey_measures_expected_base_error_before_candidates) {
    prepare_survey();
    zassert_ok(iqs9151_test_survey(&bus, &irq));
#ifdef CONFIG_INPUT_IQS9151_CALIBRATION_FREQUENCY_SURVEY
    zassert_equal(base_requests, 3);
    zassert_equal(candidate_requests, 12);
    zassert_equal(reseed_requests, 12);
    for (int i = 0; i < 15; i++) {
        const uint16_t targets[] = {0, 800, 825, 850, 875};
        zassert_equal(requested_targets[i], targets[i % 5]);
        zassert_equal(requested_fine[i], 6);
        const uint8_t frequencies[][3] = {{0x28, 2, 2}, {0x18, 4, 4}, {0x10, 7, 7}};
        zassert_mem_equal(requested_frequency[i], frequencies[i / 5], 3);
    }
    const uint8_t expected_frequency[] = {0x10, 7, 7};
    zassert_mem_equal(registers + 0x1D8, expected_frequency, 3);
#else
    zassert_equal(base_requests, 4);
    zassert_equal(candidate_requests, 3);
    zassert_equal(reseed_requests, 3);
    const uint16_t fine[] = {20, 12, 8, 6, 6, 6, 6};
    const uint16_t target[] = {0, 0, 0, 0, 800, 900, 1000};
    zassert_mem_equal(requested_fine, fine, sizeof(fine));
    zassert_mem_equal(requested_targets, target, sizeof(target));
#endif
    zassert_true(cell_reads >= 7 * 12 * 3 * 2);
    zassert_false(sys_get_le16(registers + 0x1BE) & BIT(8));
    zassert_true(sys_get_le16(registers + 0x1BE) & BIT(7));
}
ZTEST(iqs9151_profile, test_survey_skips_targets_below_measured_floor_margin) {
    prepare_survey();
    survey_base_max = 850;
    zassert_ok(iqs9151_test_survey(&bus, &irq));
#ifdef CONFIG_INPUT_IQS9151_CALIBRATION_FREQUENCY_SURVEY
    zassert_equal(base_requests, 3);
    zassert_equal(candidate_requests, 0);
#else
    zassert_equal(base_requests, 4);
    zassert_equal(candidate_requests, 1);
    zassert_equal(requested_targets[4], 1000);
#endif
}
ZTEST(iqs9151_profile, test_survey_bus_failure_does_not_try_more_settings) {
    prepare_survey();
    survey_fail_read = -EIO;
    zassert_equal(iqs9151_test_survey(&bus, &irq), -EIO);
    zassert_equal(base_requests, 1);
    zassert_equal(candidate_requests, 0);
}
ZTEST(iqs9151_profile, test_survey_never_qualifies_base_or_failed_calibration) {
    prepare_survey();
    uint16_t base = 0;
    bool passes = true;
    zassert_ok(iqs9151_test_survey_phase(&bus, &irq, 6, 0, &base, &passes));
    zassert_false(passes);
    survey_candidate_error = true;
    zassert_ok(iqs9151_test_survey_phase(&bus, &irq, 6, 800, &base, &passes));
    zassert_false(passes);
    survey_candidate_error = false;
    survey_bad_count = true;
    zassert_ok(iqs9151_test_survey_phase(&bus, &irq, 6, 800, &base, &passes));
    zassert_false(passes);
    survey_bad_count = false;
    survey_zero_comp = true;
    zassert_ok(iqs9151_test_survey_phase(&bus, &irq, 6, 800, &base, &passes));
    zassert_false(passes);
    survey_zero_comp = false;
    zassert_ok(iqs9151_test_survey_phase(&bus, &irq, 6, 800, &base, &passes));
    zassert_true(passes);
}
ZTEST(iqs9151_profile, test_survey_base_gate_uses_counts_not_stale_reference) {
    prepare_survey();
    survey_stale_ref = true;
    zassert_ok(iqs9151_test_survey(&bus, &irq));
#ifdef CONFIG_INPUT_IQS9151_CALIBRATION_FREQUENCY_SURVEY
    zassert_equal(base_requests, 3);
    zassert_equal(candidate_requests, 12);
#else
    zassert_equal(base_requests, 4);
    zassert_equal(candidate_requests, 3);
#endif
}
ZTEST(iqs9151_profile, test_survey_reset_invalidates_measurement) {
    prepare_survey();
    survey_reset = true;
    zassert_equal(iqs9151_test_survey(&bus, &irq), -EIO);
    zassert_equal(base_requests, 1);
    zassert_equal(candidate_requests, 0);
}
#endif

#ifdef CONFIG_INPUT_IQS9151_CALIBRATION_FREQUENCY_SURVEY
ZTEST(iqs9151_profile, test_frequency_readback_mismatch_stops_before_measurement) {
    prepare_survey();
    drop_frequency_write = true;
    zassert_equal(iqs9151_test_survey(&bus, &irq), -EIO);
    zassert_equal(ati_requests, 0);
}
#endif

#ifdef CONFIG_INPUT_IQS9151_VALIDATED_CALIBRATION
static void prepare_validated(void) {
    emulate_sensor = survey_mock = true;
    sys_put_le16(0x060E, registers + 0x1BE);
    sys_put_le16(0x4B21, registers + 0x17A);
    registers[0x1A0] = 50;
}
ZTEST(iqs9151_profile, test_validated_calibration_restores_automatic_modes_after_pass) {
    prepare_validated();
    zassert_ok(iqs9151_test_calibrate(&bus, &irq));
    zassert_equal(ati_requests, 1);
    zassert_equal(reseed_requests, 1);
    zassert_equal(cell_reads, 72);
    zassert_equal(sys_get_le16(registers + 0x1BE), 0x060E);
    zassert_true(sys_get_le16(registers + 0x1BC) & BIT(6));
    zassert_equal(sys_get_le16(registers + 0x196), CONFIG_INPUT_IQS9151_ATI_TARGETCOUNT);
}
ZTEST(iqs9151_profile, test_validated_bad_counts_do_not_enable_events) {
    prepare_validated();
    survey_bad_count = true;
    zassert_equal(iqs9151_test_calibrate(&bus, &irq), -EIO);
    zassert_false(sys_get_le16(registers + 0x1BE) & BIT(8));
}
ZTEST(iqs9151_profile, test_validated_ati_error_does_not_enable_events) {
    prepare_validated();
    survey_candidate_error = true;
    zassert_equal(iqs9151_test_calibrate(&bus, &irq), -EIO);
    zassert_false(sys_get_le16(registers + 0x1BE) & BIT(8));
}
ZTEST(iqs9151_profile, test_validated_cell_read_failure_aborts) {
    prepare_validated();
    survey_fail_read = -EIO;
    zassert_equal(iqs9151_test_calibrate(&bus, &irq), -EIO);
    zassert_equal(ati_requests, 1);
}
#endif
