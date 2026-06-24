/*
 * Copyright (c) 2026 The ZMK Behavior Dynamic Macro Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_ZMK_ZBDM_PERSIST)
#include <zephyr/settings/settings.h>
#endif

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keys.h>

#include "zbdm.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define ZBDM_SETTINGS_KEY "zbdm"
#define ZBDM_SETTINGS_SLOT_PREFIX "slot_"
#define ZBDM_SETTINGS_VERSION 1

#if IS_ENABLED(CONFIG_ZMK_ZBDM_DEBUG)
#define ZBDM_LOG_DBG(...) LOG_DBG(__VA_ARGS__)
#else
#define ZBDM_LOG_DBG(...)
#endif

// NOTE: this struct doubles as the on-flash settings wire format. It is
// intentionally NOT __packed: its natural layout already has no padding (every
// field is naturally aligned), so packing produces the identical byte layout
// but forces the compiler to emit byte-wise field access on cores without
// hardware unaligned support (Cortex-M0/M0+, e.g. RP2040/nRF51), bloating the
// recording and playback hot paths. The BUILD_ASSERTs below lock the layout so
// any accidental padding or field reorder fails the build instead of silently
// breaking persisted macros.
struct zbdm_event {
    uint16_t usage_page;
    uint16_t keycode;
    uint8_t implicit_modifiers;
    uint8_t explicit_modifiers;
    uint8_t state;
    uint8_t reserved;
};

struct zbdm_slot {
    uint8_t version;
    uint8_t reserved;
    uint16_t len;
    struct zbdm_event events[CONFIG_ZMK_ZBDM_MAX_EVENTS];
};

struct zbdm_pressed_usage {
    uint16_t usage_page;
    uint16_t keycode;
    uint8_t implicit_modifiers;
    uint8_t explicit_modifiers;
};

// Lock the unpacked structs to the documented on-flash wire format. A size
// match already forbids any internal padding for these field orders; the offset
// checks additionally guard against accidental field reordering during
// maintenance.
BUILD_ASSERT(sizeof(struct zbdm_event) == 8,
             "dynamic macro event size is the on-flash wire format and RAM sizing");
BUILD_ASSERT(offsetof(struct zbdm_event, keycode) == 2, "event wire layout changed");
BUILD_ASSERT(offsetof(struct zbdm_event, implicit_modifiers) == 4, "event wire layout changed");
BUILD_ASSERT(offsetof(struct zbdm_slot, len) == 2, "slot wire layout changed");
BUILD_ASSERT(offsetof(struct zbdm_slot, events) == 4, "slot wire layout changed");
BUILD_ASSERT(sizeof(struct zbdm_pressed_usage) == 6, "pressed usage tracking should stay compact");
static struct zbdm_slot slots[CONFIG_ZMK_ZBDM_SLOTS];
static bool slots_initialized;
static K_MUTEX_DEFINE(zbdm_lock);
static K_MUTEX_DEFINE(zbdm_op_lock);
static K_MUTEX_DEFINE(zbdm_event_lock);
static K_SEM_DEFINE(zbdm_cleanup_sem, 1, 1);

static bool recording;
// Fast-path hint used to avoid taking zbdm_lock on every key event
// while idle.
static atomic_t recording_active;
static uint8_t recording_slot;
#if IS_ENABLED(CONFIG_ZMK_ZBDM_CANCEL_RESTORE)
// Snapshot of a slot's contents taken before recording overwrites it, used only
// to restore the slot when a recording is canceled. Costs one extra slot-sized
// buffer (~MAX_EVENTS x 8 bytes); compiled out when cancel-restore is disabled.
static struct zbdm_slot recording_backup;
#endif
// Per-slot flag marking a slot whose persistence failed so it can be retried
// instead of being silently lost on reboot. The data remains valid in slots[].
static bool save_pending[CONFIG_ZMK_ZBDM_SLOTS];

static struct k_work_delayable playback_work;
static struct k_work_sync playback_work_sync;
static bool playback_active;
static bool playback_work_running;
static uint8_t playback_slot;
static uint16_t playback_index;
static struct zbdm_pressed_usage pressed_usages[CONFIG_ZMK_ZBDM_MAX_PRESSED_USAGES];
static uint16_t pressed_usage_count;
static atomic_ptr_t cleanup_thread;
// Nonzero while this module synchronously raises replay or cleanup events. A
// listener may start recording reentrantly; those module-generated events must
// not then become part of the new recording.
static atomic_t raised_event_depth;
static atomic_ptr_t raised_event_thread;

static bool in_generated_event(void) {
    return atomic_get(&raised_event_depth) &&
           atomic_ptr_get(&raised_event_thread) == k_current_get();
}

static bool valid_slot(uint32_t slot) { return slot < CONFIG_ZMK_ZBDM_SLOTS; }

static void init_slot(uint32_t slot) {
    slots[slot].version = ZBDM_SETTINGS_VERSION;
    slots[slot].reserved = 0;
    slots[slot].len = 0;
}

static size_t slot_used_len(const struct zbdm_slot *slot) {
    return offsetof(struct zbdm_slot, events) + slot->len * sizeof(struct zbdm_event);
}

static void copy_slot_used(struct zbdm_slot *dst, const struct zbdm_slot *src) {
    memcpy(dst, src, slot_used_len(src));
}

static void ensure_slots_initialized(void) {
    if (slots_initialized) {
        return;
    }

    for (uint8_t i = 0; i < ARRAY_SIZE(slots); i++) {
        init_slot(i);
    }

    slots_initialized = true;
}

static bool is_modifier_event(const struct zmk_keycode_state_changed *ev) {
    return is_mod(ev->usage_page, ev->keycode);
}

#if IS_ENABLED(CONFIG_ZMK_ZBDM_PERSIST)
static size_t slot_settings_len(uint32_t slot) { return slot_used_len(&slots[slot]); }

#if IS_ENABLED(CONFIG_ZMK_ZBDM_TEST_SETTINGS)
static zbdm_test_save_cb_t test_save_cb;

void zbdm_test_set_save_cb(zbdm_test_save_cb_t cb) { test_save_cb = cb; }
#endif

static int save_slot(uint32_t slot) {
    char path[sizeof(ZBDM_SETTINGS_KEY "/slot_00")];

    snprintk(path, sizeof(path), ZBDM_SETTINGS_KEY "/" ZBDM_SETTINGS_SLOT_PREFIX "%u", slot);

#if IS_ENABLED(CONFIG_ZMK_ZBDM_TEST_SETTINGS)
    int ret = test_save_cb != NULL ? test_save_cb(path, &slots[slot], slot_settings_len(slot))
                                   : settings_save_one(path, &slots[slot], slot_settings_len(slot));
#else
    int ret = settings_save_one(path, &slots[slot], slot_settings_len(slot));
#endif
    if (ret < 0) {
        LOG_ERR("Failed to save dynamic macro slot %u: %d", slot, ret);
    }

    return ret;
}
#else
static int save_slot(uint32_t slot) {
    ARG_UNUSED(slot);
    return 0;
}
#endif

// Persist a slot, remembering it for retry if the settings backend fails so the
// recorded data is not silently lost. Must be called with zbdm_lock
// held.
static int save_slot_tracked(uint32_t slot) {
    int rc = save_slot(slot);
    save_pending[slot] = (rc < 0);
    return rc;
}

// Best-effort retry of any previously failed slot persistence, attempted
// opportunistically when other macro operations run. Must be called with
// zbdm_lock held. The active recording slot is skipped because
// slots[recording_slot] holds transient in-progress data, not the intended
// persisted contents; that slot is re-saved when its recording concludes.
static void retry_pending_save_locked(void) {
    for (uint8_t i = 0; i < ARRAY_SIZE(save_pending); i++) {
        if (recording && i == recording_slot) {
            continue;
        }
        if (save_pending[i] && save_slot(i) == 0) {
            save_pending[i] = false;
        }
    }
}

static bool slot_has_pending_save_locked(uint32_t slot) {
    return valid_slot(slot) && save_pending[slot];
}

static bool event_matches_keycode(const struct zbdm_event *event,
                                  const struct zmk_keycode_state_changed *ev) {
    return event->usage_page == ev->usage_page && event->keycode == ev->keycode &&
           event->implicit_modifiers == ev->implicit_modifiers &&
           event->explicit_modifiers == ev->explicit_modifiers;
}

// The recording slot starts empty for every session, so its events are also the
// session-local multiset of presses. Count duplicates independently and only
// accept a release while one matching press remains outstanding.
static bool has_recorded_press(const struct zbdm_slot *slot,
                               const struct zmk_keycode_state_changed *ev) {
    uint16_t pressed = 0;

    for (uint16_t i = 0; i < slot->len; i++) {
        const struct zbdm_event *event = &slot->events[i];
        if (!event_matches_keycode(event, ev)) {
            continue;
        }

        if (event->state) {
            pressed++;
        } else if (pressed > 0) {
            pressed--;
        }
    }

    return pressed > 0;
}

static void track_pressed_usage(const struct zbdm_event *event) {
    if (!event->state) {
        return;
    }

    if (pressed_usage_count >= ARRAY_SIZE(pressed_usages)) {
        LOG_WRN("Dynamic macro pressed usage tracking full; cleanup may be incomplete");
        return;
    }

    pressed_usages[pressed_usage_count++] = (struct zbdm_pressed_usage){
        .usage_page = event->usage_page,
        .keycode = event->keycode,
        .implicit_modifiers = event->implicit_modifiers,
        .explicit_modifiers = event->explicit_modifiers,
    };
}

static void untrack_pressed_usage(const struct zbdm_event *event) {
    if (event->state) {
        return;
    }

    for (uint16_t i = 0; i < pressed_usage_count; i++) {
        if (pressed_usages[i].usage_page == event->usage_page &&
            pressed_usages[i].keycode == event->keycode &&
            pressed_usages[i].implicit_modifiers == event->implicit_modifiers &&
            pressed_usages[i].explicit_modifiers == event->explicit_modifiers) {
            pressed_usage_count--;
            if (i != pressed_usage_count) {
                pressed_usages[i] = pressed_usages[pressed_usage_count];
            }
            return;
        }
    }
}

static void update_pressed_usage_after_raise(const struct zbdm_event *event) {
    if (event->state) {
        // A later listener can return an error after an earlier listener has
        // already mutated HID state for the press. Track presses regardless of
        // the aggregate raise result so cancellation/end-of-playback cleanup can
        // still release them.
        track_pressed_usage(event);
    } else {
        // The event manager's aggregate result cannot tell whether an earlier
        // HID listener already processed this release before a later listener
        // failed. Retrying can therefore send duplicate releases and
        // permanently block future playback. Treat each generated release as
        // at-most-once and untrack it after the synchronous raise.
        untrack_pressed_usage(event);
    }
}

static int raise_zbdm_event(const struct zbdm_event *event) {
    struct zmk_keycode_state_changed ev = {
        .usage_page = event->usage_page,
        .keycode = event->keycode,
        .implicit_modifiers = event->implicit_modifiers,
        .explicit_modifiers = event->explicit_modifiers,
        .state = event->state,
        .timestamp = k_uptime_get(),
    };

    // Generated events may originate from playback or cleanup on different
    // threads. Serialize them so the global thread/depth guard always describes
    // the event currently moving synchronously through ZMK's listener chain.
    k_mutex_lock(&zbdm_event_lock, K_FOREVER);
    if (atomic_inc(&raised_event_depth) == 0) {
        atomic_ptr_set(&raised_event_thread, k_current_get());
    }
    int ret = raise_zmk_keycode_state_changed(ev);
    if (IS_ENABLED(CONFIG_ZMK_ZBDM_TEST_RAISE_ERROR_AFTER_DELIVERY) && ret >= 0) {
        ret = -EIO;
    }
    if (atomic_dec(&raised_event_depth) == 1) {
        atomic_ptr_set(&raised_event_thread, NULL);
    }
    k_mutex_unlock(&zbdm_event_lock);
    if (ret < 0) {
        LOG_WRN("Failed to replay dynamic macro event: %d", ret);
    }

    return ret;
}

static void release_pressed_usages(void) {
    // Cancellation and the playback worker can converge on cleanup. Give the
    // tracked stack one owner across each raise/untrack cycle so entries cannot
    // be released twice or removed by a competing cleanup pass. A listener may
    // request cleanup reentrantly while handling a generated release; that same
    // thread must return instead of waiting on its own semaphore.
    if (atomic_ptr_get(&cleanup_thread) == k_current_get()) {
        return;
    }
    k_sem_take(&zbdm_cleanup_sem, K_FOREVER);
    atomic_ptr_set(&cleanup_thread, k_current_get());

    while (true) {
        k_mutex_lock(&zbdm_lock, K_FOREVER);
        if (pressed_usage_count == 0) {
            k_mutex_unlock(&zbdm_lock);
            break;
        }

        struct zbdm_pressed_usage pressed = pressed_usages[pressed_usage_count - 1];
        k_mutex_unlock(&zbdm_lock);

        struct zbdm_event release_event = {
            .usage_page = pressed.usage_page,
            .keycode = pressed.keycode,
            .implicit_modifiers = pressed.implicit_modifiers,
            .explicit_modifiers = pressed.explicit_modifiers,
            .state = false,
        };

        // The aggregate event result cannot identify whether HID processed the
        // release before a later listener failed. Do not retry an ambiguous
        // release: duplicate releases can corrupt HID reference counts and can
        // leave playback permanently blocked. The warning from
        // raise_zbdm_event() still exposes listener failures.
        raise_zbdm_event(&release_event);

        k_mutex_lock(&zbdm_lock, K_FOREVER);
        if (pressed_usage_count > 0) {
            pressed_usage_count--;
        }
        k_mutex_unlock(&zbdm_lock);
    }

    atomic_ptr_set(&cleanup_thread, NULL);
    k_sem_give(&zbdm_cleanup_sem);
}

static k_timeout_t playback_delay(bool first_event) {
    if (first_event) {
        return K_NO_WAIT;
    }
    return CONFIG_ZMK_ZBDM_PLAYBACK_WAIT_MS > 0 ? K_MSEC(CONFIG_ZMK_ZBDM_PLAYBACK_WAIT_MS)
                                                : K_NO_WAIT;
}

static void finish_playback(void) {
    // Keep playback_active set until leftover keys are released so a concurrent
    // zmk_zbdm_play() cannot start (and reset pressed_usage_count)
    // mid-cleanup, which would strand held keys.
    release_pressed_usages();

    k_mutex_lock(&zbdm_lock, K_FOREVER);
    playback_active = false;
    playback_work_running = false;
    playback_index = 0;
    k_mutex_unlock(&zbdm_lock);
}

// Stop playback and wait until its worker can no longer emit events. Public API
// entry points reject calls made reentrantly by generated events, so this
// cannot wait on the current workqueue thread.
static int stop_playback(bool only_slot, uint32_t slot) {
    k_mutex_lock(&zbdm_lock, K_FOREVER);
    bool stop_active = playback_active && (!only_slot || playback_slot == slot);
    bool cleanup_pending = pressed_usage_count > 0 && (!only_slot || playback_slot == slot);
    if (stop_active) {
        playback_active = false;
        playback_index = 0;
    }
    k_mutex_unlock(&zbdm_lock);

    if (!stop_active && !cleanup_pending) {
        return 0;
    }

    if (stop_active) {
        k_work_cancel_delayable_sync(&playback_work, &playback_work_sync);
    }
    release_pressed_usages();

    return 0;
}

static void playback_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    uint16_t events_processed = 0;

    k_mutex_lock(&zbdm_lock, K_FOREVER);
    playback_work_running = true;
    k_mutex_unlock(&zbdm_lock);

    while (true) {
        k_mutex_lock(&zbdm_lock, K_FOREVER);
        if (!playback_active || !valid_slot(playback_slot)) {
            playback_active = false;
            playback_index = 0;
            k_mutex_unlock(&zbdm_lock);
            release_pressed_usages();
            k_mutex_lock(&zbdm_lock, K_FOREVER);
            playback_work_running = false;
            k_mutex_unlock(&zbdm_lock);
            return;
        }

        struct zbdm_slot *slot = &slots[playback_slot];
        if (playback_index >= slot->len) {
            k_mutex_unlock(&zbdm_lock);
            finish_playback();
            return;
        }

        struct zbdm_event event = slot->events[playback_index++];
        bool last_event = playback_index >= slot->len;

        if (event.state && pressed_usage_count >= ARRAY_SIZE(pressed_usages)) {
            LOG_WRN("Stopping dynamic macro playback: pressed usage tracking full");
            k_mutex_unlock(&zbdm_lock);
            finish_playback();
            return;
        }
        k_mutex_unlock(&zbdm_lock);

        // Raise without holding the lock: the event manager re-enters this
        // module and the HID/BLE pipeline, which must not run under the lock.
        // Cancellation that races this window is caught by the post-raise
        // playback_active re-check below, which tracks and drains whatever this
        // event pressed, so no key is stranded. That lets us skip a third
        // per-event lock acquisition that only re-validated before raising; a
        // cancel landing in this gap now lets one already-dequeued event replay
        // before cleanup releases it, instead of being suppressed.
        raise_zbdm_event(&event);

        k_mutex_lock(&zbdm_lock, K_FOREVER);
        if (!playback_active) {
            update_pressed_usage_after_raise(&event);
            k_mutex_unlock(&zbdm_lock);
            release_pressed_usages();
            k_mutex_lock(&zbdm_lock, K_FOREVER);
            playback_work_running = false;
            k_mutex_unlock(&zbdm_lock);
            return;
        }

        update_pressed_usage_after_raise(&event);
        k_mutex_unlock(&zbdm_lock);

        if (last_event) {
            finish_playback();
            return;
        }

        events_processed++;
        if (CONFIG_ZMK_ZBDM_PLAYBACK_WAIT_MS > 0 ||
            events_processed >= CONFIG_ZMK_ZBDM_PLAYBACK_BATCH_SIZE) {
            int ret = k_work_schedule(&playback_work, playback_delay(false));
            if (ret < 0) {
                finish_playback();
            } else {
                k_mutex_lock(&zbdm_lock, K_FOREVER);
                playback_work_running = false;
                k_mutex_unlock(&zbdm_lock);
            }
            return;
        }
    }
}

static int stop_recording_locked(bool persist) {
    if (!recording) {
        ZBDM_LOG_DBG("No dynamic macro recording to stop");
        return 0;
    }

    uint8_t slot = recording_slot;
    recording = false;
    atomic_set(&recording_active, 0);
    ZBDM_LOG_DBG("Stopped dynamic macro recording slot %u with %u events", slot, slots[slot].len);

    return persist ? save_slot_tracked(slot) : 0;
}

static int stop_recording(bool persist) {
    k_mutex_lock(&zbdm_lock, K_FOREVER);
    retry_pending_save_locked();
    int ret = stop_recording_locked(persist);
    k_mutex_unlock(&zbdm_lock);
    return ret;
}

static int zbdm_record(uint32_t slot) {
    k_mutex_lock(&zbdm_lock, K_FOREVER);
    ensure_slots_initialized();
    retry_pending_save_locked();

    if (!valid_slot(slot)) {
        k_mutex_unlock(&zbdm_lock);
        LOG_ERR("Dynamic macro slot %u is out of range; valid range is 0-%u", slot,
                CONFIG_ZMK_ZBDM_SLOTS - 1);
        return -EINVAL;
    }

    if (slot_has_pending_save_locked(slot) && (!recording || recording_slot != slot)) {
        k_mutex_unlock(&zbdm_lock);
        LOG_WRN("Refusing to overwrite dynamic macro slot %u while save is pending", slot);
        return -EAGAIN;
    }

    bool was_recording = recording;
    uint8_t previous_slot = recording_slot;
    int ret = stop_recording_locked(true);
    if (ret < 0 && (!was_recording || previous_slot == slot)) {
        k_mutex_unlock(&zbdm_lock);
        return ret;
    }

    if (ret < 0) {
        // The previous slot's data is still in RAM and marked save_pending for
        // retry. Do not fail a request to record a different slot just because
        // persistence had a transient error; only abort when recording the same
        // slot would overwrite the unsaved data we are trying to preserve.
        LOG_WRN("Starting dynamic macro slot %u while save for slot %u remains "
                "pending: %d",
                slot, (uint32_t)previous_slot, ret);
    }

    k_mutex_unlock(&zbdm_lock);

    ret = stop_playback(false, 0);
    if (ret < 0) {
        LOG_WRN("Refusing to record dynamic macro slot %u while pressed-key cleanup is pending",
                slot);
        return ret;
    }

    k_mutex_lock(&zbdm_lock, K_FOREVER);

#if IS_ENABLED(CONFIG_ZMK_ZBDM_CANCEL_RESTORE)
    copy_slot_used(&recording_backup, &slots[slot]);
#endif
    init_slot(slot);
    recording_slot = slot;
    recording = true;
    atomic_set(&recording_active, 1);

    ZBDM_LOG_DBG("Started dynamic macro recording slot %u", slot);
    k_mutex_unlock(&zbdm_lock);
    return 0;
}

int zmk_zbdm_record(uint32_t slot) {
    if (in_generated_event()) {
        return -EBUSY;
    }
    k_mutex_lock(&zbdm_op_lock, K_FOREVER);
    int ret = zbdm_record(slot);
    k_mutex_unlock(&zbdm_op_lock);
    return ret;
}

int zmk_zbdm_stop(void) {
    if (in_generated_event()) {
        return -EBUSY;
    }
    k_mutex_lock(&zbdm_op_lock, K_FOREVER);
    int ret = stop_recording(true);
    k_mutex_unlock(&zbdm_op_lock);
    return ret;
}

int zmk_zbdm_toggle(uint32_t slot) {
    if (in_generated_event()) {
        return -EBUSY;
    }
    k_mutex_lock(&zbdm_op_lock, K_FOREVER);
    k_mutex_lock(&zbdm_lock, K_FOREVER);
    ensure_slots_initialized();
    retry_pending_save_locked();

    if (!valid_slot(slot)) {
        k_mutex_unlock(&zbdm_lock);
        k_mutex_unlock(&zbdm_op_lock);
        LOG_ERR("Dynamic macro slot %u is out of range; valid range is 0-%u", slot,
                CONFIG_ZMK_ZBDM_SLOTS - 1);
        return -EINVAL;
    }

    if (recording && recording_slot == slot) {
        int ret = stop_recording_locked(true);
        k_mutex_unlock(&zbdm_lock);
        k_mutex_unlock(&zbdm_op_lock);
        return ret;
    }

    k_mutex_unlock(&zbdm_lock);
    int ret = zbdm_record(slot);
    k_mutex_unlock(&zbdm_op_lock);
    return ret;
}

int zmk_zbdm_play(uint32_t slot) {
    if (in_generated_event()) {
        return -EBUSY;
    }
    k_mutex_lock(&zbdm_op_lock, K_FOREVER);
    k_mutex_lock(&zbdm_lock, K_FOREVER);
    ensure_slots_initialized();
    retry_pending_save_locked();

    if (!valid_slot(slot)) {
        k_mutex_unlock(&zbdm_lock);
        k_mutex_unlock(&zbdm_op_lock);
        LOG_ERR("Dynamic macro slot %u is out of range; valid range is 0-%u", slot,
                CONFIG_ZMK_ZBDM_SLOTS - 1);
        return -EINVAL;
    }

    if (recording) {
        int ret = stop_recording_locked(true);
        k_mutex_unlock(&zbdm_lock);
        k_mutex_unlock(&zbdm_op_lock);
        return ret;
    }

    if (playback_active) {
        k_mutex_unlock(&zbdm_lock);
        k_mutex_unlock(&zbdm_op_lock);
        LOG_WRN("Ignoring play request while dynamic macro slot %u is playing", playback_slot);
        return 0;
    }

    if (playback_work_running) {
        k_mutex_unlock(&zbdm_lock);
        k_mutex_unlock(&zbdm_op_lock);
        LOG_WRN("Refusing dynamic macro playback while previous playback is stopping");
        return -EBUSY;
    }

    if (slots[slot].len == 0) {
        ZBDM_LOG_DBG("Dynamic macro slot %u is empty; playback skipped", slot);
        k_mutex_unlock(&zbdm_lock);
        k_mutex_unlock(&zbdm_op_lock);
        return 0;
    }

    if (pressed_usage_count > 0) {
        // A previous cleanup pass failed to deliver one or more releases. Retry
        // before starting another playback; otherwise resetting the tracking
        // state below would discard the only recovery path for those usages.
        k_mutex_unlock(&zbdm_lock);
        release_pressed_usages();
        k_mutex_lock(&zbdm_lock, K_FOREVER);

        if (pressed_usage_count > 0) {
            k_mutex_unlock(&zbdm_lock);
            k_mutex_unlock(&zbdm_op_lock);
            LOG_WRN("Refusing dynamic macro playback while pressed-key cleanup is "
                    "pending");
            return -EAGAIN;
        }
    }

    playback_slot = slot;
    playback_index = 0;
    pressed_usage_count = 0;
    playback_active = true;

    int ret = k_work_schedule(&playback_work, playback_delay(true));
    if (ret < 0) {
        playback_active = false;
        k_mutex_unlock(&zbdm_lock);
        k_mutex_unlock(&zbdm_op_lock);
        return ret;
    }

    k_mutex_unlock(&zbdm_lock);
    k_mutex_unlock(&zbdm_op_lock);
    return 0;
}

int zmk_zbdm_clear(uint32_t slot) {
    if (in_generated_event()) {
        return -EBUSY;
    }
    k_mutex_lock(&zbdm_op_lock, K_FOREVER);
    k_mutex_lock(&zbdm_lock, K_FOREVER);
    ensure_slots_initialized();
    retry_pending_save_locked();

    if (!valid_slot(slot)) {
        k_mutex_unlock(&zbdm_lock);
        k_mutex_unlock(&zbdm_op_lock);
        LOG_ERR("Dynamic macro slot %u is out of range; valid range is 0-%u", slot,
                CONFIG_ZMK_ZBDM_SLOTS - 1);
        return -EINVAL;
    }

    if (recording && recording_slot == slot) {
        stop_recording_locked(false);
    }

    k_mutex_unlock(&zbdm_lock);

    int cleanup_ret = stop_playback(true, slot);

    k_mutex_lock(&zbdm_lock, K_FOREVER);

    init_slot(slot);
    int ret = save_slot_tracked(slot);
    k_mutex_unlock(&zbdm_lock);
    k_mutex_unlock(&zbdm_op_lock);
    return ret < 0 ? ret : cleanup_ret;
}

int zmk_zbdm_cancel(void) {
    if (in_generated_event()) {
        return -EBUSY;
    }
    k_mutex_lock(&zbdm_op_lock, K_FOREVER);
    k_mutex_lock(&zbdm_lock, K_FOREVER);
    bool had_activity = recording || playback_active || pressed_usage_count > 0;
    if (recording) {
        uint8_t slot = recording_slot;
        stop_recording_locked(false);
#if IS_ENABLED(CONFIG_ZMK_ZBDM_CANCEL_RESTORE)
        // Restore the pre-recording contents snapshotted at record start.
        copy_slot_used(&slots[slot], &recording_backup);
#else
        // No snapshot was kept (cancel-restore disabled to save RAM): drop the
        // aborted recording and leave the slot empty in RAM. Any previously
        // persisted macro is untouched and reloads on the next boot.
        init_slot(slot);
#endif
    }
    // Retry only after any in-progress recording is discarded and its slot is
    // restored or cleared, so a pending retry never persists canceled/transient
    // contents.
    retry_pending_save_locked();

    k_mutex_unlock(&zbdm_lock);

    // Also drains keys left by an earlier failed cleanup.
    int ret = stop_playback(false, 0);

    if (!had_activity) {
        ZBDM_LOG_DBG("No dynamic macro activity to cancel");
    }

    k_mutex_unlock(&zbdm_op_lock);
    return ret;
}

bool zmk_zbdm_is_recording(void) {
    k_mutex_lock(&zbdm_lock, K_FOREVER);
    bool ret = recording;
    k_mutex_unlock(&zbdm_lock);
    return ret;
}

bool zmk_zbdm_is_playing(void) {
    k_mutex_lock(&zbdm_lock, K_FOREVER);
    bool ret = playback_active || pressed_usage_count > 0;
    k_mutex_unlock(&zbdm_lock);
    return ret;
}

int zmk_zbdm_get_active_slot(uint32_t *slot) {
    if (slot == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&zbdm_lock, K_FOREVER);
    if (recording) {
        *slot = recording_slot;
    } else if (playback_active) {
        *slot = playback_slot;
    } else {
        k_mutex_unlock(&zbdm_lock);
        return -ENOENT;
    }
    k_mutex_unlock(&zbdm_lock);
    return 0;
}

int zmk_zbdm_get_slot_length(uint32_t slot, uint16_t *length) {
    if (!valid_slot(slot) || length == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&zbdm_lock, K_FOREVER);
    ensure_slots_initialized();
    *length = slots[slot].len;
    k_mutex_unlock(&zbdm_lock);
    return 0;
}

uint16_t zmk_zbdm_get_slot_capacity(void) { return CONFIG_ZMK_ZBDM_MAX_EVENTS; }

static int zbdm_keycode_state_changed_listener(const zmk_event_t *eh) {
    // Hottest path in the module: invoked for every keycode event, including the
    // events this module replays during playback. Check the lock-free recording
    // hint first so idle typing (recording inactive) returns after a single
    // atomic load, before the event-type cast and field reads.
    if (!atomic_get(&recording_active)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (in_generated_event()) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->usage_page != HID_USAGE_KEY && ev->usage_page != HID_USAGE_CONSUMER) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!IS_ENABLED(CONFIG_ZMK_ZBDM_RECORD_MODIFIERS) && is_modifier_event(ev)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->keycode > UINT16_MAX) {
        LOG_WRN("Ignoring dynamic macro event with out-of-range keycode: %u", ev->keycode);
        return ZMK_EV_EVENT_BUBBLE;
    }

    k_mutex_lock(&zbdm_lock, K_FOREVER);
    ensure_slots_initialized();

    if (!recording || playback_active) {
        k_mutex_unlock(&zbdm_lock);
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct zbdm_slot *slot = &slots[recording_slot];
    if (!ev->state && !has_recorded_press(slot, ev)) {
        k_mutex_unlock(&zbdm_lock);
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (slot->len >= ARRAY_SIZE(slot->events)) {
        LOG_WRN("Dynamic macro slot %u full; stopping recording", recording_slot);
        stop_recording_locked(true);
        k_mutex_unlock(&zbdm_lock);
        return ZMK_EV_EVENT_BUBBLE;
    }

    slot->events[slot->len++] = (struct zbdm_event){
        .usage_page = ev->usage_page,
        .keycode = (uint16_t)ev->keycode,
        .implicit_modifiers = ev->implicit_modifiers,
        .explicit_modifiers = ev->explicit_modifiers,
        .state = ev->state,
    };

    if (slot->len >= ARRAY_SIZE(slot->events)) {
        LOG_WRN("Dynamic macro slot %u full; stopping recording", recording_slot);
        stop_recording_locked(true);
    }

    k_mutex_unlock(&zbdm_lock);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zbdm, zbdm_keycode_state_changed_listener);
ZMK_SUBSCRIPTION(zbdm, zmk_keycode_state_changed);

static int zbdm_init(void) {
    k_mutex_lock(&zbdm_lock, K_FOREVER);
    ensure_slots_initialized();
    k_mutex_unlock(&zbdm_lock);

    k_work_init_delayable(&playback_work, playback_work_handler);
    return 0;
}

SYS_INIT(zbdm_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#if IS_ENABLED(CONFIG_ZMK_ZBDM_PERSIST)
static bool valid_zbdm_event(const struct zbdm_event *event) {
    return (event->usage_page == HID_USAGE_KEY || event->usage_page == HID_USAGE_CONSUMER) &&
           event->state <= 1;
}

static int parse_settings_slot_id(const char *name, unsigned long *slot) {
    if (strncmp(name, ZBDM_SETTINGS_SLOT_PREFIX, strlen(ZBDM_SETTINGS_SLOT_PREFIX)) != 0) {
        return -ENOENT;
    }

    const char *slot_str = name + strlen(ZBDM_SETTINGS_SLOT_PREFIX);
    // strtoul() accepts leading whitespace and an optional sign, which would let
    // non-canonical keys (e.g. "slot_+0", "slot_ 0") alias a real slot. Also
    // reject leading-zero aliases such as "slot_00", so only the canonical keys
    // written by save_slot() load.
    if (*slot_str < '0' || *slot_str > '9' || (slot_str[0] == '0' && slot_str[1] != '\0')) {
        return -EINVAL;
    }

    char *endptr;
    *slot = strtoul(slot_str, &endptr, 10);
    // Compare against the slot count on the full-width value before truncation so
    // that an out-of-range value which would alias a valid slot when cast to
    // uint32_t (e.g. 2^32 on a 64-bit host) is still rejected. This also avoids a
    // "slot > UINT32_MAX" comparison that is always false (and warns under
    // -Wtype-limits) on 32-bit targets where unsigned long is 32 bits.
    if (endptr == slot_str || *endptr != '\0' || *slot >= CONFIG_ZMK_ZBDM_SLOTS) {
        return -EINVAL;
    }

    return 0;
}

static int validate_loaded_slot(uint32_t slot_id, unsigned long slot, size_t len) {
    if (slots[slot_id].version != ZBDM_SETTINGS_VERSION ||
        slots[slot_id].len > CONFIG_ZMK_ZBDM_MAX_EVENTS || len != slot_settings_len(slot_id)) {
        LOG_WRN("Ignoring invalid dynamic macro settings for slot %lu", slot);
        init_slot(slot_id);
        return -EINVAL;
    }

    // Persisted events are replayed verbatim, bypassing the runtime recording
    // filter, so reject any slot whose stored events fall outside the pages and
    // states this module records (guards against flash corruption).
    for (uint16_t i = 0; i < slots[slot_id].len; i++) {
        if (!valid_zbdm_event(&slots[slot_id].events[i])) {
            LOG_WRN("Ignoring dynamic macro settings for slot %lu with invalid event %u", slot, i);
            init_slot(slot_id);
            return -EINVAL;
        }
    }

    return 0;
}

static int zbdm_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    unsigned long slot;
    int rc = parse_settings_slot_id(name, &slot);
    if (rc < 0) {
        return rc;
    }

    uint32_t slot_id = (uint32_t)slot;

    k_mutex_lock(&zbdm_lock, K_FOREVER);
    ensure_slots_initialized();

    if (len < offsetof(struct zbdm_slot, events)) {
        LOG_WRN("Ignoring dynamic macro settings for slot %lu: length %zu is below "
                "minimum %zu",
                slot, len, offsetof(struct zbdm_slot, events));
        init_slot(slot_id);
        k_mutex_unlock(&zbdm_lock);
        return -EINVAL;
    }

    if (len > sizeof(struct zbdm_slot)) {
        LOG_WRN("Ignoring dynamic macro settings for slot %lu: length %zu exceeds "
                "maximum %zu",
                slot, len, sizeof(struct zbdm_slot));
        init_slot(slot_id);
        k_mutex_unlock(&zbdm_lock);
        return -EINVAL;
    }

    init_slot(slot_id);

    ssize_t ret = read_cb(cb_arg, &slots[slot_id], len);
    if (ret < 0 || (size_t)ret != len) {
        LOG_WRN("Failed to read dynamic macro settings for slot %lu: expected %zu "
                "bytes, got %zd",
                slot, len, ret);
        init_slot(slot_id);
        k_mutex_unlock(&zbdm_lock);
        return ret < 0 ? (int)ret : -EINVAL;
    }

    rc = validate_loaded_slot(slot_id, slot, len);
    k_mutex_unlock(&zbdm_lock);
    return rc;
}

SETTINGS_STATIC_HANDLER_DEFINE(zbdm, ZBDM_SETTINGS_KEY, NULL, zbdm_settings_set, NULL, NULL);

#if IS_ENABLED(CONFIG_ZMK_ZBDM_TEST_SETTINGS)
void zbdm_test_reset_slots(void) {
    k_mutex_lock(&zbdm_lock, K_FOREVER);
    for (uint8_t i = 0; i < ARRAY_SIZE(slots); i++) {
        init_slot(i);
        save_pending[i] = false;
    }
    recording = false;
    atomic_set(&recording_active, 0);
    k_mutex_unlock(&zbdm_lock);
}

int zbdm_test_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    return zbdm_settings_set(name, len, read_cb, cb_arg);
}
#endif
#endif
