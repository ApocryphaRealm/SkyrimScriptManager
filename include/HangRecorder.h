#pragma once

#include <string>

// The hang recorder, on by default: no frame for a while and it writes the threads' stacks, the read calls in flight, the last 64 of
// them, the load state and the VM's state to the log, from a thread of its own that takes no game lock. The core is HangCore.
namespace HangRecorder
{
	void Configure(bool a_enabled, float a_stallSeconds);
	void NoteInstallThread();   // at plugin load
	void OnDataLoaded();        // main thread, after FastReads::Apply: wraps the read calls, joins the menu event, starts the watcher
	void OnPreLoad();           // main thread: a save is about to load
	void OnGameLoaded();        // a save loaded or a new game began

	// DevBench. a_where "main": stall the main thread by sleeping in a queued task. "read": the next recorded read call sleeps inside the
	// recorder's wrapper, on whichever thread makes it. Either shows a report without waiting for a real freeze.
	void TestStall(const std::string& a_where, float a_seconds);

	std::string StateJsonFragment();
}
