# Third-party components

| Component | Path | License | Upstream | Notes |
|---|---|---|---|---|
| SameBoy APU (`apu.c`, `apu.h`) | `third_party/vendor/sameboy/` | Expat (MIT), see `third_party/vendor/sameboy/LICENSE` | <https://github.com/LIJI32/SameBoy> `208ba4afabffab9edde416f2dbb8ae459e34adb8`, via rackboy | Byte-identical to rackboy's vendored copy. |
| rackboy core (APU shim, voice, pitch, taps, table interpreter, `.sav` parser, resampler) | `src/{apu,core,table,sav,dsp}/`, `tests/rackboy/` | MIT (see `LICENSE`) | <https://github.com/mattsp1290/rackboy> `245230d` | See `src/VENDORED.md` for the file list and local changes. |
| doctest 2.4.11 | `tests/third_party/doctest/` | MIT, see `tests/third_party/doctest/LICENSE.txt` | <https://github.com/doctest/doctest> `ae7a13539fb71f270b87eb2e874fbac80bc8dda2` | Test-only. |
| Akai MPC Plugin Builder SDK | `vendor/mpc-plugin-builder-sdk/` (git submodule) | MIT | <https://github.com/inmusic-software/mpc-plugin-builder-sdk> `febd2ca5bd9eb16c4a412837196e145566bc981b` | Framework headers, host simulator, validator. |
