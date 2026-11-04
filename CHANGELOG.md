# ScriptLoadMonitor - changelog

Written as changes happen, not reconstructed afterwards (rule 61). Each version carries its
**version-ledger status**, so this file cannot quietly claim more than the ledger does:

* **working** - observed running in game
* **untested** - built and packaged, not yet confirmed
* **failed** - built but crashed or malfunctioned; the number was reclaimed
* **scratch** - a hypothesis-test build that never held a real number

## 1.0.0 - 2026-11-04 - untested

### Added
- The listener: a report every 60 s and when overstress begins, with FPS, frame times, overstressed frames, VM time and budget, stack counts from the VM's stats event, new stacks per second, events sent and to whom, what began the waiting stacks, what they wait on, how long the longest has been seen waiting, and each script's role. A stack walk only at a report and during overstress, capped by stacks and time. DevBench tool `scriptmonitor.state`.
- The classifier: reads each loaded script on a low-priority thread and judges each script and event as may drop, may wait or never touch from what it calls, unsure being never touch. Writes `ScriptLoadMonitor_Roles.txt` with the deciding call. `ScriptLoadMonitor_Overrides.ini` ships empty.
- The check: a labelled script seen on a stack calling what its code was not found to call gets a `ROLE ERROR` line and turns never touch, kept in `ScriptLoadMonitor_Flips.txt`.
- Management stage 1, off by default: `bFastReads` marks a short list of read-only native calls callable from tasklets, using the VM's own call. Stands aside if Papyrus Tweaks' `bSpeedUpNativeCalls` is on.
