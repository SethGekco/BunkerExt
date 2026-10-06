// ---------------------------------------------------------------------------
// Phase 0: tank-bunker entry probe.  Phase 1: jumpjet deploy-to-enter.
//
// Probe findings (2026-09-23, in-game, see DESIGN.md for the pipeline):
//   - MGTK (turretless, Drive loco) + BunkerableAnyway=yes: full success.
//     Idle -> 1..5 -> Bunkered at 15-frame ticks, hull tracks targets. The
//     turretless-swivel feature costs zero code.
//   - SCHP (Jumpjet loco): IsBunkerableNow fired with perfect flags and still
//     produced the no-entry cursor. Disassembly of the DEPLOYED Phobos Build
//     #48 (handler at RVA 0x2C900) shows why: it gates on the locomotor GUID
//     (five blocklist compares + an allow test) BEFORE reading
//     BunkerableAnyway, and Jumpjet does not pass. Drive does — which is the
//     exact MGTK/SCHP differential the probe logged.
//
// Phase 1 therefore takes over at 0x70FB50 (function entry — we run before
// Phobos' 0x70FB73 hook can reject), and drives a deploy-to-enter capture for
// tagged jumpjet deployers from the UpdateBunker hooks we already own:
//
//   enter order -> jumpjet flies to the bunker (vanilla Mission::Enter, radio
//   link forms) -> Idle handler sees a tagged link and orders a deploy
//   (Mission::Unload; DeployToLand makes it land) -> the deploy breaks the
//   radio link, so the dispatch hook ADOPTS a deployed, grounded, tagged unit
//   standing on the foundation (sets BunkerLinkedItem) -> Idle handler sees it
//   deployed+grounded and jumps the state machine straight to RaiseWalls(5),
//   skipping the drive-locomotor tracking states a jumpjet cannot perform.
//   Vanilla state 5 raises the walls and finalizes the links exactly as the
//   MGTK trace showed (bunkerLinked=1 on the 5 -> 6 transition).
//
// Return targets (verified against objdump of gamemd-spawn.exe):
//   0x70FBCA  bare `ret` in IsBunkerableNow's success epilogue. Jumping there
//             from the ENTRY hook returns AL to the caller without the
//             function's `pop esi` (its `push esi` never ran in this path).
//   0x459374  UpdateBunker's common epilogue (pop edi/esi/ebp/ebx; ret) —
//             used to keep the vanilla Idle body away from jumpjet links.
//
// All hook addresses verified against the hook registry: no other framework
// touches them. All stolen-byte windows are relative-branch-free, so
// `return 0` stays safe.
// ---------------------------------------------------------------------------

#include <BuildingClass.h>
#include <HouseClass.h>
#include <UnitClass.h>
#include <FootClass.h>
#include <TechnoTypeClass.h>
#include <BuildingTypeClass.h>
#include <CellClass.h>
#include <MapClass.h>
#include <Helpers/Cast.h>
#include <Fundamentals.h>
#include <Dir.h>
#include <JumpjetLocomotionClass.h>
#include <LocomotionClass.h>
#include <AnimClass.h>
#include <AnimTypeClass.h>
#include <Memory.h>

#include <Syringe.h>
#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

#include <BunkerTags.h>

namespace BunkerProbe
{
	// One-shot banner so an otherwise quiet log still proves the DLL loaded
	// and at least one hook fired (ScatterExt lesson: an empty log cannot
	// distinguish "no hooks ran" from "nothing to report").
	static bool bannerShown = false;

	static void Banner()
	{
		if (!bannerShown)
		{
			bannerShown = true;
			Debug::Log("[BunkerExt] probe active, frame %d\n", Unsorted::CurrentFrame);
		}
	}

	// Throttle state for probe A. IsBunkerableNow runs on every mouse-over
	// frame, so log only when the unit changes or every ~10s for the same one.
	static FootClass* lastQueried = nullptr;
	static int lastQueriedFrame = -100000;

	// Per-building memory for probes B/C. Raw pointers as keys never get
	// erased -- acceptable for a diagnostic build only: bounded by the number
	// of bunker buildings ever polled, and stale entries are never
	// dereferenced (pointers are used as opaque keys, IDs re-read live).
	struct BunkerMemory
	{
		int state = -1;
		TechnoClass* link = nullptr;
		int lastIdleLogFrame = -100000;
		int lastDeployCmdFrame = -100000;
		int lastAdoptScanFrame = -100000;
		int lastScanLogFrame = -100000;
		int lastEntryLogFrame = -100000;
	};

