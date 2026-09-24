/* SPDX-License-Identifier: MIT
 * Diagnostic-only inclusion after the normal register helpers.
 * Azoteq IQS9150/IQS9151 User Guide v1.0 (Apr 2025), section 4.2:
 * manual Active mode, coarse set 0, target zero to measure base counts.
 * Fine divider 6 is that guide's lower bound. No result enables input.
 */
enum {
    IQS9151_SURVEY_RX = 13,
    IQS9151_SURVEY_TX = 12,
    IQS9151_SURVEY_DRIFT = 50,
};
BUILD_ASSERT(TRACKPAD_SETTINGS_0_1 == IQS9151_SURVEY_RX &&
             TRACKPAD_SETTINGS_1_0 == IQS9151_SURVEY_TX);
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1,
             "The calibration survey supports one sensor per firmware");

struct iqs9151_survey_sample {
    uint16_t min_count, max_count, min_ref, max_ref;
    uint16_t min_comp, max_comp, min_div, max_div;
    unsigned int comp_zero, comp_saturated, outside_range;
};

#if defined(CONFIG_INPUT_IQS9151_CALIBRATION_SURVEY)
static const struct device *iqs9151_survey_device;
#endif

static int iqs9151_survey_info(const struct iqs9151_config *cfg, uint16_t *info) {
    int ret = iqs9151_read_u16(cfg, IQS9151_ADDR_INFO_FLAGS, info);
    if (ret == 0 && (*info & IQS9151_INFO_SHOW_RESET)) {
        LOG_ERR("SURVEY aborted: sensor reset during measurement info=0x%04x", *info);
        return -EIO;
    }
    return ret;
}

static int iqs9151_survey_write_verify(const struct iqs9151_config *cfg,
                                      uint16_t reg, uint16_t value) {
    int ret = iqs9151_write_u16(cfg, reg, value);
    uint16_t actual;
    if (ret != 0) return ret;
    ret = iqs9151_read_u16(cfg, reg, &actual);
    if (ret != 0) return ret;
    if (actual != value) {
        LOG_ERR("SURVEY readback mismatch reg=0x%04x expected=0x%04x actual=0x%04x",
                reg, value, actual);
        return -EIO;
    }
    return 0;
}

static int iqs9151_survey_wait_command(const struct iqs9151_config *cfg,
                                     uint16_t command, uint16_t *info) {
    const int64_t deadline = k_uptime_get() + 2000;
    while (k_uptime_get() < deadline) {
        uint16_t control;
        int ret = iqs9151_read_u16(cfg, IQS9151_ADDR_SYSTEM_CONTROL, &control);
        if (ret == -ETIMEDOUT) continue; /* ATI can temporarily suppress RDY. */
        if (ret != 0) return ret;
        ret = iqs9151_survey_info(cfg, info);
        if (ret == -ETIMEDOUT) continue;
        if (ret != 0) return ret;
        if ((control & command) == 0) {
            /* TP ATI error is expected for target zero; caller classifies
             * nonzero targets strictly. ALP is not calibrated in Active mode. */
            return 0;
        }
        k_sleep(K_MSEC(IQS9151_ATI_POLL_INTERVAL_MS));
    }
    LOG_ERR("SURVEY command 0x%04x did not finish within 2000 ms", command);
    return -ETIMEDOUT;
}

