#pragma once

#include <cstdint>
#include <string>

// The listener: counters on the VM's own calls, a stack walk only at the interval and during overstress, and the report.
namespace Monitor
{
	struct Settings
	{
		float intervalSeconds = 60.0F;         // a report this often
		float overstressGapSeconds = 10.0F;    // at least this long between reports when overstress begins
		float overstressSampleSeconds = 2.0F;  // a stack walk this often while overstress is on
		int maxStacksPerWalk = 4000;           // a walk stops here, and says so
		float maxWalkMS = 4.0F;                // or here
		int topRows = 10;                      // rows in each top list
	};

	void Configure(const Settings& a_settings);
	void InstallHooks();    // at plugin load: vtable writes only, nothing else is touched until data has loaded
	void OnDataLoaded();    // main thread: remembers the thread, checks the VM's layout, joins the stats event
	void OnGameLoaded();    // a save loaded or a new game began: reports start
	void RequestReport();   // DevBench: a report at the next VM update
	std::string StateJson();

	// For the hang recorder's watcher: atomics only, so it costs no lock and works whatever the VM is doing.
	struct HangSnapshot
	{
		std::uint64_t vmUpdates;
		std::uint32_t running;
		std::uint32_t suspended;
		std::uint32_t messages;
		std::uint32_t detached;
		bool overstressed;
		float budgetMS;
		std::uint32_t updateThread;       // the thread inside the VM's update now, else 0
		double updateRunningMS;
		std::uint32_t lastUpdateThread;
		double sinceUpdateEndedMS;
		int taskletsActive;
		std::uint32_t lastTaskletThread;
		std::uint32_t tasksToJobs;
		std::uint32_t updatesOffMain;
		std::uint32_t taskletsOffMain;
	};
	HangSnapshot GetHangSnapshot();
}