	static constexpr size_t MaxTracked = 64;
	static BuildingClass* trackedKeys[MaxTracked];
	static BunkerMemory trackedVals[MaxTracked];
	static size_t trackedCount = 0;

	static BunkerMemory* Track(BuildingClass* pBld)
	{
		for (size_t i = 0; i < trackedCount; ++i)
		{
			if (trackedKeys[i] == pBld)
				return &trackedVals[i];
		}
		if (trackedCount < MaxTracked)
		{
			trackedKeys[trackedCount] = pBld;
			trackedVals[trackedCount] = BunkerMemory {};
			return &trackedVals[trackedCount++];
		}
		return nullptr;
	}

	static const char* IdOf(TechnoClass* pTechno)
	{
		return pTechno ? pTechno->GetTechnoType()->ID : "<null>";
	}

	// Entry intent: a unit ordered into a bunker (Enter cursor) and told to
	// deploy, awaiting deploy-complete before final capture. Small fixed
	// table; pointers are opaque keys, re-validated live each use.
	struct Intent { UnitClass* unit; BuildingClass* bld; int doneFrame; bool hidden; };
	static constexpr size_t MaxIntents = 32;
	static Intent intents[MaxIntents];
	static size_t intentCount = 0;

	static Intent* FindIntent(UnitClass* pUnit)
	{
		for (size_t i = 0; i < intentCount; ++i)
			if (intents[i].unit == pUnit)
				return &intents[i];
		return nullptr;
	}

	static void AddIntent(UnitClass* pUnit, BuildingClass* pBld, int doneFrame)
	{
		if (FindIntent(pUnit))
			return;
		if (intentCount < MaxIntents)
			intents[intentCount++] = { pUnit, pBld, doneFrame, true };
	}

	// True while a unit's deploy animation is playing on the pad — the draw
	// hook uses this to skip rendering the voxel so the animation shows alone.
	static bool IsHidden(UnitClass* pUnit)
	{
		auto const pI = FindIntent(pUnit);
		return pI && pI->hidden;
	}

	static void RemoveIntent(UnitClass* pUnit)
	{
		for (size_t i = 0; i < intentCount; ++i)
			if (intents[i].unit == pUnit)
			{
				intents[i] = intents[--intentCount];
				return;
			}
	}

	// YRpp's GetNthLink is an unchecked Items[idx] read; the game's own
	// 0x65AD30 bounds-checks. Guard so an empty link vector can't fault us.
	static TechnoClass* FirstLink(RadioClass* pRadio)
	{
		return pRadio->RadioLinks.Capacity > 0 ? pRadio->RadioLinks[0] : nullptr;
	}

}

// --- Probe A + Phase 1 gate: who gets asked "is bunkerable now". ------------
// 0x70FB50 = FootClass::IsBunkerableNow entry. Stolen bytes (5):
//   56 / 8b f1 / 8b 06  (push esi; mov esi,ecx; mov eax,[esi])  -- clean.
DEFINE_HOOK(0x70FB50, FootClass_IsBunkerableNow_BunkerExtProbe, 0x5)
{
	enum { RetTrue = 0x70FBCA }; // bare `ret`; AL already set, esi untouched

	GET(FootClass*, pThis, ECX);
	GET_STACK(unsigned int, retAddr, 0x0); // who asked: 0x43C517 / 0x43C86F / 0x4DFF54

	BunkerProbe::Banner();

	const int frame = Unsorted::CurrentFrame;
	auto const pType = pThis->GetTechnoType();

	if (pThis != BunkerProbe::lastQueried
		|| frame - BunkerProbe::lastQueriedFrame > 150)
	{
		BunkerProbe::lastQueried = pThis;
		BunkerProbe::lastQueriedFrame = frame;

		Debug::Log(
			"[BunkerExt] f%d IsBunkerableNow(%s) from %08X: Bunkerable=%d Turret=%d"
			" SpeedType=%d LocoData1=%08X Parasite=%d tag=%d\n",
			frame, pType->ID, retAddr,
			pType->Bunkerable, pType->Turret,
			static_cast<int>(pType->SpeedType),
			static_cast<unsigned int>(pType->Locomotor.Data1),
			pThis->ParasiteEatingMe != nullptr,
			BunkerTags::DeployToEnter(pType));
	}

	// Phase 1: tagged deployers are bunkerable regardless of the locomotor
	// gates in this function and in Phobos' 0x70FB73 hook (which the deployed
	// Build #48 applies BEFORE BunkerableAnyway). Keep the vanilla
	// Bunkerable= and parasite requirements.
	if (pType->Bunkerable && !pThis->ParasiteEatingMe
		&& BunkerTags::DeployToEnter(pType))
	{
		R->EAX(1);
		return RetTrue;
	}

	return 0;
}