static int iqs9151_survey_collect(const struct iqs9151_config *cfg,
                                uint16_t target, struct iqs9151_survey_sample *s) {
    uint8_t counts[IQS9151_SURVEY_RX * 2], refs[IQS9151_SURVEY_RX * 2];
    uint8_t comps[IQS9151_SURVEY_RX * 2];
    *s = (struct iqs9151_survey_sample){
        .min_count = UINT16_MAX, .min_ref = UINT16_MAX,
        .min_comp = UINT16_MAX, .min_div = UINT16_MAX,
    };
    for (unsigned int tx = 0; tx < IQS9151_SURVEY_TX; ++tx) {
        const uint16_t offset = tx * IQS9151_SURVEY_RX * 2;
        int ret = iqs9151_i2c_read(cfg, 0xD000 + offset, comps, sizeof(comps));
        if (ret == 0) ret = iqs9151_i2c_read(cfg, 0xA000 + offset, counts, sizeof(counts));
        if (ret == 0) ret = iqs9151_i2c_read(cfg, 0xB000 + offset, refs, sizeof(refs));
        if (ret != 0) return ret;
        for (unsigned int rx = 0; rx < IQS9151_SURVEY_RX; ++rx) {
            const uint16_t raw = sys_get_le16(comps + 2 * rx);
            const uint16_t comp = raw & 0x3ff;
            const uint16_t divider = (raw >> 10) & 0x1f;
            const uint16_t count = sys_get_le16(counts + 2 * rx);
            const uint16_t ref = sys_get_le16(refs + 2 * rx);
            s->min_count = MIN(s->min_count, count);
            s->max_count = MAX(s->max_count, count);
            s->min_ref = MIN(s->min_ref, ref);
            s->max_ref = MAX(s->max_ref, ref);
            s->min_comp = MIN(s->min_comp, comp);
            s->max_comp = MAX(s->max_comp, comp);
            s->min_div = MIN(s->min_div, divider);
            s->max_div = MAX(s->max_div, divider);
            s->comp_zero += comp == 0;
            s->comp_saturated += comp == 1023;
            const bool outside_range = target != 0 &&
                ((int32_t)count < (int32_t)target - IQS9151_SURVEY_DRIFT ||
                 (uint32_t)count > target + IQS9151_SURVEY_DRIFT ||
                 (int32_t)ref < (int32_t)target - IQS9151_SURVEY_DRIFT ||
                 (uint32_t)ref > target + IQS9151_SURVEY_DRIFT);
            if (outside_range) {
                s->outside_range++;
            }
            if (IS_ENABLED(CONFIG_INPUT_IQS9151_CALIBRATION_SURVEY) ||
                comp == 0 || comp == 1023 || outside_range) {
                LOG_INF("ATI cell tx=%u rx=%u comp=%u div=%u count=%u ref=%u",
                        tx, rx, comp, divider, count, ref);
            }
        }
    }
    return 0;
}

static int iqs9151_survey_phase(const struct iqs9151_config *cfg,
                               unsigned int fine, uint16_t target,
                               uint16_t *base_max, bool *passes) {
    /* User Guide table 4.1 set 0: fine multiplier 1, coarse multiplier 9,
     * coarse divider 1. Only the fine divider changes between base phases. */
    const uint16_t multipliers = BIT(14) | (fine << 9) | (9 << 5) | 1;
    uint16_t info;
    int ret = iqs9151_survey_info(cfg, &info);
    *passes = false;
    if (ret != 0) return ret;
    ret = iqs9151_survey_write_verify(cfg, IQS9151_ADDR_ATI_MULTIPLIERS, multipliers);
    if (ret != 0) return ret;
    ret = iqs9151_survey_write_verify(cfg, IQS9151_ADDR_TRACKPAD_ATI_TARGET, target);
    if (ret != 0) return ret;
    LOG_INF("SURVEY phase=%s fine=%u target=%u multipliers=0x%04x",
            target == 0 ? "base" : "candidate", fine, target, multipliers);
    /* Manual Active (mode 0), TP request only: ALP cannot complete here. */
    ret = iqs9151_write_u16(cfg, IQS9151_ADDR_SYSTEM_CONTROL, IQS9151_SYS_CTRL_TP_RE_ATI);
    if (ret != 0) return ret;
    ret = iqs9151_survey_wait_command(cfg, IQS9151_SYS_CTRL_TP_RE_ATI, &info);
    if (ret != 0) return ret;
    bool qualified = target != 0 && !(info & IQS9151_INFO_TP_ATI_ERROR);
    LOG_INF("SURVEY ATI finished fine=%u target=%u info=0x%04x", fine, target, info);
    if (target != 0) {
        /* Manual mode delegates reference management to the host (datasheet
         * 5.5.1/6.3). The official Arduino example also reseeds after ATI.
         * Preserve the ATI verdict above: reseed must not hide an ATI error. */
        ret = iqs9151_write_u16(cfg, IQS9151_ADDR_SYSTEM_CONTROL,
                               IQS9151_SYS_CTRL_TP_RESEED);
        if (ret != 0) return ret;
        ret = iqs9151_survey_wait_command(cfg, IQS9151_SYS_CTRL_TP_RESEED, &info);
        if (ret != 0) return ret;
        if (info & IQS9151_INFO_TP_ATI_ERROR) qualified = false;
    }
    for (unsigned int sample = 0; sample < 2; ++sample) {
        struct iqs9151_survey_sample s;
        if (sample != 0) k_sleep(K_MSEC(200));
        ret = iqs9151_survey_info(cfg, &info);
        if (ret != 0) return ret;
        if (info & IQS9151_INFO_TP_ATI_ERROR) qualified = false;
        LOG_INF("SURVEY sample=%u fine=%u target=%u info=0x%04x sequential snapshots",
                sample, fine, target, info);
        ret = iqs9151_survey_collect(cfg, target, &s);
        if (ret != 0) return ret;
        ret = iqs9151_survey_info(cfg, &info);
        if (ret != 0) return ret;
        if (info & IQS9151_INFO_TP_ATI_ERROR) qualified = false;
        LOG_INF("SURVEY stats sample=%u fine=%u target=%u cells=156 info=0x%04x "
                "count=%u..%u ref=%u..%u comp=%u..%u div=%u..%u "
                "comp_zero=%u comp_max=%u outside_range=%u",
                sample, fine, target, info, s.min_count, s.max_count,
                s.min_ref, s.max_ref, s.min_comp, s.max_comp, s.min_div, s.max_div,
                s.comp_zero, s.comp_saturated, s.outside_range);
        if (target == 0) {
            if (s.comp_zero != IQS9151_SURVEY_RX * IQS9151_SURVEY_TX) {
                LOG_WRN("SURVEY base note: target zero compensation_zero=%u/156",
                        s.comp_zero);
            }
            /* The manufacturer's target-zero method specifies Counts.
             * Reference can still describe an earlier sensing cycle. */
            *base_max = MAX(*base_max, s.max_count);
        } else if (s.comp_zero != 0 || s.comp_saturated != 0 || s.outside_range != 0) {
            qualified = false;
        }
    }
    *passes = qualified;
    LOG_INF("SURVEY result fine=%u target=%u candidate_pass=%u", fine, target, qualified);
    return 0;
}

