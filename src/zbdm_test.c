/*
 * Copyright (c) 2026 The ZMK Behavior Dynamic Macro Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

#include "zbdm.h"

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            printk("zbdm settings self-test failed at line %d: %s\n", __LINE__, #expr);            \
            k_panic();                                                                             \
        }                                                                                          \
    } while (false)

struct test_event {
    uint16_t usage_page;
    uint16_t keycode;
    uint8_t implicit_modifiers;
    uint8_t explicit_modifiers;
    uint8_t state;
    uint8_t reserved;
};

struct test_slot_one {
    uint8_t version;
    uint8_t reserved;
    uint16_t len;
    struct test_event events[1];
};

BUILD_ASSERT(sizeof(struct test_event) == 8);
BUILD_ASSERT(sizeof(struct test_slot_one) == 12);

struct read_fixture {
    const void *data;
    ssize_t result;
};

static ssize_t fixture_read(void *cb_arg, void *data, size_t len) {
    struct read_fixture *fixture = cb_arg;
    if (fixture->result < 0) {
        return fixture->result;
    }

    size_t copied = MIN(len, (size_t)fixture->result);
    memcpy(data, fixture->data, copied);
    return fixture->result;
}

static int failing_save(const char *name, const void *value, size_t len) {
    ARG_UNUSED(name);
    ARG_UNUSED(value);
    ARG_UNUSED(len);
    return -EIO;
}

static int successful_save(const char *name, const void *value, size_t len) {
    ARG_UNUSED(name);
    ARG_UNUSED(value);
    ARG_UNUSED(len);
    return 0;
}

static void expect_rejected(const char *name, size_t len, struct read_fixture *fixture,
                            int expected) {
    zbdm_test_reset_slots();
    CHECK(zbdm_test_settings_set(name, len, fixture_read, fixture) == expected);

    uint16_t slot_len = UINT16_MAX;
    CHECK(zmk_zbdm_get_slot_length(0, &slot_len) == 0);
    CHECK(slot_len == 0);
}

static void test_settings_parser(void) {
    struct test_slot_one valid = {
        .version = 1,
        .len = 1,
        .events = {{.usage_page = HID_USAGE_KEY, .keycode = 0x04, .state = 1}},
    };
    struct read_fixture full = {.data = &valid, .result = sizeof(valid)};

    expect_rejected("other_0", sizeof(valid), &full, -ENOENT);
    expect_rejected("slot_", sizeof(valid), &full, -EINVAL);
    expect_rejected("slot_+0", sizeof(valid), &full, -EINVAL);
    expect_rejected("slot_00", sizeof(valid), &full, -EINVAL);
    expect_rejected("slot_0x", sizeof(valid), &full, -EINVAL);
    expect_rejected("slot_99", sizeof(valid), &full, -EINVAL);
    expect_rejected("slot_0", 3, &full, -EINVAL);
    expect_rejected("slot_0",
                    sizeof(valid) + CONFIG_ZMK_ZBDM_MAX_EVENTS * sizeof(struct test_event), &full,
                    -EINVAL);

    struct read_fixture failed = {.data = &valid, .result = -EIO};
    expect_rejected("slot_0", sizeof(valid), &failed, -EIO);

    struct read_fixture short_read = {.data = &valid, .result = sizeof(valid) - 1};
    expect_rejected("slot_0", sizeof(valid), &short_read, -EINVAL);

    valid.version = 2;
    expect_rejected("slot_0", sizeof(valid), &full, -EINVAL);
    valid.version = 1;

    valid.events[0].usage_page = 0xffff;
    expect_rejected("slot_0", sizeof(valid), &full, -EINVAL);
    valid.events[0].usage_page = HID_USAGE_KEY;

    valid.events[0].state = 2;
    expect_rejected("slot_0", sizeof(valid), &full, -EINVAL);
    valid.events[0].state = 1;

    zbdm_test_reset_slots();
    CHECK(zbdm_test_settings_set("slot_0", sizeof(valid), fixture_read, &full) == 0);
    uint16_t slot_len = 0;
    CHECK(zmk_zbdm_get_slot_length(0, &slot_len) == 0);
    CHECK(slot_len == 1);
}

static void test_failed_save_retry(void) {
    zbdm_test_reset_slots();
    zbdm_test_set_save_cb(failing_save);

    CHECK(zmk_zbdm_record(0) == 0);
    raise_zmk_keycode_state_changed((struct zmk_keycode_state_changed){
        .usage_page = HID_USAGE_KEY, .keycode = 0x04, .state = true, .timestamp = k_uptime_get()});
    raise_zmk_keycode_state_changed((struct zmk_keycode_state_changed){
        .usage_page = HID_USAGE_KEY, .keycode = 0x04, .state = false, .timestamp = k_uptime_get()});
    CHECK(zmk_zbdm_stop() == -EIO);

    uint16_t slot_len = 0;
    CHECK(zmk_zbdm_get_slot_length(0, &slot_len) == 0);
    CHECK(slot_len == 2);
    CHECK(zmk_zbdm_record(0) == -EAGAIN);

    zbdm_test_set_save_cb(successful_save);
    CHECK(zmk_zbdm_stop() == 0);
    CHECK(zmk_zbdm_record(0) == 0);
    CHECK(zmk_zbdm_cancel() == 0);
    zbdm_test_set_save_cb(NULL);
}

static int zbdm_settings_self_test(void) {
    test_settings_parser();
    test_failed_save_retry();
    printk("zbdm_settings_self_test: pass\n");
    return 0;
}

SYS_INIT(zbdm_settings_self_test, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