// --- Probe D: what the building-side enter helper decides after a positive --
// --- IsBunkerableNow. -------------------------------------------------------
// 0x43C52C sits right after `ReceiveCommand(QueryOnBuilding, unit)` in the
// helper at 0x43C4xx (caller of IsBunkerableNow at 0x43C512). Vanilla:
// answer == AnswerPositive (already on top) -> 0x43CB68, else the helper
// returns 1 ("may enter"). Stolen bytes are cmp+je (9 bytes) — a relative
// branch, so this hook must NEVER return 0; both paths are explicit.
DEFINE_HOOK(0x43C52C, BuildingClass_EnterHelper_QueryOnBuilding_BunkerExtProbe, 0x9)
{
	enum { OnTopPath = 0x43CB68, MayEnterPath = 0x43C535 };

	GET(BuildingClass*, pThis, ESI);
	GET(TechnoClass*, pUnit, EDI);
	GET(int, answer, EAX);

	static TechnoClass* lastUnit = nullptr;
	static int lastFrame = -100000;
	const int frame = Unsorted::CurrentFrame;
	if (pUnit != lastUnit || frame - lastFrame > 150)
	{
		lastUnit = pUnit;
		lastFrame = frame;
		Debug::Log("[BunkerExt] f%d %s QueryOnBuilding(%s) = %d -> %s\n",
			frame, pThis->Type->ID, BunkerProbe::IdOf(pUnit),
			answer, answer == 1 ? "onTop-path" : "MAY-ENTER");
	}

	return answer == 1 ? OnTopPath : MayEnterPath;
}

// --- Probe B + Phase 1 adoption: bunker-side state watcher. -----------------
// 0x458E99 = BuildingClass::UpdateBunker, state dispatch. Stolen bytes (6):
//   8b 86 18 07 00 00  (mov eax,[esi+0x718])  -- idempotent read, clean.
DEFINE_HOOK(0x458E99, BuildingClass_UpdateBunker_BunkerExtProbe, 0x6)
{
	GET(BuildingClass*, pThis, ESI);

	BunkerProbe::Banner();

	auto* mem = BunkerProbe::Track(pThis);

	if (mem)
	{
		const int state = static_cast<int>(pThis->TankBunkerState);
		auto const link = pThis->BunkerLinkedItem
			? pThis->BunkerLinkedItem : BunkerProbe::FirstLink(pThis);

		if (state != mem->state || link != mem->link)
		{
			Debug::Log(
				"[BunkerExt] f%d UpdateBunker(%s @%p): state %d -> %d,"
				" link %s, bunkerLinked=%d\n",
				Unsorted::CurrentFrame, pThis->Type->ID, pThis,
				mem->state, state, BunkerProbe::IdOf(link),
				pThis->BunkerLinkedItem != nullptr);
			mem->state = state;
			mem->link = link;
		}
	}

	return 0;
}

