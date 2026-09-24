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
#include <UnitClass.h>
#include <FootClass.h>
#include <TechnoTypeClass.h>
#include <BuildingTypeClass.h>
#include <CellClass.h>
#include <MapClass.h>
#include <Fundamentals.h>

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

	// YRpp's GetNthLink is an unchecked Items[idx] read; the game's own
	// 0x65AD30 bounds-checks. Guard so an empty link vector can't fault us.
	static TechnoClass* FirstLink(RadioClass* pRadio)
	{
		return pRadio->RadioLinks.Capacity > 0 ? pRadio->RadioLinks[0] : nullptr;
	}

	// A deployed, grounded, unlinked, tagged unit — the post-deploy shape a
	// jumpjet deployer has after our Mission::Unload order landed it.
	static UnitClass* AdoptableUnit(ObjectClass* pFirst)
	{
		for (auto pObj = pFirst; pObj; pObj = pObj->NextObject)
		{
			if (pObj->WhatAmI() != AbstractType::Unit)
				continue;
			auto const pUnit = static_cast<UnitClass*>(pObj);
			if (pUnit->Deployed && !pUnit->IsInAir()
				&& !pUnit->BunkerLinkedItem
				&& BunkerTags::DeployToEnter(pUnit->GetTechnoType()))
			{
				return pUnit;
			}
		}
		return nullptr;
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

	if (pThis->TankBunkerState != ::TankBunkerState::Idle
		|| pThis->BunkerLinkedItem || BunkerProbe::FirstLink(pThis))
		return 0;

	const int range = BunkerTags::DeployCaptureRange(pThis->Type);
	if (range <= 0)
		return 0;

	auto* mem = BunkerProbe::Track(pThis);
	const int frame = Unsorted::CurrentFrame;
	if (!mem || frame - mem->lastAdoptScanFrame < 10)
		return 0;
	mem->lastAdoptScanFrame = frame;

	auto const base = pThis->GetMapCoords();
	const int w = pThis->Type->GetFoundationWidth();
	const int h = pThis->Type->GetFoundationHeight(false);

	for (int dx = -range; dx < w + range && dx < 3 + range; ++dx)
	{
		for (int dy = -range; dy < h + range && dy < 3 + range; ++dy)
		{
			auto const pCell = MapClass::Instance.TryGetCellAt(
				CellStruct { static_cast<short>(base.X + dx),
				             static_cast<short>(base.Y + dy) });
			if (!pCell)
				continue;
			auto const pUnit = BunkerProbe::AdoptableUnit(pCell->FirstObject);
			if (!pUnit)
				continue;

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
			Debug::Log("[BunkerExt] f%d %s pulled in deployed %s from (%d,%d)\n",
				frame, pThis->Type->ID, BunkerProbe::IdOf(pUnit), dx, dy);
			return 0;
		}
	}

	return 0;
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
			// Landed and deployed on the bunker: capture in place. Skip the
			// rotate/track states (2..4) and let vanilla state 5 raise the
			// walls and finalize, as in the MGTK trace.
			pThis->BunkerLinkedItem = pUnit;
			pUnit->BunkerLinkedItem = pThis;
			pThis->TankBunkerState = ::TankBunkerState::RaiseWalls;
			Debug::Log("[BunkerExt] f%d %s captured deployed %s -> RaiseWalls\n",
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
