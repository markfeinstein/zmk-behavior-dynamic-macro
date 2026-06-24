# ZMK Behavior Dynamic Macro

Out-of-tree ZMK module that adds QMK/Vial-style runtime macro recording and
playback under the collision-resistant `ZBDM` namespace.

This module records logical `zmk_keycode_state_changed` keyboard and
consumer/media HID events emitted by behaviors such as `&kp`, stores them in
fixed-size runtime slots, and replays them later by raising the same ZMK event
type. It does not record physical matrix positions, so playback follows the
originally produced key actions rather than whatever bindings are currently
assigned to those positions.

## Quick Start

1. Add the module to your ZMK config repo's `config/west.yml`:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: zbdm
      url-base: https://github.com/markfeinstein
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: 904c9aec8822d79149d42c8a9a77e8828eb08f5a
      import: app/west.yml
    - name: zmk-behavior-dynamic-macro
      remote: zbdm
      revision: v0.1.0
      path: modules/zmk/zmk-behavior-dynamic-macro
  self:
    path: config
```

Use the latest published module release tag in place of `v0.1.0`. The ZMK
revision above is intentionally pinned to the revision tested for `v0.1.x` so
firmware builds stay reproducible after upstream ZMK changes. Tracking ZMK
`main` and this module's `main` branch is supported for development and canary
testing, but release tags plus a tested ZMK revision are recommended for normal
keyboard firmware builds.

2. Include the provided behavior and command definitions in your keymap:

```dts
#include <behaviors/zbdm.dtsi>
#include <dt-bindings/zmk/zbdm.h>
```

3. Add bindings. The second parameter is the slot; `ZBDM_STOP` and `ZBDM_CANCEL`
   require `0` because they operate on the current activity rather than a slot.

```dts
bindings = <
    &zbdm ZBDM_TOGGLE 0  &kp A  &kp B  &kp C  &zbdm ZBDM_PLAY 0
    &zbdm ZBDM_STOP 0    &zbdm ZBDM_CLEAR 0          &zbdm ZBDM_CANCEL 0
>;
```

Dynamic macros record keyboard and consumer/media keycode events, not arbitrary
behaviors such as layer changes, pointing, Bluetooth selection, or reset. See
[Limitations](#limitations).

The behavior include enables the module automatically. Optionally override its
defaults in the central-side build's `.conf` file:

```conf
CONFIG_ZMK_ZBDM_SLOTS=4
CONFIG_ZMK_ZBDM_MAX_EVENTS=128
CONFIG_ZMK_ZBDM_PERSIST=y
```

`CONFIG_ZMK_ZBDM_PERSIST` requires `CONFIG_SETTINGS` and working
settings storage on the target board. Commit and push these changes for the
normal ZMK config GitHub Actions build. For a local west workspace, run
`west update`, then build:

```sh
west build -s zmk/app -b <board> -- \
  -DZMK_CONFIG=/path/to/zmk-config/config
```

Add the usual `-DSHIELD=<shield>` CMake argument when your keyboard build uses a
shield. A module listed in `west.yml` does not need `-DZMK_EXTRA_MODULES`; use
that option only for a local checkout outside the manifest.

### Complete Example

Minimal keymap fragment:

```dts
#include <behaviors.dtsi>
#include <behaviors/zbdm.dtsi>
#include <dt-bindings/zmk/zbdm.h>
#include <dt-bindings/zmk/keys.h>

