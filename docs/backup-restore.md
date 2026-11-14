# Backup and restore

Run commands from the repository root.

```sh
./bin/setup backup --agents --relay
# Prints a local backup directory under ~/.local/state/omarchy-setup/backups/
./bin/setup restore --from /path/to/that/backup
```

Backups are private, local, allowlisted copies; keep them off GitHub and transfer them through your own secure backup channel. They preserve customized widget code, the shell layout, Afterglow preferences, and Vigil’s battery reserve. Restore verifies checksums, rejects unexpected paths/symlinks, and creates a rollback backup first. When moving between usernames, it rebases old home-directory paths. It does not migrate credentials, hardware configuration, or active tasks.

On a new machine, run `install` first, then restore your private backup. Restart the shell when unlocked. Agent instructions and packaged applications are reinstalled from source, not restored from account directories. Hardware-specific app preferences should be checked on the destination.

`apply --home /tmp/example-home` writes only configuration to an alternate home and starts no services. It is useful for previewing the bundled configuration in an isolated directory; it does not restore a backup. `plan` makes no changes. Existing layout settings and unrelated configuration keys are preserved; stock widgets with bundled replacements are switched to the custom versions.
