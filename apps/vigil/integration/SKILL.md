---
name: vigil
description: Keep this PC awake for prolonged local tasks using Vigil; release the hold when work ends.
---
Use `vigil run --name "Task" -- <command>` for supervised work. For multi-command work, use `vigil task begin --name "Task" --json`, renew its returned token every 5 minutes with `vigil task renew <token> --json`, and finish with `vigil task finish <token> --json`. Do not finish while background work remains.
If requests are disabled, tell the user; never enable them yourself. Release only your token. Never change power policy or force sleep to finish a task. Use `vigil status --json` or `vigil --help` when needed. Other PC controls: `omarchy <group> --help`.