/ {
    keymap {
        compatible = "zmk,keymap";

        default_layer {
            bindings = <
                &zbdm ZBDM_TOGGLE 0  &zbdm ZBDM_PLAY 0  &zbdm ZBDM_CLEAR 0
                &zbdm ZBDM_TOGGLE 1  &zbdm ZBDM_PLAY 1  &zbdm ZBDM_CANCEL 0
            >;
        };
    };
};
```

Matching central-side configuration:

```conf
CONFIG_SETTINGS=y
CONFIG_ZMK_ZBDM_SLOTS=2
CONFIG_ZMK_ZBDM_MAX_EVENTS=128
CONFIG_ZMK_ZBDM_PERSIST=y
```

For a split keyboard, put these options in the central half's configuration,
such as `config/adv360_left.conf`.

### Compatibility

| Module release | Tested ZMK revision | Notes |
| --- | --- | --- |
| `v0.1.x` | [`904c9aec`](https://github.com/zmkfirmware/zmk/commit/904c9aec8822d79149d42c8a9a77e8828eb08f5a) | Required CI target; current ZMK `main` is also tested as a non-blocking canary. |

## Features

- `&zbdm` behavior with record, stop, toggle, play, clear, and cancel commands.
- Multiple build-time configurable slots.
- Fixed-size per-slot storage with no heap allocation.
- Optional Zephyr settings/NVS persistence.
- Central-side behavior locality for split keyboards.
- Playback cleanup that releases any keys still tracked as pressed at macro end
  or cancellation.
- Keyboard and consumer/media HID usage pages are recorded.

## Module Development

Nix is optional module-development tooling; it is not required to consume this
module from a normal ZMK config build. For local module development, use a file
remote that points at the directory containing your checkout:

```yaml
    - name: local-modules
      url-base: file:///path/to/local/modules
    - name: zmk-behavior-dynamic-macro
      remote: local-modules
      revision: main
      path: modules/zmk/zmk-behavior-dynamic-macro
```

This repo includes a Nix flake that provides the Zephyr/ZMK toolchain, `west`,
and helper apps.

Run the module tests from a Linux host with a ZMK checkout available:

```sh
ZMK_APP_DIR=/path/to/zmk/app nix run .#test
```

The test wrapper runs every test case under `tests/zmk-behavior-dynamic-macro` by default.
You can pass one or more test case paths to run a subset:

```sh
ZMK_APP_DIR=/path/to/zmk/app nix run .#test -- \
  tests/zmk-behavior-dynamic-macro/basic \
  tests/zmk-behavior-dynamic-macro/capacity-auto-stop
