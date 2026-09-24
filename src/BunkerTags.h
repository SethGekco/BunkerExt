#pragma once

// Lazy per-TechnoType INI tags, read from the in-memory rules INI
// (CCINIClass::INI_Rules, 0x887048 — stays loaded for the whole process, which
// is what makes lazy reads safe). Phase 1 deliberately reads rulesmd only; map
// / game-mode overrides can come later with a proper ext container.
//
// The cache key is the type pointer, which the game frees and reallocates
// between games — so each entry also stores the type ID and re-reads on
// mismatch instead of trusting a recycled pointer.

#include <CCINIClass.h>
#include <TechnoTypeClass.h>

#include <unordered_map>
#include <string>

namespace BunkerTags
{
	struct Entry
	{
		std::string id;
		bool deployToEnter;
	};

	// [<TypeID>] Bunker.DeployToEnter=yes
	// Jumpjet deployers (SCHP): the enter order makes them fly to the bunker,
	// land-deploy on it, and get captured in place.
	inline bool DeployToEnter(TechnoTypeClass* pType)
	{
		static std::unordered_map<TechnoTypeClass*, Entry> cache;

		auto const it = cache.find(pType);
		if (it != cache.end() && it->second.id == pType->ID)
			return it->second.deployToEnter;

		bool value = false;
		if (auto const pINI = CCINIClass::INI_Rules)
			value = pINI->ReadBool(pType->ID, "Bunker.DeployToEnter", false);

		cache[pType] = Entry { pType->ID, value };
		return value;
	}
}
