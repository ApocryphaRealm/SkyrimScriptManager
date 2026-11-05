# SkyrimScriptManager - changelog

Written as changes happen, not reconstructed afterwards (rule 61). Each version carries its
**version-ledger status**, so this file cannot quietly claim more than the ledger does:

* **working** - observed running in game
* **untested** - built and packaged, not yet confirmed
* **failed** - built but crashed or malfunctioned; the number was reclaimed
* **scratch** - a hypothesis-test build that never held a real number

## 1.0.0 - 2026-11-04 - untested

### Added
- The hang recorder, on by default (bHangRecorder, fStallSeconds=15): when no frame comes for that long, a thread of its own that takes no game lock writes the main thread's stack, the stacks of the other threads in the game and in SKSE plugins, the recorded read calls in flight and the last 64 of them, the load state, the VM's state and which of its locks are held to the log, and again 60 s later if it lasts. It records the calls bFastReads marks whether that setting is on or off, noting the route of each, so a run with it off is a control. It logs the thread ids at start. DevBench op=stall proves it without a real freeze.
- The Skyrim 1.7.x build line, on CommonLibSSE-NG 7.2.0.
- DevBench op=flood and op=floodstop repeat an event on a test actor from the main thread, to prove a detection without waiting on luck. The tool is no longer read-only.
- The loop guard, off by default (bLoopGuard): on a caught actor while the VM is overstressed, drops repeated OnEffectStart with its OnEffectFinish, keeping each effect's pairs balanced, and delivers a last start to any effect still running at release or a save. Any other event on a held effect delivers the start first. No other event is ever dropped. It turns itself off on any fault.
- The loop watch, on by default, report only: one object taking the same event at an abnormal rate for seconds on end, per object and event at SendEvent in a fixed table; an effect's OnEffectStart and OnEffectFinish count for its actor. Also stacks on one object at a walk, recursion on waiting stacks, and actor attach or detach and 3D load or unload rates from the game's public events. Every detection logs the object, form, base, plugin, event, rate, its scripts with roles, what was done, and a cause reading. Each report lists the period's busiest rows, so thresholds can be checked against real play.
- The listener: a report every 60 s and when overstress begins, with FPS, frame times, overstressed frames, VM time and budget, stack counts from the VM's stats event, new stacks per second, events sent and to whom, what began the waiting stacks, what they wait on, how long the longest has been seen waiting, and each script's role. A stack walk only at a report and during overstress, capped by stacks and time. DevBench tool `scriptmanager.state`.
- The classifier: reads each loaded script on a low-priority thread and judges each script and event as may drop, may wait or never touch from what it calls, unsure being never touch. Writes `SkyrimScriptManager_Roles.txt` with the deciding call. `SkyrimScriptManager_Overrides.ini` ships empty.
- The check: a labelled script seen on a stack calling what its code was not found to call gets a `ROLE ERROR` line and turns never touch, kept in `SkyrimScriptManager_Flips.txt`.
- Management stage 1, off by default: `bFastReads` marks a short list of read-only native calls callable from tasklets, using the VM's own call. Stands aside if Papyrus Tweaks' `bSpeedUpNativeCalls` is on.

### Changed
- Renamed Skyrim Script Manager, before any release: plugin, log, INI, Roles, Overrides and Flips files are SkyrimScriptManager, the DevBench tool is scriptmanager.state. The repo and its folder keep the name ScriptLoadMonitor for now.
### Fixed
- Reports ran only when the VM's update was on one thread, so a list that moves it between threads got almost none. Reports now run wherever the update runs, one tick at a time.
- A stack whose effect had already ended now counts for the effect's target, read from the start's first parameter, instead of for no one: that was nearly every stack in the flood. Reading a script object from a stack no longer copies its smart pointer, which wrote a refcount through an unchecked value.