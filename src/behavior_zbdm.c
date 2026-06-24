/*
 * Copyright (c) 2026 The ZMK Behavior Dynamic Macro Contributors
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_zbdm

#include <drivers/behavior.h>
#include <dt-bindings/zmk/zbdm.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>

#include "zbdm.h"

#define ZBDM_RUNTIME_ENABLED                                                                       \
    (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if ZBDM_RUNTIME_ENABLED
static const char *command_name(uint32_t command) {
    switch (command) {
    case ZBDM_RECORD:
        return "ZBDM_RECORD";
    case ZBDM_STOP:
        return "ZBDM_STOP";
    case ZBDM_TOGGLE:
        return "ZBDM_TOGGLE";
    case ZBDM_PLAY:
        return "ZBDM_PLAY";
    case ZBDM_CLEAR:
        return "ZBDM_CLEAR";
    case ZBDM_CANCEL:
        return "ZBDM_CANCEL";
    default:
        return "unknown";
    }
}
#endif

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata slot_command_values[] = {
    {.display_name = "Record", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = ZBDM_RECORD},
    {.display_name = "Toggle", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = ZBDM_TOGGLE},
    {.display_name = "Play", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = ZBDM_PLAY},
    {.display_name = "Clear", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = ZBDM_CLEAR},
};

static const struct behavior_parameter_value_metadata no_arg_command_values[] = {
    {.display_name = "Stop", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = ZBDM_STOP},
    {.display_name = "Cancel", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = ZBDM_CANCEL},
};

static const struct behavior_parameter_value_metadata slot_values[] = {{
    .display_name = "Slot",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
    .range = {.min = 0, .max = CONFIG_ZMK_ZBDM_SLOTS - 1},
}};

static const struct behavior_parameter_value_metadata zero_values[] = {{
    .display_name = "Unused",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
    .value = 0,
}};

static const struct behavior_parameter_metadata_set metadata_sets[] = {
    {
        .param1_values = slot_command_values,
        .param1_values_len = ARRAY_SIZE(slot_command_values),
        .param2_values = slot_values,
        .param2_values_len = ARRAY_SIZE(slot_values),
    },
    {
        .param1_values = no_arg_command_values,
        .param1_values_len = ARRAY_SIZE(no_arg_command_values),
        .param2_values = zero_values,
        .param2_values_len = ARRAY_SIZE(zero_values),
    },
};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(metadata_sets),
    .sets = metadata_sets,
};

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static int on_zbdm_pressed(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

#if ZBDM_RUNTIME_ENABLED
    int ret;
    switch (binding->param1) {
    case ZBDM_RECORD:
        ret = zmk_zbdm_record(binding->param2);
        break;
    case ZBDM_STOP:
        if (binding->param2 != 0) {
            LOG_ERR("ZBDM_STOP requires slot parameter 0; use &zbdm ZBDM_STOP 0");
            return ZMK_BEHAVIOR_OPAQUE;
        }
        ret = zmk_zbdm_stop();
        break;
    case ZBDM_TOGGLE:
        ret = zmk_zbdm_toggle(binding->param2);
        break;
    case ZBDM_PLAY:
        ret = zmk_zbdm_play(binding->param2);
        break;
    case ZBDM_CLEAR:
        ret = zmk_zbdm_clear(binding->param2);
        break;
    case ZBDM_CANCEL:
        if (binding->param2 != 0) {
            LOG_ERR("ZBDM_CANCEL requires slot parameter 0; use &zbdm ZBDM_CANCEL 0");
            return ZMK_BEHAVIOR_OPAQUE;
        }
        ret = zmk_zbdm_cancel();
        break;
    default:
        LOG_ERR("Unknown dynamic macro command: %u", binding->param1);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (ret < 0) {
        LOG_WRN("Dynamic macro command %s (slot %u) failed: %d", command_name(binding->param1),
                binding->param2, ret);
    }
#else
    LOG_WRN("Ignoring dynamic macro command %u on split peripheral", binding->param1);
#endif

    // The key press was consumed by this behavior regardless of the internal
    // result. Always report it as handled (ZMK_BEHAVIOR_OPAQUE) so ZMK does not
    // treat a negative errno as a behavior error and abort position-state
    // processing for this key.
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_zbdm_released(struct zmk_behavior_binding *binding,
                            struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_zbdm_driver_api = {
    .binding_pressed = on_zbdm_pressed,
    .binding_released = on_zbdm_released,
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

#define ZBDM_INST(n)                                                                               \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_zbdm_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ZBDM_INST)

#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
