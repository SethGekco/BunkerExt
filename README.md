# BunkerExt

Tank-bunker improvements for Yuri's Revenge as a standalone Syringe DLL,
co-loaded with Antares and Phobos. See [DESIGN.md](DESIGN.md) for the feature
roadmap and the reverse-engineered entry pipeline.

Current state: **Phase 0** — a log-only probe that instruments the tank-bunker
entry pipeline to find out where jumpjet units (SCHP) fall out of it.

## Build

CI builds `DevBuild` via MSBuild (see `.github/workflows/build.yml`). The
workflow also runs the Syringe hook overlap + bounds checks against the
[YR-Hook-Encyclopedia](https://github.com/SethGekco/YR-Hook-Encyclopedia)
registry before compiling.

Submodules: `YRpp` and `Phobos` (headers + utility sources only), pinned to
the same commits as ScatterExt.