// --- Phase 1b: adoption at UpdateBunker ENTRY. ------------------------------
// 0x458E50 = BuildingClass::UpdateBunker entry (ECX=this). Stolen bytes (5):
//   83 ec 78 / 53 / 55  (sub esp,0x78; push ebx; push ebp)  -- clean.
//
// This must run at the ENTRY: with no radio link and no BunkerLinkedItem the
// function bails out before the state dispatch, so a hook at 0x458E99 never
// sees a linkless bunker (run-3 lesson). Setting BunkerLinkedItem here means
// the pre-switch code picks the unit up as the link in this very call, and
// the Idle handler captures it.
//
// Vanilla vetoes force the pull-in design: a jumpjet refuses destinations on
// a building and DeployToLand shuffles to clear ground, so the unit can
// never legally stand ON the foundation. Instead it deploys NEXT to the
// bunker and gets teleported in (loco occupation bits up -> SetLocation ->
// bits down), matching where the MGTK trace showed the captured unit rests.
DEFINE_HOOK(0x458E50, BuildingClass_UpdateBunker_BunkerExtAdopt, 0x5)
{
	GET(BuildingClass*, pThis, ECX);

	auto* mem = BunkerProbe::Track(pThis);
	const int frame = Unsorted::CurrentFrame;
	const int range = BunkerTags::DeployCaptureRange(pThis->Type);

	// Unconditional entry diagnostic (throttled): proves the hook fires at all
	// and reports exactly which early-out (if any) suppresses the scan. This
	// exists because "scan sees" never printed across six runs.
	if (mem && frame - mem->lastEntryLogFrame >= 120)
	{
		mem->lastEntryLogFrame = frame;
		Debug::Log("[BunkerExt] f%d %s adopt-entry: state=%d linked=%d firstLink=%d range=%d\n",
			frame, pThis->Type->ID, static_cast<int>(pThis->TankBunkerState),
			pThis->BunkerLinkedItem != nullptr,
			BunkerProbe::FirstLink(pThis) != nullptr, range);
	}

	// Availability is Idle + no BunkerLinkedItem ONLY. A tank bunker keeps a
	// non-null RadioLinks[0] even when empty (run-9: state=0 linked=0
	// firstLink=1), so gating on FirstLink wrongly suppressed the scan on
	// every empty-bunker frame — that was the silent early-out.
	if (pThis->TankBunkerState != ::TankBunkerState::Idle
		|| pThis->BunkerLinkedItem)
		return 0;

	if (range <= 0)
		return 0;

	if (!mem || frame - mem->lastAdoptScanFrame < 10)
		return 0;
	mem->lastAdoptScanFrame = frame;

	// Global-array scan, NOT cell-object chains: air units (and jumpjets in
	// several states) are absent from CellClass::FirstObject, which made the
	// cell-based scan silently blind (run-4 lesson).
	const int w = pThis->Type->GetFoundationWidth();
	const int maxDist = w * 128 + range * 256 + 128; // leptons from center

	for (auto const pUnit : UnitClass::Array)
	{
		if (pUnit->InLimbo || pUnit->BunkerLinkedItem
			|| !BunkerTags::DeployToEnter(pUnit->GetTechnoType()))
			continue;

		const int dist = pUnit->DistanceFrom(pThis);
		const bool sameSide = pUnit->Owner == pThis->Owner
			|| (pThis->Owner && pThis->Owner->IsAlliedWith(pUnit->Owner));

		// Unconditional visibility: every tagged unit anywhere, with the exact
		// reason it is or isn't eligible. This is the diagnostic that tells us
		// whether owner or distance is the blocker (run-5 found nothing).
		if (mem && frame - mem->lastScanLogFrame >= 120)
		{
			mem->lastScanLogFrame = frame;
			Debug::Log(
				"[BunkerExt] f%d %s scan sees %s: dist=%d (max=%d) sameSide=%d"
				" deployed=%d deploying=%d inAir=%d\n",
				frame, pThis->Type->ID, BunkerProbe::IdOf(pUnit), dist, maxDist,
				sameSide, pUnit->Deployed, pUnit->Deploying, pUnit->IsInAir());
		}

		if (!sameSide || dist > maxDist)
			continue;

		if (pUnit->Deployed)
		{
			// Teleport onto the pad, then link; the Idle handler finishes
			// (RaiseWalls) on this same UpdateBunker call.
			auto dest = pThis->GetCoords();
			dest.X += 128;
			dest.Y += 128;

			if (pUnit->Locomotor)
				pUnit->Locomotor->Mark_All_Occupation_Bits(MarkType::Up);
			pUnit->SetLocation(dest);
			if (pUnit->Locomotor)
				pUnit->Locomotor->Mark_All_Occupation_Bits(MarkType::Down);

			pThis->BunkerLinkedItem = pUnit;
			Debug::Log("[BunkerExt] f%d %s pulled in deployed %s (inAir=%d)\n",
				frame, pThis->Type->ID, BunkerProbe::IdOf(pUnit), pUnit->IsInAir());
			return 0;
		}

		// A tagged deployer that stopped next to the bunker (ordered in via
		// the enter click, or just parked): give it the deploy order; the
		// pull-in adopts it once it lands deployed.
		const bool moving = pUnit->Locomotor && pUnit->Locomotor->Is_Moving();
		if (!pUnit->Deploying && !moving)
		{
			if (frame - mem->lastDeployCmdFrame >= 45)
			{
				mem->lastDeployCmdFrame = frame;
				pUnit->QueueMission(Mission::Unload, true);
				Debug::Log("[BunkerExt] f%d %s auto-deploy order to %s (inAir=%d)\n",
					frame, pThis->Type->ID, BunkerProbe::IdOf(pUnit), pUnit->IsInAir());
			}
		}
	}

	return 0;
}

