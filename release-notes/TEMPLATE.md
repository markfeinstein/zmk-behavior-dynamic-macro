# vX.Y.Z

## Compatibility

- Module release: `vX.Y.Z`
- Module commit: `<module commit SHA>`
- Required ZMK revision: `904c9aec8822d79149d42c8a9a77e8828eb08f5a`
- main-canary result: `<pass/fail and notes>`

## Automated gates

- [ ] `Nix checks` passed on the tagged commit: `<workflow URL>`
- [ ] `ZMK compatibility (fixed)` passed on the tagged commit: `<workflow URL>`
- [ ] `Consumer manifest` passed on the tagged commit: `<workflow URL>`
- [ ] CI log records resolved west project revisions: `<log URL or excerpt>`

## Hardware persistence evidence

- Test keyboard and controller: `<model>`
- Central firmware artifact/checksum: `<URL and SHA-256>`
- Peripheral firmware artifact/checksum: `<URL and SHA-256>`
- Tester/date: `<name, YYYY-MM-DD>`

- [ ] Record `A B C`, stop, play, and verify exactly `A B C`: `<evidence>`
- [ ] Reboot/power-cycle and verify the macro still plays when persistence is enabled: `<evidence>`
- [ ] Clear the slot, reboot, and verify it remains empty: `<evidence>`
- [ ] Cancel a replacement recording and verify the old slot is restored when `CONFIG_ZMK_ZBDM_CANCEL_RESTORE=y`: `<evidence>`
- [ ] Repeat with persistence disabled and verify the slot does not survive reboot: `<evidence>`
- [ ] Review logs for settings save/load errors: `<evidence>`

## Physical split behavior evidence

- [ ] Both central and peripheral firmware build from the recorded revisions: `<evidence>`
- [ ] The same keymap containing `&zbdm` is accepted by both halves: `<evidence>`
- [ ] Record at least one key from each half and verify playback order: `<evidence>`
- [ ] Disconnect/reconnect the peripheral and repeat recording and playback: `<evidence>`
- [ ] Cancel playback while a key is held; verify the host has no stuck key: `<evidence>`
- [ ] Clear a playing slot; verify the host has no stuck key: `<evidence>`

## Resource and upgrade checks

- [ ] Central/peripheral RAM and flash deltas from build output: `<evidence>`
- [ ] README compatibility table and installation manifest match this release: `<evidence>`
- [ ] Release notes warn that persisted keystrokes are plaintext: `<evidence>`
- [ ] For a namespace migration, old `dynamic_macro/` settings are intentionally ignored and migration note is present: `<evidence or N/A>`

## User-facing notes

- Persisted dynamic macros are stored as plaintext/binary keystroke data in Zephyr settings/NVS. Do not record passwords, recovery phrases, or other secrets. Clear dynamic macro slots before sharing, selling, or transferring hardware.

## Known limitations

- Records keyboard and consumer/media HID usage-page events only.
- Playback uses a fixed interval rather than recorded human timing.
- Native simulation does not model physical split forwarding or power-cycle persistence; those are covered by the evidence above.
