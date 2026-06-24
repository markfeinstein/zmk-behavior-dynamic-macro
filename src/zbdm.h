/*
 * Copyright (c) 2026 The ZMK Behavior Dynamic Macro Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/zbdm.h>

#if defined(CONFIG_ZMK_ZBDM_TEST_SETTINGS)
#include <stddef.h>
#include <zephyr/settings/settings.h>

typedef int (*zbdm_test_save_cb_t)(const char *name, const void *value, size_t len);

void zbdm_test_set_save_cb(zbdm_test_save_cb_t cb);
void zbdm_test_reset_slots(void);
int zbdm_test_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg);
#endif