// --- Phase 1f: UNIT-side per-frame capture driver in UnitClass::Update. -----
// Every building-side hook failed for the empty-bunker case:
//   - UpdateBunker (0x458E50) is mission-gated — never ticks for an idle bunker.
//   - 0x43FE98 is behind a per-building branch (jne 0x43FEBE) — never reached.
//   - 0x43FB23 (BuildingClass::AI entry) is RAW-PATCHED back to original bytes
//     by Phobos (Phobos.cpp:271, Patch::Apply_RAW) at startup, which clobbers
//     the Syringe JMP and silently un-hooks us.
// UnitClass::Update (0x7360C0, vtable slot 0x7F5CCC) runs every frame for every
// unit, is unclaimed, and is not raw-patched (Phobos hooks live inside it at
// 0x736234). Drive capture from the deployed unit: find a nearby empty friendly
// Bunker=yes building, teleport onto its pad, link both ways, and call
// UpdateBunker() directly so our Idle-capture hook finalizes (walls up).
// Stolen bytes (5): 83 ec 20 53 55 (sub esp,0x20; push ebx; push ebp) — clean,
// ECX=unit at entry (mov esi,ecx is at 0x7360C7, after the stolen window).
DEFINE_HOOK(0x7360C0, UnitClass_Update_BunkerExtCapture, 0x5)
{
	GET(UnitClass*, pUnit, ECX);

	if (pUnit->InLimbo)
		return 0;

	// Already linked: drive our bunker's state machine to completion (the
	// game's mission tick is unreliable for a directly-linked occupant), then
	// stay out of the way once Bunkered.
	if (pUnit->BunkerLinkedItem)
	{
		// Keep a bunkered jumpjet grounded EVERY frame (including after it is
		// fully Bunkered): the loco's Process runs later in this same Update
		// and re-raises flight height unless its State is pinned to Grounded.
		pUnit->InAir = false;
		if (auto const pJJ = locomotion_cast<JumpjetLocomotionClass*>(pUnit->Locomotor))
		{
			pJJ->State = JumpjetLocomotionClass::State::Grounded;
			pJJ->CurrentHeight = 0;
			pJJ->IsMoving = false;
		}

		if (auto const pBld = abstract_cast<BuildingClass*>(pUnit->BunkerLinkedItem))
			if (pBld->TankBunkerState != ::TankBunkerState::Bunkered)
				pBld->UpdateBunker();
		return 0;
	}

	// INTENT capture, two phases. The player orders a tagged deployer in via
	// the Enter cursor (0x74022D), which issues Mission::Enter toward the
	// bunker; the jumpjet flies over and hovers near the centre but can't
	// finish the vanilla (drive-track) entry. A plain deploy near a bunker
	// never captures — intent only — so it can't fire by accident.
	if (!BunkerTags::DeployToEnter(pUnit->GetTechnoType()))
		return 0;

	const int frame = Unsorted::CurrentFrame;

	// Foundation geometric centre (a building's GetCoords is its TARGET coord,
	// not its centre: compute from cell origin + half the foundation).
	auto bunkerCentre = [](BuildingClass* pB) {
		auto const tl = pB->GetMapCoords();
		auto c = pB->GetCoords();
		c.X = tl.X * 256 + pB->Type->GetFoundationWidth() * 128;
		c.Y = tl.Y * 256 + pB->Type->GetFoundationHeight(false) * 128;
		return c;
	};

	// Pin a unit grounded at a coord (used while the manual deploy anim plays).
	auto groundAt = [](UnitClass* pU, const CoordStruct& c) {
		if (pU->Locomotor)
			pU->Locomotor->Mark_All_Occupation_Bits(MarkType::Up);
		pU->SetLocation(c);
		if (pU->Locomotor)
			pU->Locomotor->Mark_All_Occupation_Bits(MarkType::Down);
		pU->InAir = false;
		pU->SetHeight(0);
		if (auto const pJJ = locomotion_cast<JumpjetLocomotionClass*>(pU->Locomotor))
		{
			pJJ->State = JumpjetLocomotionClass::State::Grounded;
			pJJ->CurrentHeight = 0;
			pJJ->IsMoving = false;
		}
	};

	auto faceSouth = [](UnitClass* pU) {
		pU->PrimaryFacing.SetCurrent(DirStruct(DirType::South));
		pU->SecondaryFacing.SetCurrent(DirStruct(DirType::South)); // turret
	};

	// --- Phase B: arrival detected -> land the unit on the pad centre and play
	// its deploy animation THERE, manually. The engine refuses to deploy a unit
	// onto a building's own foundation (it shuffles off / flies back up), so we
	// ground the unit dead-centre, hide its voxel (draw hook, via IsHidden),
	// spawn its DeployingAnim so the animation plays alone on the pad, and
	// finalize after the animation's own duration. No shuffle, no side-landing.
	if (!BunkerProbe::FindIntent(pUnit) && pUnit->CurrentMission == Mission::Enter)
	{
		auto const pBld = abstract_cast<BuildingClass*>(pUnit->Destination);
		if (pBld && pBld->Type->Bunker && !pBld->BunkerLinkedItem && !pBld->InLimbo
			&& pBld->TankBunkerState == ::TankBunkerState::Idle
			&& pUnit->Owner == pBld->Owner
			&& !pUnit->Deployed && !pUnit->Deploying
			&& pUnit->DistanceFrom(pBld) <= pBld->Type->GetFoundationWidth() * 128 + 256)
		{
			auto const centre = bunkerCentre(pBld);
			pUnit->SetDestination(nullptr, false);
			pUnit->QueueMission(Mission::Guard, false); // stop the enter/fly retry
			groundAt(pUnit, centre);
			faceSouth(pUnit);

			int dur = 30;
			if (auto const pAnimType = pUnit->Type->DeployingAnim)
			{
				GameCreate<AnimClass>(pAnimType, centre, 0, 1, 0x600, 0, false);
				const int rate = pAnimType->Rate > 0 ? pAnimType->Rate : 1;
				dur = pAnimType->End > 0 ? pAnimType->End * rate : 30;
				if (dur < 15) dur = 15;
				if (dur > 120) dur = 120;
			}

			BunkerProbe::AddIntent(pUnit, pBld, frame + dur); // hidden = true
			Debug::Log("[BunkerExt] f%d IntentAnim %s -> %s (centred, %d frames, voxel hidden)\n",
				frame, BunkerProbe::IdOf(pUnit), pBld->Type->ID, dur);
		}
		return 0;
	}

	// --- Phase A: deploy animation playing on the pad -> hold the (hidden)
	// unit grounded at centre until the timer elapses, then reveal it as the
	// deployed form facing South and finalize (link, raise walls over it).
	if (auto const pIntent = BunkerProbe::FindIntent(pUnit))
	{
		auto const pBld = pIntent->bld;
		if (!pBld->Type->Bunker || pBld->BunkerLinkedItem || pBld->InLimbo
			|| pBld->TankBunkerState != ::TankBunkerState::Idle)
		{
			BunkerProbe::RemoveIntent(pUnit);
			return 0;
		}

		auto const centre = bunkerCentre(pBld);

		if (frame < pIntent->doneFrame)
		{
			groundAt(pUnit, centre); // keep it pinned, voxel stays hidden
			faceSouth(pUnit);
			return 0;
		}

		// Animation finished -> reveal the deployed/siege form, facing South.
		pIntent->hidden = false;
		pUnit->Deployed = true;
		pUnit->Deploying = false;
		groundAt(pUnit, centre);
		faceSouth(pUnit);
		pBld->BunkerLinkedItem = pUnit;
		pUnit->BunkerLinkedItem = pBld;
		pBld->TankBunkerState = ::TankBunkerState::Idle;
		BunkerProbe::RemoveIntent(pUnit);
		Debug::Log("[BunkerExt] f%d IntentReveal %s -> %s, raising walls\n",
			frame, BunkerProbe::IdOf(pUnit), pBld->Type->ID);
		pBld->UpdateBunker();
	}

	return 0;
}

