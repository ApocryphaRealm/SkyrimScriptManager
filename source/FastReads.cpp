// Management stage 1, off by default: read-only native calls answered without waiting for the frame. GPL-3.0-or-later (2026-11-04).
//
// A script that calls a native which is not marked callable from tasklets gives up its turn until the VM's next function-message
// pass, so even a read costs a frame or more under load. The VM has a public call for that mark, SetCallableFromTasklets, which
// is what this uses: no hook, and nothing runs that the game would not run itself.
//
// Papyrus Tweaks' bSpeedUpNativeCalls is the same idea from the other side: it hooks the check and lets every native through
// except a block list by class and name prefix, and serialises those calls with a mutex. This is an allow list of reads of
// records that do not change during play, so it needs no mutex. If that option is on, this stands aside.
#include "PCH.h"

#include "FastReads.h"

#include <fstream>
#include <string>

namespace FastReads
{
	namespace
	{
		bool PapyrusTweaksSpeedUpIsOn()
		{
			std::ifstream file("Data/SKSE/Plugins/PapyrusTweaks.ini");
			if (!file) { return false; }
			for (std::string line; std::getline(file, line);)
			{
				std::string compact;
				for (const char c : line)
				{
					if (c != ' ' && c != '\t' && c != '\r') { compact += static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
				}
				if (compact.starts_with("bspeedupnativecalls=")) { return compact.ends_with("=true") || compact.ends_with("=1"); }
			}
			return false;
		}

		struct Name
		{
			const char* type;
			const char* function;
		};

		// Reads of a record's own data. Nothing about a live actor's position, state or inventory: those change under a job thread.
		constexpr Name kReads[] = {
			{ "Form", "GetFormID" }, { "Form", "GetType" }, { "Form", "HasKeyword" }, { "Form", "GetName" }, { "Form", "GetWeight" }, { "Form", "GetGoldValue" }, { "Form", "GetNumKeywords" }, { "Form", "GetNthKeyword" },
			{ "ObjectReference", "GetBaseObject" },
			{ "ActorBase", "GetSex" }, { "ActorBase", "GetRace" }, { "ActorBase", "IsUnique" },
			{ "ActiveMagicEffect", "GetBaseObject" },
			{ "MagicEffect", "GetCastingType" }, { "MagicEffect", "GetAssociatedSkill" }, { "MagicEffect", "GetArea" },
			{ "Keyword", "GetString" },
		};
	}

	void Apply(bool a_enabled)
	{
		if (!a_enabled)
		{
			logger::info("Fast reads: off (bFastReads=0), as shipped");
			return;
		}
		if (PapyrusTweaksSpeedUpIsOn())
		{
			logger::warn("Fast reads: standing aside, Papyrus Tweaks' bSpeedUpNativeCalls is on and already covers these calls");
			return;
		}
		auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm)
		{
			logger::error("Fast reads: no VM, nothing done");
			return;
		}
		int count = 0;
		for (const auto& read : kReads)
		{
			vm->SetCallableFromTasklets2(read.type, read.function, true);
			logger::debug("Fast reads: {}.{}", read.type, read.function);
			++count;
		}
		logger::info("Fast reads: ON for {} read-only native calls. These now answer without waiting for the frame.", count);
	}
}
