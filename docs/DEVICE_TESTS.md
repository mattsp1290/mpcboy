# Device tests

Results observed on a real MPC Live III. An implementing agent never records a
device result it did not observe; unobserved steps stay "pending" and block
the release (see the plan's definition of done).

| Step | Status | Notes |
|---|---|---|
| WP1 device smoke | pending (needs user's MPC Live III) | Install the placeholder folder (`scripts/make_min_folder.sh` output) into `VST2 Plugins/`; confirm "mpcboy" is listed under External Plugins and plays a quiet 440 Hz sine on pad hits. |