// --- Hide the voxel while the deploy animation plays on the pad. -----------
// 0x73CF62 is UnitClass::DrawIt's "draw this unit" path (the Continue target of
// Phobos' KeepUnitVisible hook at 0x73CF46; ESI=unit, frame set up). For a unit
// whose deploy anim is mid-play we jump to DoNotDraw (0x73D43F, the function's
// clean epilogue) so only the animation shows — no voxel on top. Chained after
// Kratos(0x73CF16)/Phobos(0x73CF46); those decide to draw, we veto for our
// hidden units. Stolen bytes (6): 8b 0d 24 73 88 00 (mov ecx,[0x887324]).
DEFINE_HOOK(0x73CF62, UnitClass_DrawIt_BunkerExtHideVoxel, 0x6)
{
	enum { DoNotDraw = 0x73D43F };
	GET(UnitClass*, pThis, ESI);
	return BunkerProbe::IsHidden(pThis) ? DoNotDraw : 0;
}

// --- Phase 1d: lift the MovementZone=Fly veto on building-enter actions. ----
// 0x74018D, in UnitClass::GetActionOnObject: vanilla forces Action::NoEnter
// (31) for any unit with MovementZone=Fly right AFTER the target building
// approved the enter (run-4 telemetry: SCHP baseAction=31 while MTNK got
// Enter=3 through the same approvals). Tagged units skip the veto and
// continue down the same path that grants land units Action::Enter.
// Stolen bytes (6): 8b 86 c4 06 00 00 (mov eax,[esi+0x6c4]) — idempotent.
DEFINE_HOOK(0x74018D, UnitClass_GetActionOnObject_BunkerExtFlyVeto, 0x6)
{
	enum { SkipFlyVeto = 0x7401A3 };

	GET(TechnoClass*, pThis, ESI);

	if (BunkerTags::DeployToEnter(pThis->GetTechnoType()))
		return SkipFlyVeto;

	return 0;
}

