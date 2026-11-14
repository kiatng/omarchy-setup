# Maintain and extend

Run commands from the repository root.

Edit the curated sources here, test, then reinstall the affected component. Do not copy new home-directory files into this repository wholesale. Private backups are not a publication source.

```sh
./bin/test
./bin/audit /path/to/gitleaks
```

The audit checks the publishable tree for credentials and unexpected/private artifact types. Review source changes before pushing; automated scanning cannot prove that arbitrary text is non-private. CI runs the portable Python tests and the publication checks. Qt tests run locally with `apps/vigil/bin/test` and `apps/relay/bin/test`.


## Add a component

1. Add curated, portable source under `apps/` or `config/omarchy/`; preserve upstream attribution in [THIRD_PARTY.md](../THIRD_PARTY.md). Use neutral defaults and synthetic fixtures.
2. Update `bin/setup` file planning, layout integration, package dependencies, and backup/restore allowlists as needed. Decide whether it is default or opt-in. Do not discover and copy arbitrary home files.
3. Add one row to the [README catalog](../README.md#customizations) and one usage section in [components](components.md). Keep stable names and links. Split a section into its own page only when needed.
4. Test the changed behavior in an isolated home before applying to a live desktop. Run relevant app tests for app changes; documentation-only changes need link/command review and publication audit.
5. Inspect the exact diff and run the publication audit before pushing. Keep private backups, account data, logs, and screenshots outside the public tree.

`bin/setup install` reinstalls the default bundle plus selected optional apps; it is not a component-specific update command. Preserve existing optional selections when reinstalling. Configuration-only `apply` starts no services, but still writes files and creates a backup.
