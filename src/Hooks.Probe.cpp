// ---------------------------------------------------------------------------
// Phase 0: tank-bunker entry probe.
//
// Question under test: with Bunkerable=yes + Phobos BunkerableAnyway=yes, a
// jumpjet unit (SCHP) still does not enter a Bunker=yes building. Where does
// the entry pipeline die?
//
// The pipeline, from reading gamemd with objdump (see DESIGN.md):
//
//   1. cursor/action    -- FootClass::IsBunkerableNow (0x70FB50) gates the
//                          enter action. Phobos hooks 0x70FB73 inside it;
//                          with BunkerableAnyway=yes it returns true unless
//                          the unit is parasitized.
//   2. radio handshake  -- the entering unit becomes the bunker's radio link
//                          (or is already its BunkerLinkedItem).
//   3. capture          -- BuildingClass::UpdateBunker (0x458E50) polls: takes
//                          first radio link, requires WhatAmI()==Unit, then a
//                          6-state machine (TankBunkerState 0..5, +6 =
//                          Bunkered). The Idle handler (0x458EAF) requires the
//                          unit's radio contact to resolve to this building
//                          AND locomotor->Is_Moving() == false before it
//                          starts force-tracking the unit to the center.
//
// Hypothesis: a jumpjet either never establishes the radio link (probe B
// shows no link) or hovers with Is_Moving()==true forever (probe C shows the
// stall). Either way the fix is a custom deploy-to-enter flow; this probe
// tells us which end to graft it onto.
//
// All three hook addresses verified against the hook registry: no other
// framework touches them. All stolen-byte windows checked against objdump
// output: no relative branches inside any window, so `return 0` is safe.
// ---------------------------------------------------------------------------

#include <BuildingClass.h>
#include <FootClass.h>
#include <TechnoTypeClass.h>
#include <BuildingTypeClass.h>
#include <Fundamentals.h>

#include <Syringe.h>
#include <Utilities/Debug.h>

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
}

// --- Probe A: who gets asked "is bunkerable now", and with what type flags. -
// 0x70FB50 = FootClass::IsBunkerableNow entry. Stolen bytes (5):
//   56 / 8b f1 / 8b 06  (push esi; mov esi,ecx; mov eax,[esi])  -- clean.
DEFINE_HOOK(0x70FB50, FootClass_IsBunkerableNow_BunkerExtProbe, 0x5)
{
	GET(FootClass*, pThis, ECX);

	BunkerProbe::Banner();

	const int frame = Unsorted::CurrentFrame;
	if (pThis != BunkerProbe::lastQueried
		|| frame - BunkerProbe::lastQueriedFrame > 150)
	{
		BunkerProbe::lastQueried = pThis;
		BunkerProbe::lastQueriedFrame = frame;

		auto const pType = pThis->GetTechnoType();
		Debug::Log(
			"[BunkerExt] f%d IsBunkerableNow(%s): Bunkerable=%d Turret=%d"
			" SpeedType=%d LocoData1=%08X Parasite=%d\n",
			frame, pType->ID,
			pType->Bunkerable, pType->Turret,
			static_cast<int>(pType->SpeedType),
			static_cast<unsigned int>(pType->Locomotor.Data1),
			pThis->ParasiteEatingMe != nullptr);
	}

	return 0;
}

// --- Probe B: does the bunker ever see a candidate, and how far does the ----
// --- state machine get. ----------------------------------------------------
// 0x458E99 = BuildingClass::UpdateBunker, state dispatch. Stolen bytes (6):
//   8b 86 18 07 00 00  (mov eax,[esi+0x718])  -- idempotent read, clean.
DEFINE_HOOK(0x458E99, BuildingClass_UpdateBunker_BunkerExtProbe, 0x6)
{
	GET(BuildingClass*, pThis, ESI);

	BunkerProbe::Banner();

	if (auto* mem = BunkerProbe::Track(pThis))
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

// --- Probe C: why the Idle state refuses to advance. ------------------------
// 0x458EAF = UpdateBunker's Idle handler; only reached with a non-null link
// (EBP) that already passed WhatAmI()==Unit. Stolen bytes (5):
//   8b 55 00 / 8b cd  (mov edx,[ebp+0]; mov ecx,ebp)  -- clean.
// The original then checks (a) unit's radio contact resolves to this
// building, (b) locomotor->Is_Moving() == false. Log both, throttled.
DEFINE_HOOK(0x458EAF, BuildingClass_UpdateBunker_Idle_BunkerExtProbe, 0x5)
{
	GET(BuildingClass*, pThis, ESI);
	GET(FootClass*, pUnit, EBP);

	if (auto* mem = BunkerProbe::Track(pThis))
	{
		const int frame = Unsorted::CurrentFrame;
		if (frame - mem->lastIdleLogFrame >= 30)
		{
			mem->lastIdleLogFrame = frame;

			const bool moving = pUnit->Locomotor
				&& pUnit->Locomotor->Is_Moving();
			auto const unitRadio = BunkerProbe::FirstLink(pUnit);
			auto const unitCoords = pUnit->GetCoords();
			auto const bldCoords = pThis->GetCoords();

			Debug::Log(
				"[BunkerExt] f%d Idle(%s): unit=%s mission=%d moving=%d"
				" radioBack=%s height=%d dxy=(%d,%d)\n",
				frame, pThis->Type->ID, BunkerProbe::IdOf(pUnit),
				static_cast<int>(pUnit->CurrentMission), moving,
				unitRadio == pThis ? "this"
					: (unitRadio ? unitRadio->GetTechnoType()->ID : "<none>"),
				unitCoords.Z - bldCoords.Z,
				unitCoords.X - bldCoords.X, unitCoords.Y - bldCoords.Y);
		}
	}

	return 0;
}
