# Repository guidance

Read only the guide needed for the task:

- Install or migrate: [installation](docs/install.md), [backup/restore](docs/backup-restore.md).
- Use a component: [component reference](docs/components.md).
- Change or publish: [maintenance](docs/maintenance.md), [privacy](docs/privacy.md).

Work from the repository root. Use `bin/setup plan` before installation; preview configuration with `bin/setup apply --home /tmp/example-home`. Do not apply to the live desktop merely to test documentation. Preserve optional components and user preferences. Never import account directories, private backups, credentials, or diagnostic captures into this public repository.

For prolonged local work, use the installed Vigil skill or `vigil --help`; respect disabled task requests and release only your own hold. Other PC controls: `omarchy <group> --help`. Do not suspend or lock a working computer for validation.

Keep the README catalog and relevant guide current when adding components. Validate according to the maintenance guide before publishing.
