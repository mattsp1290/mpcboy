# Device tests

Results observed on a real MPC Live III. An implementing agent never records a
device result it did not observe; unobserved steps stay "pending" and block
the release (see the plan's definition of done).

| Step | Status | Notes |
|---|---|---|
| WP1 device smoke | pending (needs user's MPC Live III) | Install the placeholder folder (`scripts/make_min_folder.sh` output) into `VST2 Plugins/`; confirm "mpcboy" is listed under External Plugins and plays a quiet 440 Hz sine on pad hits. |
| G-CPU (WP3 test 7) | pending (needs user's MPC Live III) | Build the CPU spike plugin (`-DMPCBOY_CPU_PROBE=ON`, see `docs/BENCH.md` for the exact commands), install the folder, insert it on a track. It holds a note on its own (4 channels, 4 tables) and shows `m<mean> p99 <p99>` in microseconds per process() call as the display text of the read-only parameter `CPU us` (refreshed once per second). Hold at a 128-frame buffer for 60 s. Pass: mean <= 145 us and p99 <= 290 us. Not run: no device result is claimed. If it is still pending at WP10 it is a release blocker (WP10 row 9). |
