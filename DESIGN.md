# BunkerExt

Tank-bunker feature DLL for YR (Antares + Phobos stack). Planned features, in
dependency order:

1. **Phase 0 (current): entry-pipeline probe.** SCHP with `Bunkerable=yes` +
   Phobos `BunkerableAnyway=yes` still does not enter a `Bunker=yes` building.
   Three log-only hooks find out where the pipeline dies.
2. **Jumpjet deploy-to-enter** (`Bunker.DeployToEnter=yes` or similar): order a
   jumpjet deployer (SCHP) onto a bunker → fly to center, land/deploy, get
   captured in place.
3. **Turretless swivel** (`Bunker.Swivel=yes` on the building): a bunkered
   turretless unit tracks its target with body facing.
4. **Per-building `BunkerWeaponRangeBonus=`**: additive range for the bunkered
   unit; neither Phobos nor Antares has it.

Out of scope for now: multi-occupant bunkers (single `BunkerLinkedItem` pointer
plus a one-unit state machine make that a rewrite, not an extension).

## What already exists (deployed Phobos Build #48, verified via dll strings)

- Per-BuildingType `BunkerDamageMultiplier=`, `BunkerROFMultMultiplier=`
  (rearm delay is **divided** by it), `BunkerWallsUpSound=`/`DownSound=`.
- `BunkerableAnyway=` on TechnoTypes: skips the turret/weapon/SpeedType checks
  inside `FootClass::IsBunkerableNow`; Phobos' own locomotor blocklist
  (Hover/Mech/Fly/Droppod/Rocket/Ship) does **not** block Jumpjet.
- NOT in this build: `BunkerStateUpdateDelay`, odd-foundation fixes (newer
  develop only).

## Entry pipeline (objdump of pristine gamemd-spawn.exe, image base 0x400000)

All addresses verified against the hook registry; the three probe sites are
unclaimed by any framework.

- `FootClass::IsBunkerableNow` = **0x70FB50** (thiscall, ECX=foot).
  Check order: `Bunkerable` [type+0xD2E] → turret [type+0xCA1] (Phobos hooks
  0x70FB73 here) → has-weapon (vtbl+0x3F4) → SpeedType!=Hover [type+0x67C]!=3
  → not parasitized [foot+0x694]. Callers: 0x43C512, 0x43C86A (both in the
  building-side action path, gated on BuildingType `Bunker=` [type+0x16AB]),
  0x4DFF4F (`FootClass::FindTankBunker` = 0x4DFF40, the AI path).
- `BuildingClass::UpdateBunker` = **0x458E50** (YRpp name; polled per frame).
  Candidate = `BunkerLinkedItem` [this+0x2E4] else first radio link
  (0x65AD30). Candidate must be WhatAmI()==Unit. Then a switch at 0x458E99 on
  `TankBunkerState` [this+0x718] (jump table 0x45937C, states 0..5).
- **Idle (0) handler = 0x458EAF**: requires (a) unit's radio contact resolves
  back to this building (vtbl+0x1BC then 0x47C520), (b) `locomotor->
  Is_Moving() == false` (ILocomotion vtbl+0x10; E_POINTER trap 0x7DC720 if
  locomotor null), (c) center cell free of other technos (0x47C3D0; blockers
  are told to scatter). Only then does it force-track the unit to the
  foundation center (drive-locomotor mechanics; Phobos' odd-foundation hooks
  0x459069/0x459101 sit in the later states).
- Other landmarks: helper 0x458A00 = "is this techno's current bunker me +
  foundation math" (used by the unit-side path at 0x73F74B); self-deploy
  cursor site 0x7400F0 (Antares hooks it; semantics preserved, not a
  blocker); `UnloadBunker`=0x4593A0, `ClearBunker`=0x459470,
  `EmptyBunker`=0x4595C0, destroy path hook site 0x4593C7.

**Working hypothesis** (unconfirmed until the probe log says so): a jumpjet
either never establishes the radio handshake, or hovers with
`Is_Moving()==true` forever, so the Idle state never advances. Do not build
Phase 1 until the log picks one.

## Probe hook sites (Phase 0)

| addr     | size | stolen bytes                  | notes                        |
|----------|------|-------------------------------|------------------------------|
| 0x70FB50 | 5    | push esi; mov esi,ecx; mov eax,[esi] | fn entry, no rel branches |
| 0x458E99 | 6    | mov eax,[esi+0x718]           | jump-target safe, idempotent |
| 0x458EAF | 5    | mov edx,[ebp+0]; mov ecx,ebp  | jump-table target, clean     |

All three `return 0` (re-run stolen bytes). Logging is throttled: probe A by
unit-change or 150 frames, probe B on state/link change only, probe C every
30 frames per building.