static int iqs9151_survey_prepare(const struct iqs9151_config *cfg) {
    uint16_t config, info;
    uint8_t drift;
    int ret = iqs9151_survey_info(cfg, &info);
    if (ret != 0) return ret;
    ret = iqs9151_read_u16(cfg, IQS9151_ADDR_CONFIG_SETTINGS, &config);
    if (ret != 0) return ret;
    config &= ~(IQS9151_CFG_EVENT_MODE | IQS9151_CFG_ALP_RE_ATI_ENABLE |
                IQS9151_CFG_TP_RE_ATI_ENABLE);
    config |= IQS9151_CFG_MANUAL_MODE;
    ret = iqs9151_survey_write_verify(cfg, IQS9151_ADDR_CONFIG_SETTINGS, config);
    if (ret != 0) return ret;
    ret = iqs9151_write_u16(cfg, IQS9151_ADDR_SYSTEM_CONTROL, 0);
    if (ret != 0) return ret;
    uint16_t control;
    ret = iqs9151_read_u16(cfg, IQS9151_ADDR_SYSTEM_CONTROL, &control);
    if (ret != 0) return ret;
    /* Reseed requests can be queued by the chip itself; only require
     * Active mode with no pending ATI, reset or suspend command here. */
    if (control & (GENMASK(2, 0) | IQS9151_SYS_CTRL_TP_RE_ATI |
                   IQS9151_SYS_CTRL_ALP_RE_ATI | IQS9151_SYS_CTRL_SW_RESET | BIT(11))) {
        LOG_ERR("SURVEY could not establish manual Active state: control=0x%04x", control);
        return -EIO;
    }
    ret = iqs9151_i2c_read(cfg, IQS9151_ADDR_TP_REF_DRIFT_LIMIT, &drift, sizeof(drift));
    if (ret != 0) return ret;
    if (drift != IQS9151_SURVEY_DRIFT) {
        LOG_ERR("SURVEY expected drift limit 50, read %u; aborting", drift);
        return -EIO;
    }
    return 0;
}

