# Repository guidance

Read only the guide needed for the task:

- Install or migrate: [installation](docs/install.md), [backup/restore](docs/backup-restore.md).
- Use a component: [component reference](docs/components.md).
- Change or publish: [maintenance](docs/maintenance.md), [privacy](docs/privacy.md).

Work from the repository root. Use `bin/setup plan` before installation; preview configuration with `bin/setup apply --home /tmp/example-home`. Do not apply to the live desktop merely to test documentation. Preserve optional components and user preferences. Never import account directories, private backups, credentials, or diagnostic captures into this public repository.

For prolonged local work, use the installed Vigil skill or `vigil --help`; respect disabled task requests and release only your own hold. Other PC controls: `omarchy <group> --help`. Do not suspend or lock a working computer for validation.

Keep the README catalog and relevant guide current when adding components. Validate according to the maintenance guide before publishing.

## Working with Kiat

Observed preferences, short and editable:

- Offer options before building: a few rendered mockups or directions, then implement the chosen one.
- Expect detail passes after the first cut: colour, sizing, placement, wording.
- Explain the reasoning behind a visual or structural choice, and say plainly when an earlier claim or mock was wrong.
- Prefer meaning over decoration: colour and indicators only where they carry information.
- Questions about a design are often bug reports in disguise; re-check the logic before answering.
- Verify against the real thing (live desktop, rendered output), not only mockups, before calling a change done.
- Keep repository hygiene: commit and push finished work, keep README and component docs current, and keep working clones out of /tmp.
