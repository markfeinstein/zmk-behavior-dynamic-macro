# Release process

Releases are created only from annotated `vMAJOR.MINOR.PATCH` tags after the
fixed-revision CI jobs and the following manual checks pass. Copy
`release-notes/TEMPLATE.md` to `release-notes/vX.Y.Z.md`, replace every
placeholder with evidence, complete every checklist item, and commit that file
before creating the tag. The tag workflow refuses to publish a GitHub release
without a matching completed release-notes file.

## Release record

- Release: `vX.Y.Z`
- Commit: `<module commit SHA>`
- Required ZMK revision: `904c9aec8822d79149d42c8a9a77e8828eb08f5a`
- Test keyboard and controller: `<model>`
- Central firmware artifact/checksum: `<URL and SHA-256>`
- Peripheral firmware artifact/checksum: `<URL and SHA-256>`
- Tester/date: `<name, YYYY-MM-DD>`

## Automated gates

- [ ] `Nix checks` passed on the tagged commit.
- [ ] `ZMK compatibility (fixed)` passed on the tagged commit.
- [ ] `Consumer manifest` passed on the tagged commit.
- [ ] The `main-canary` result was reviewed; any failure is documented.
- [ ] The CI log records resolved west project revisions.

## Hardware persistence

- [ ] Record `A B C`, stop, play, and verify exactly `A B C`.
- [ ] Reboot/power-cycle and verify the macro still plays when persistence is enabled.
- [ ] Clear the slot, reboot, and verify it remains empty.
- [ ] Cancel a replacement recording and verify the old slot is restored when
      `CONFIG_ZMK_ZBDM_CANCEL_RESTORE=y`.
- [ ] Repeat with persistence disabled and verify the slot does not survive reboot.
- [ ] Review logs for settings save/load errors.

## Physical split behavior

- [ ] Both central and peripheral firmware build from the recorded revisions.
- [ ] The same keymap containing `&zbdm` is accepted by both halves.
- [ ] Record at least one key from each half and verify playback order.
- [ ] Disconnect/reconnect the peripheral and repeat recording and playback.
- [ ] Cancel playback while a key is held; verify the host has no stuck key.
- [ ] Clear a playing slot; verify the host has no stuck key.

## Resource and upgrade checks

- [ ] Record central/peripheral RAM and flash deltas from the build output.
- [ ] Confirm the README compatibility table and installation manifest match the release.
- [ ] Confirm release notes warn that persisted keystrokes are plaintext.
- [ ] For a namespace migration, confirm old `dynamic_macro/` settings are intentionally
      ignored and the migration note is present.

Create the annotated tag only after completing and committing
`release-notes/vX.Y.Z.md`:

```sh
git add release-notes/vX.Y.Z.md
git commit -m "docs: add vX.Y.Z release notes"
git tag -a vX.Y.Z -m "vX.Y.Z"
git push origin vX.Y.Z
```

The tag workflow reruns the gates, validates the annotated tag, checks that
`release-notes/$TAG.md` exists with no placeholders or unchecked checklist items,
and creates the GitHub release from that file. Protect `v*` tags from deletion
or rewriting in repository rules.