#if defined(CONFIG_INPUT_IQS9151_CALIBRATION_SURVEY)
#if defined(CONFIG_INPUT_IQS9151_CALIBRATION_FREQUENCY_SURVEY)
static int iqs9151_frequency_survey(const struct device *dev) {
    const struct iqs9151_config *cfg = dev->config;
    /* Datasheet v1.1 A.17: TP Frac, TP Period 1, TP Period 2.
     * Keep oscillator and every other hardware parameter unchanged. */
    static const struct {
        uint32_t hz;
        uint8_t registers[3];
    } frequencies[] = {
        {2500000, {0x28, 2, 2}},
        {1500000, {0x18, 4, 4}},
        {1000000, {0x10, 7, 7}},
    };
    static const uint16_t targets[] = {800, 825, 850, 875};
    unsigned int total_attempted = 0, total_passed = 0, phases = 0;
    const int64_t started = k_uptime_get();
    LOG_WRN("SURVEY frequency comparison begin: maximum 15 phases; no cursor input");
    for (unsigned int f = 0; f < ARRAY_SIZE(frequencies); ++f) {
        uint8_t readback[3];
        uint16_t info;
        int ret = iqs9151_survey_prepare(cfg);
        if (ret != 0) return ret;
        LOG_INF("SURVEY frequency begin hz=%u frac=0x%02x period1=%u period2=%u",
                frequencies[f].hz, frequencies[f].registers[0],
                frequencies[f].registers[1], frequencies[f].registers[2]);
        ret = iqs9151_i2c_write(cfg, IQS9151_ADDR_TP_FREQUENCY,
                                frequencies[f].registers, sizeof(readback));
        if (ret != 0) return ret;
        ret = iqs9151_i2c_read(cfg, IQS9151_ADDR_TP_FREQUENCY,
                               readback, sizeof(readback));
        if (ret != 0) return ret;
        if (memcmp(readback, frequencies[f].registers, sizeof(readback)) != 0) {
            LOG_ERR("SURVEY frequency readback mismatch hz=%u read=0x%02x,%u,%u",
                    frequencies[f].hz, readback[0], readback[1], readback[2]);
            return -EIO;
        }
        ret = iqs9151_survey_info(cfg, &info);
        if (ret != 0) return ret;
        uint16_t base_max = 0;
        bool ignored;
        phases++;
        ret = iqs9151_survey_phase(cfg, 6, 0, &base_max, &ignored);
        if (ret != 0) return ret;
        unsigned int attempted = 0, passed = 0;
        for (unsigned int t = 0; t < ARRAY_SIZE(targets); ++t) {
            const uint16_t target = targets[t];
            if ((uint32_t)base_max + IQS9151_SURVEY_DRIFT >= target) {
                LOG_WRN("SURVEY skip hz=%u fine=6 target=%u base_max=%u "
                        "needs > base_max+50", frequencies[f].hz, target, base_max);
                continue;
            }
            bool candidate_pass;
            phases++;
            attempted++;
            ret = iqs9151_survey_phase(cfg, 6, target, &base_max, &candidate_pass);
            if (ret != 0) return ret;
            passed += candidate_pass;
        }
        total_attempted += attempted;
        total_passed += passed;
        LOG_WRN("SURVEY frequency done hz=%u base_max=%u attempted=%u pass_count=%u",
                frequencies[f].hz, base_max, attempted, passed);
    }
    LOG_WRN("SURVEY frequency comparison done phases=%u attempted=%u pass_count=%u "
            "elapsed_ms=%lld; diagnostics only, input remains disabled",
            phases, total_attempted, total_passed, (long long)(k_uptime_get() - started));
    return 0;
}
#endif

static int iqs9151_calibration_survey(const struct device *dev) {
#if defined(CONFIG_INPUT_IQS9151_CALIBRATION_FREQUENCY_SURVEY)
    return iqs9151_frequency_survey(dev);
#else
    const struct iqs9151_config *cfg = dev->config;
    static const unsigned int fine_values[] = {20, 12, 8, 6};
    static const uint16_t targets[] = {800, 900, 1000};
    int ret = iqs9151_survey_prepare(cfg);
    if (ret != 0) return ret;
    LOG_WRN("SURVEY begin: manual Active, streaming, auto re-ATI off; no cursor input");
    uint16_t fine6_base_max = 0;
    for (unsigned int i = 0; i < ARRAY_SIZE(fine_values); ++i) {
        uint16_t base_max = 0;
        bool ignored;
        ret = iqs9151_survey_phase(cfg, fine_values[i], 0, &base_max, &ignored);
        if (ret != 0) return ret;
        if (fine_values[i] == 6) fine6_base_max = base_max;
    }
    unsigned int attempted = 0, passed = 0;
    for (unsigned int i = 0; i < ARRAY_SIZE(targets); ++i) {
        const uint16_t target = targets[i];
        if ((uint32_t)fine6_base_max + IQS9151_SURVEY_DRIFT >= target) {
            LOG_WRN("SURVEY skip fine=6 target=%u base_max=%u needs > base_max+50",
                    target, fine6_base_max);
            continue;
        }
        bool candidate_pass;
        attempted++;
        ret = iqs9151_survey_phase(cfg, 6, target, &fine6_base_max, &candidate_pass);
        if (ret != 0) return ret;
        passed += candidate_pass;
    }
    LOG_WRN("SURVEY done base_max_fine6=%u attempted=%u pass_count=%u; "
            "diagnostics only, input remains disabled", fine6_base_max, attempted, passed);
    return 0;
#endif
}

static void iqs9151_survey_thread(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    if (iqs9151_survey_device == NULL) {
        LOG_ERR("SURVEY not started: normal sensor initialization failed");
        return;
    }
    int ret = iqs9151_calibration_survey(iqs9151_survey_device);
    if (ret != 0) {
        LOG_ERR("SURVEY stopped (%d); no later candidates attempted, input disabled", ret);
    }
}
K_THREAD_DEFINE(iqs9151_survey_tid, 2048, iqs9151_survey_thread,
                NULL, NULL, NULL, 8, 0, 12000);
#endif /* CONFIG_INPUT_IQS9151_CALIBRATION_SURVEY */