```

CI runs the complete native_sim suite on Linux, including a build with the module
present but `CONFIG_ZMK_ZBDM=n`.

Build firmware with this module as an extra ZMK module:

```sh
ZMK_APP_DIR=/path/to/zmk/app \
ZMK_CONFIG=/path/to/zmk-config/config \
BOARD='nice_nano_v2' \
nix run .#build
```

For an interactive shell:

```sh
nix develop
```

`native_sim` tests use Zephyr's POSIX architecture and must run on Linux. On
macOS, use a Linux machine or VM for tests.

Maintainers must complete the automated and hardware gates in
[RELEASING.md](RELEASING.md) before creating a release tag.

## Kconfig

The module defaults on when your keymap enables a
`zmk,behavior-zbdm` devicetree node, such as the provided `&zbdm` node.
You can also enable it explicitly in your central-side build config, for example
`config/adv360_left.conf` or `config/prj.conf`:

```conf
CONFIG_SETTINGS=y
CONFIG_ZMK_ZBDM=y
CONFIG_ZMK_ZBDM_SLOTS=4
CONFIG_ZMK_ZBDM_MAX_EVENTS=128
CONFIG_ZMK_ZBDM_MAX_PRESSED_USAGES=16
CONFIG_ZMK_ZBDM_PERSIST=y
```

`CONFIG_ZMK_ZBDM_PERSIST` depends on `CONFIG_SETTINGS`. ZMK normally
enables settings already; include `CONFIG_SETTINGS=y` only if your board does
not, otherwise persistence silently stays disabled.

Available options:

- `CONFIG_ZMK_ZBDM`: enables the module.
- `CONFIG_ZMK_ZBDM_SLOTS`: number of slots, default `4` (range
  `1`-`8`).
- `CONFIG_ZMK_ZBDM_MAX_EVENTS`: events per slot, default `128` (range
  `1`-`512`). A normal tap consumes two events (press and release); modifier
  shortcuts can consume more. Each slot statically reserves a 4-byte header plus
  this many 8-byte events, for roughly `SLOTS x (4 + MAX_EVENTS x 8)` bytes. The
  extra slot-sized recording backup is allocated only when
  `CONFIG_ZMK_ZBDM_CANCEL_RESTORE=y`.
- `CONFIG_ZMK_ZBDM_MAX_PRESSED_USAGES`: simultaneously pressed usages
  tracked during playback cleanup, default `16` (range `1`-`512`). This buffer
  costs 6 bytes per entry; raise it for unusual NKRO macros that press many keys
  before releasing them.
- `CONFIG_ZMK_ZBDM_CANCEL_RESTORE`: restore a slot's previous contents
  when `ZBDM_CANCEL` aborts a recording, default `y`. Keeps one extra slot-sized
  backup buffer (~`MAX_EVENTS x 8` bytes). Disable to reclaim that RAM;
  `ZBDM_CANCEL` then leaves the canceled slot empty in RAM (a previously persisted
  macro still reloads on the next boot).
- `CONFIG_ZMK_ZBDM_PERSIST`: save slots with Zephyr settings, default
  `y` when `CONFIG_SETTINGS` is enabled.
- `CONFIG_ZMK_ZBDM_PLAYBACK_WAIT_MS`: fixed wait inserted between
  consecutive replay events, default `10`. Playback replays at this fixed
  interval rather than reproducing recorded human-speed timing.
- `CONFIG_ZMK_ZBDM_PLAYBACK_BATCH_SIZE`: events replayed per work item
  when `CONFIG_ZMK_ZBDM_PLAYBACK_WAIT_MS=0`, default `16` (range
  `1`-`128`). Higher values reduce scheduling overhead; lower values improve
  cancellation responsiveness for very long zero-delay macros.
- `CONFIG_ZMK_ZBDM_RECORD_MODIFIERS`: record explicit modifier keys,
  default `y`.
- `CONFIG_ZMK_ZBDM_DEBUG`: verbose debug logging.

### Namespace and upstream compatibility

ZBDM intentionally does not provide the generic `&dm`, `DM_*`,
`zmk_dynamic_macro_*`, `zmk,behavior-dynamic-macro`, or `dynamic_macro/`
settings namespaces proposed by [upstream ZMK PR #2678][zmk-pr-2678]. Its
`&zbdm`, `ZBDM_*`, `zmk_zbdm_*`, `zmk,behavior-zbdm`, and `zbdm/` names allow
this module to coexist with a future upstream implementation.

Pre-release builds that used the old generic names must update their keymaps and
configuration. Persisted slots from the old `dynamic_macro/` settings subtree
are intentionally not imported; record them again after upgrading.

## Attribution

Portions of the behavior devicetree include, command binding header, and behavior
adapter source are adapted from [upstream ZMK PR #2678][zmk-pr-2678]. The
original copyright notice for those portions is preserved in source headers and
[NOTICE.md](NOTICE.md).

## Keymap Usage

Add the behavior and command header to your keymap:

```dts
#include <behaviors/zbdm.dtsi>
#include <dt-bindings/zmk/zbdm.h>
```

If you prefer defining the behavior directly, keep `display-name` so the
behavior is easier to identify in ZMK Studio:

```dts
#include <dt-bindings/zmk/zbdm.h>

/ {
    behaviors {
        zbdm: zbdm {
            compatible = "zmk,behavior-zbdm";
            #binding-cells = <2>;
            display-name = "ZBDM Dynamic Macro";
        };
    };
};
```

Optional convenience defines:

```dts
#define ZBDM_REC_0  &zbdm ZBDM_RECORD 0
#define ZBDM_STOP_  &zbdm ZBDM_STOP 0
#define ZBDM_TOG_0  &zbdm ZBDM_TOGGLE 0
#define ZBDM_PLAY_0 &zbdm ZBDM_PLAY 0
#define ZBDM_CLR_0  &zbdm ZBDM_CLEAR 0
#define ZBDM_CANCL  &zbdm ZBDM_CANCEL 0
```

Example bindings:

```dts
bindings = <
    ZBDM_TOG_0  &kp A  &kp B  &kp C  ZBDM_PLAY_0