// --- Phase 1c: grant the enter UX (Action::Move) for tagged deployers. ------
// 0x74022D sits in UnitClass::GetActionOnObject's own-building override,
// after the human-control check. Vanilla only converts action Select(7) into
// Move(1) for Bunker=yes targets after the unit's ReceiveCommand
// (QueryCanEnter) answers positive and the bunker is link-free — checks with
// their own inline copy of the bunkerable rules, which is where jumpjets die
// without ever reaching IsBunkerableNow. For tagged units we grant the Move
// action directly (bunker must be empty); everyone else replicates vanilla.
// Stolen bytes are cmp+jne (5) — relative branch, so NEVER return 0.
DEFINE_HOOK(0x74022D, UnitClass_GetActionOnObject_BunkerExtEnterUX, 0x5)
{
	enum { SelectBranch = 0x740232, NotSelect = 0x74027C,
	       FinalizeAction = 0x74037A }; // common tail; EBX=action -> cursor+return

	GET(TechnoClass*, pThis, ESI);
	GET(ObjectClass*, pTarget, EDI);
	GET(int, action, EBX);

	if (pTarget->WhatAmI() == AbstractType::Building)
	{
		auto const pBld = static_cast<BuildingClass*>(pTarget);

		// Grant the real Enter cursor for a tagged deployer over its own
		// empty bunker. The jumpjet MovementZone=Fly vetoes (three of them in
		// this function) deny Enter(3) naturally; force it and route to the
		// action-finalize tail, which turns EBX into the Enter cursor and
		// returns it (click then issues Mission::Enter toward the bunker).
		if (pBld->Type->Bunker && !pBld->BunkerLinkedItem
			&& pThis->Owner == pBld->Owner
			&& BunkerTags::DeployToEnter(pThis->GetTechnoType()))
		{
			static TechnoClass* lastPair = nullptr;
			if (pThis != lastPair)
			{
				lastPair = pThis;
				Debug::Log("[BunkerExt] f%d EnterUX(%s over %s): force Enter (was %d)\n",
					Unsorted::CurrentFrame, BunkerProbe::IdOf(pThis),
					pBld->Type->ID, action);
			}
			R->EBX(3); // Action::Enter
			return FinalizeAction;
		}
	}

	return action == 7 ? SelectBranch : NotSelect;
}

