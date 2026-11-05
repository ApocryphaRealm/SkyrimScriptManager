#pragma once

#include <span>

// Management stage 1: read-only native calls answered without the frame wait. Off unless bFastReads=1.
namespace FastReads
{
	void Apply(bool a_enabled);  // main thread, once natives are bound

	struct Name
	{
		const char* type;
		const char* function;
	};
	std::span<const Name> Reads();   // the list of calls, marked or not: the hang recorder records these
	const char* Status();            // what Apply did, for the hang recorder's report
}
