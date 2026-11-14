# Vigil

Keep local work running while the lid is closed. Manual holds and task holds share one state; Off revokes tasks. Battery warnings begin 10 percentage points above the adjustable reserve (default 15%); reserve sleep has a 60-second countdown. External power cancels a battery countdown. Normal screen locking stays active. Other sleep inhibitors are respected.

Build/test with `bin/build` and `bin/test` from this app directory. The root setup installer supplies the desktop integration. `vigil --help` documents task registration and supervised commands. `vigil skill install` installs minimal agent guidance.

The tests simulate battery/lid events and never suspend the active machine. Physical lid/suspend behavior must be checked on the destination hardware. Startup/resume clear holds and disable task requests.