>;
```

Commands:

- `ZBDM_RECORD slot`: clear `slot` and start recording into it.
- `ZBDM_STOP 0`: stop the current recording and persist it if enabled.
- `ZBDM_TOGGLE slot`: stop if already recording `slot`; otherwise clear and
  record `slot`.
- `ZBDM_PLAY slot`: replay `slot` if non-empty. While recording, this instead
  stops and saves the active recording (it does not start playback). Ignored
  while another slot is already playing.
- `ZBDM_CLEAR slot`: stop recording/playback for `slot`, clear it, and persist the
  empty slot if enabled.
- `ZBDM_CANCEL 0`: cancel active recording without saving and/or stop playback,
  releasing tracked pressed keys. With `CONFIG_ZMK_ZBDM_CANCEL_RESTORE=y`
  (default) a canceled recording restores the slot's previous contents; with it
  disabled the canceled slot is left empty in RAM.

## Behavior Semantics

If recording starts while another slot is recording, the previous slot is
stopped and saved, then the new slot is cleared and recorded.

If a slot reaches `CONFIG_ZMK_ZBDM_MAX_EVENTS`, recording stops and
saves the events captured so far.

Playback does not overlap. A play request while recording stops and saves the
active recording instead of starting playback, so the macro key doubles as a
record-stop. A play request while another playback is active is ignored. Playback
can be interrupted with `ZBDM_CANCEL` or by clearing the playing slot; both release
tracked pressed keys.

The dynamic macro control key is not recorded because the behavior itself does
not emit a keycode event. Other concurrently pressed keys are recorded normally.

The behavior runs on press and ignores release. This is intentional for
command-style bindings and avoids accidentally toggling recording twice.

## Status and Feedback

The module has no built-in LED, display, or other user-visible status indicator.
Custom firmware code can query `zmk_zbdm_is_recording()`,
`zmk_zbdm_is_playing()`, `zmk_zbdm_get_active_slot()`,
`zmk_zbdm_get_slot_length()`, and
`zmk_zbdm_get_slot_capacity()` for an LED/OLED status integration.
Operational warnings and errors are available through Zephyr logs
(`CONFIG_ZMK_ZBDM_DEBUG=y` adds state and benign no-op messages).

| Situation | Result |
| --- | --- |
| Recording starts | The target slot is cleared and recording begins; there is no built-in visual indication. |
| Slot reaches capacity | Recording automatically stops and saves the events captured so far; a warning is logged. |
| Slot number is invalid | The command does nothing and an error is logged. |
| Empty slot is played | Playback is a no-op; debug logging reports it when enabled. |
| Persistence save fails | Data remains in RAM, the slot is marked pending, and the error is logged; later macro commands retry the save. |
| Recording would overwrite a slot with a pending save | Recording that slot is refused until a retry succeeds, and a warning is logged. |

Behavior bindings consume command return codes, so failures are not reported as
key events. Use logs or custom status integration when explicit feedback is
required.

## Public C API

Custom firmware can include `<zmk/zbdm.h>` when `CONFIG_ZMK_ZBDM=y`. The header
fully documents arguments, state transitions, persistence effects, and return
codes for:

- `zmk_zbdm_record()`, `zmk_zbdm_stop()`, and `zmk_zbdm_toggle()`;
- `zmk_zbdm_play()`, `zmk_zbdm_clear()`, and `zmk_zbdm_cancel()`;
- `zmk_zbdm_is_recording()` and `zmk_zbdm_is_playing()`; and
- `zmk_zbdm_get_active_slot()`, `zmk_zbdm_get_slot_length()`, and
  `zmk_zbdm_get_slot_capacity()`.

Slots are zero-based. All API calls use kernel synchronization and require thread
context; never call them from an ISR. Mutating calls must not run on ZMK's system
workqueue because stopping playback can synchronously cancel work on that queue.
Calls made synchronously from a ZBDM-generated event return `-EBUSY` instead of
re-entering the module.

## Split Keyboards

The behavior declares `BEHAVIOR_LOCALITY_CENTRAL`. The behavior device is
registered on both split halves so keymaps that are shared by central and
peripheral builds can still reference `&zbdm`, while the
recording/playback runtime is linked only for non-split builds or the split
central side. On a Kinesis Advantage 360 Pro, this means recording and playback
run on the left/central half.

ZMK split peripherals forward position events to the central side, where the
keymap resolves them into `zmk_keycode_state_changed` events. Because this
module listens on that central-side keycode event pipeline, key presses from both
halves should be visible as long as they ultimately emit keyboard HID keycode
events.

## Persistence

When `CONFIG_ZMK_ZBDM_PERSIST=y`, each slot is stored under
`zbdm/slot_N` in Zephyr settings. The v1 stored format is binary and
includes a version byte, event length, and compact 8-byte event records. Invalid
lengths, unknown versions, out-of-range slot data, or events outside the recorded
HID usage pages are ignored and cleared in RAM.

Persisted dynamic macros are stored as plaintext/binary keystroke data in the
keyboard's settings/NVS storage. Do not record passwords, recovery phrases, or
other secrets. Clear dynamic macro slots before sharing, selling, or transferring
hardware.

Settings are written only when recording stops or a slot is cleared, not on every
key event.

For persistence, verify both options are enabled:

```conf
CONFIG_SETTINGS=y
CONFIG_ZMK_ZBDM_PERSIST=y
```

The board must also provide a working Zephyr settings storage backend. If macros
disappear after reboot, inspect the build's Kconfig output for both options,
confirm the board's settings/NVS storage configuration, and enable logging to
check for save or load errors.

## Limitations

- Records keyboard and consumer/media HID usage-page events from
  `zmk_keycode_state_changed`.
- Mouse keys, pointing, layers, Bluetooth profile switching, output switching,
  bootloader, reset, transparent/no-op, and arbitrary behavior bodies are not
  recorded.
- Playback replays at a fixed interval
  (`CONFIG_ZMK_ZBDM_PLAYBACK_WAIT_MS`) rather than reproducing recorded
  human-speed timing. Inter-event delays are not stored in the v1 format.
- If recording is stopped while keys are still held, release events may be
  missing. Playback tracks up to `CONFIG_ZMK_ZBDM_MAX_PRESSED_USAGES`
  pressed usages and releases leftovers at the end to avoid stuck keys.
- ZMK Studio can assign `&zbdm` bindings only when this behavior exists in
  firmware and behavior metadata support is enabled. Studio still cannot edit
  the recorded macro contents without custom Studio support.

## Automated Test Coverage

The checked-in native_sim tests under `tests/zmk-behavior-dynamic-macro` currently cover:

- Basic record, stop, and play of a keyboard keycode sequence.
- Auto-stop and cleanup at one-event and larger slot capacities.
- Ignoring releases for keys held before recording started.
- Modifier and consumer/media event replay.
- Playback cancellation cleanup and recording cancellation with restore enabled
  and disabled.
- Starting a new recording immediately after canceling playback.
- Valid toggle/clear, recording-slot transitions, and play-while-recording.
- Modifier recording enabled and disabled.
- Pressed-usage capacity and generated-event listener failures.
- Empty playback, invalid slots, and command parameter validation.
- Persistence parser rejection and failed-save retry behavior.
- Zero-delay batched playback.
- A normal ZMK build with this module present but explicitly disabled.
- Split central/peripheral auto-enable and link-boundary checks in CI.

Actual persistence across a power cycle and physical split-keyboard forwarding
remain mandatory [release checks](RELEASING.md) because native_sim does not model
those hardware boundaries.

## Manual Test Plan

1. Build both central and peripheral split firmware with the module present.
2. Record `A B C` into slot `0`, stop recording, and play slot `0`.
3. Record keys from both halves of a split keyboard and replay the sequence.
4. Reboot and verify the macro persists when settings/NVS is enabled.
5. Clear slot `0` and verify play does nothing.
6. Verify record/toggle/stop/play/clear/cancel controls do not appear in the
    recorded output.
7. Verify no modifiers or keys remain stuck after playback cancellation or
    clearing a playing slot.

## Advantage 360 Pro Example

For a Kinesis Advantage 360 Pro keymap, one option is to put slot `0` controls on
a `MOD` layer with these aliases:

```dts
#define dmr &zbdm ZBDM_TOGGLE 0
#define dms &zbdm ZBDM_STOP 0
#define dmp &zbdm ZBDM_PLAY 0
#define dmc &zbdm ZBDM_CLEAR 0
#define dmx &zbdm ZBDM_CANCEL 0
```

The bindings are on the left home row of the `MOD` layer in order: toggle
record, stop, play, clear, cancel.

[zmk-pr-2678]: https://github.com/zmkfirmware/zmk/pull/2678
