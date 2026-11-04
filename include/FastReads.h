#pragma once

// Management stage 1: read-only native calls answered without the frame wait. Off unless bFastReads=1.
namespace FastReads
{
	void Apply(bool a_enabled);  // main thread, once natives are bound
}