// --- Probe C + Phase 1 capture: the Idle state. -----------------------------
// 0x458EAF = UpdateBunker's Idle handler; only reached with a non-null link
// (EBP) that already passed WhatAmI()==Unit. Stolen bytes (5):
//   8b 55 00 / 8b cd  (mov edx,[ebp+0]; mov ecx,ebp)  -- clean.
// The original then checks (a) unit's radio contact resolves to this
// building, (b) locomotor->Is_Moving() == false, and starts force-tracking —
// drive-locomotor mechanics a jumpjet cannot perform (that failure mode is
// exactly why newer Phobos blocklists the Jumpjet locomotor). Tagged units
// therefore never reach the vanilla Idle body.
DEFINE_HOOK(0x458EAF, BuildingClass_UpdateBunker_Idle_BunkerExtProbe, 0x5)
{
	enum { SkipVanillaIdle = 0x459374 }; // UpdateBunker common epilogue

	GET(BuildingClass*, pThis, ESI);
	GET(FootClass*, pFoot, EBP);

	auto* mem = BunkerProbe::Track(pThis);
	const int frame = Unsorted::CurrentFrame;

	if (mem && frame - mem->lastIdleLogFrame >= 30)
	{
		mem->lastIdleLogFrame = frame;

		const bool moving = pFoot->Locomotor
			&& pFoot->Locomotor->Is_Moving();
		auto const unitRadio = BunkerProbe::FirstLink(pFoot);
		auto const unitCoords = pFoot->GetCoords();
		auto const bldCoords = pThis->GetCoords();

		Debug::Log(
			"[BunkerExt] f%d Idle(%s): unit=%s mission=%d moving=%d"
			" radioBack=%s height=%d dxy=(%d,%d)\n",
			frame, pThis->Type->ID, BunkerProbe::IdOf(pFoot),
			static_cast<int>(pFoot->CurrentMission), moving,
			unitRadio == pThis ? "this"
				: (unitRadio ? unitRadio->GetTechnoType()->ID : "<none>"),
			unitCoords.Z - bldCoords.Z,
			unitCoords.X - bldCoords.X, unitCoords.Y - bldCoords.Y);
	}

	// Phase 1: deploy-to-enter flow for tagged units. The pre-switch code
	// already guaranteed pFoot is a UnitClass.
	if (BunkerTags::DeployToEnter(pFoot->GetTechnoType()))
	{
		auto const pUnit = static_cast<UnitClass*>(pFoot);

		if (pUnit->Deployed && !pUnit->IsInAir())
		{
			// Landed and deployed on the bunker. Skip the track states (2,3 —
			// a jumpjet can't drive-track; the unit is already centered), but
			// route through RotateInBunker(4), NOT straight to RaiseWalls(5):
			// state 4 is what actually PLAYS the walls-up animation (run-14:
			// jumping 0->5 set state=Bunkered but left the walls down). Pin the
			// body facing to South with SetCurrent so state 4's IsRotating
			// check passes at once (a deployed SCHP won't rotate its hull).
			pThis->BunkerLinkedItem = pUnit;
			pUnit->BunkerLinkedItem = pThis;
			pUnit->PrimaryFacing.SetCurrent(DirStruct(DirType::South));

			// Ground the unit so it draws inside the walls, not above them. A
			// jumpjet keeps a flight height (JumpjetLocomotionClass::CurrentHeight)
			// that we skipped zeroing by bypassing the track states; left set it
			// floats over the walls AND, via iso projection, looks off-centre.
			pUnit->InAir = false;
			pUnit->SetHeight(0);
			if (auto const pJJ = locomotion_cast<JumpjetLocomotionClass*>(pUnit->Locomotor))
			{
				pJJ->State = JumpjetLocomotionClass::State::Grounded;
				pJJ->CurrentHeight = 0;
				pJJ->IsMoving = false;
			}

			pThis->TankBunkerState = ::TankBunkerState::RotateInBunker;
			Debug::Log("[BunkerExt] f%d %s captured deployed %s -> RotateInBunker\n",
				frame, pThis->Type->ID, BunkerProbe::IdOf(pUnit));
		}
		else if (!pUnit->Deploying)
		{
			auto const unitCoords = pUnit->GetCoords();
			auto const bldCoords = pThis->GetCoords();
			const int dx = unitCoords.X - bldCoords.X;
			const int dy = unitCoords.Y - bldCoords.Y;

			// Hovering near the bunker: order the deploy (DeployToLand lands
			// it). Rate-limited; the order also drops the radio link, after
			// which the adoption path above takes over.
			if (dx * dx + dy * dy <= 512 * 512
				&& mem && frame - mem->lastDeployCmdFrame >= 45)
			{
				mem->lastDeployCmdFrame = frame;
				pUnit->QueueMission(Mission::Unload, true);
				Debug::Log("[BunkerExt] f%d %s ordered deploy of %s (dxy %d,%d)\n",
					frame, pThis->Type->ID, BunkerProbe::IdOf(pUnit), dx, dy);
			}
		}

		return SkipVanillaIdle;
	}

	return 0;
}
