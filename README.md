# Skyrim Script Manager

A plain SKSE plugin for Skyrim SE 1.5.97, AE 1.6 and 1.7 that listens to the Papyrus VM. Four parts:

- **The listener** writes `SkyrimScriptManager.log`, a short report every 60 s and when overstress begins: FPS and frame times, frames
  overstressed, the VM's time and budget, running, suspended and queued stacks, new stacks per second, events sent, what began the
  waiting stacks and which actor they are on, what they wait on, and how long the longest has been seen waiting.
- **The classifier** reads each script the game has loaded and judges each one, and each event in it, from its code: may drop, may
  wait or never touch. Anything unsure is never touch. `SkyrimScriptManager_Roles.txt` gives the deciding call for each. No table ships:
  it is worked out from the player's own scripts on each run.
- **The check**: a script labelled may drop or may wait that is seen on a stack calling something its code was not found to call
  gets an error line and turns never touch, and stays so through `SkyrimScriptManager_Flips.txt`.

- **The loop watch and guard**: see below. The watch is on and only reports; the guard is off.

As shipped it holds and drops nothing. `bFastReads` (off) is management stage 1. `bLoopGuard` (off) is the one thing that drops an event. Holding event floods by role is not built.

See `dist/README.txt` for what to install and `PRIOR-ART.md` for what already exists. The classifier builds outside the game too:
`tools\build-rolecheck.bat`, then `build\rolecheck\rolecheck.exe <folder of .pex> <out.tsv>`.

Version 1.0.0

## The loop watch

General, with no mod or actor named in it. It came from one case: 11 reindeer restarting FEC's and RASS's effects, which one-off patches stop only for that herd.

**Detects**, once a second, from a fixed table of 4096 rows filled at `SendEvent` without a lock:
- one object taking the same event at an abnormal rate for seconds on end. An effect's `OnEffectStart` and `OnEffectFinish` count for the actor it is on;
- stacks on one object at a stack walk;
- recursion: a stack too deep, or one function many times on it (waiting stacks only, at a walk, and only the first 64 frames);
- an actor's cell attach or detach and any object's 3D load or unload, from the game's public events, so a restart storm can be told apart by cause.

**Reports** in the log on every detection and at its end: `LOOP:` object (form, base, plugin, persistence, distance), event, rate, stacks on it, its scripts with their roles, and what was done. `LOOP cause:` gives the attach and 3D rates and the current package, and a reading. Each report also lists this period's three busiest rows even if nothing tripped, which is how the rates below get checked against real play.

**Acts**, only with `bLoopGuard=1`, only on a caught actor, only while the VM is overstressed when it catches on, until the rate falls. It drops repeats of one pair of events.

### What is safe to drop, per event

| Event | Guard | Why |
|---|---|---|
| `OnEffectStart` + `OnEffectFinish` on an effect | drops repeats, as a pair | See the pair rule. Of 9,313 start stacks in the flood's dumps, 8,903 had no object left to name: the effect ended before the start ran, so those stacks ran on nothing |
| `OnEffectFinish` to a start that was delivered | never | Cleanup is often here |
| any other event on an effect whose start is held | never | It delivers the held start first, then itself |
| `OnLoad`, `OnUnload`, `OnCellAttach`, `OnCellDetach`, `OnAttachedToCell`, `OnDetachedFromCell` | report only | A script may track loaded state, and there is no effect to re-check at the end, so the last state could not be restored |
| `OnUpdate` | report only | A `RegisterForSingleUpdate` chain re-arms inside the event: one drop stops that script's loop for good |
| `OnHit` and the rest | report only | Each carries its own data: who, with what. A repeat is not the same event |

### The pair rule

Per effect handle, while its actor is armed:
- a start on a handle not seen before goes through (a new spell on that actor is not a repeat);
- a start after a closed pair is dropped and marked held;
- a finish on a held handle is dropped and closes it: that pair never happened;
- a finish to a delivered start always goes through;
- at release, at a save, or if the guard turns off, a held handle whose effect still exists gets its start, rebuilt from the effect.

FEC's two scripts have no `OnEffectFinish` at all; RASS has one, and its starts and finishes stay paired under this rule.

So a script sees its first pair and, if its effect outlived the storm, one last start, and every start it saw has its finish. A script counting starts against finishes stays balanced.

That is the case for applying it to scripts labelled never touch: it removes only repeats of an event the script has had, and a start that matters is delivered late, not lost. **What it costs:** a script doing one-off work per start, with no finish (adding an item each time, say), does it fewer times, which in a storm is the point. A real new effect that reuses a handle on an armed actor counts as a repeat, and if it starts and ends inside the storm it is lost; handles are the effect's unique id on that actor, so this needs an ended effect's id to come back. The flood's dumps suggest ids do: every flood stack whose effect was still alive (about 20 of them) named id 1, 2 or 3. That window is only on an actor already restarting at 15+/s.

The guard turns itself off for the session, delivering what it holds, on any exception, a failed VM layout check, or the effect handle format not matching the game's own answer. It stays report only until that format has been checked on at least one real effect.

### The thresholds, and what they stand on

| Setting | Value | Flood | Normal, as far as the logs show |
|---|---|---|---|
| `fOneShotRate` + `iSustainSeconds` | 15/s for 3 s | 25-60 starts/s per actor (from effect ages); 332 new stacks/s overall over 7 actors, and two of them held 84% of the stacks | One burst per attach, then nothing. 20 chickens, 11 goats, horses and a fox with the same scripts had no stacks in any dump |
| `fAttachRate` | 5/s for 3 s | not measured: about one per frame is inferred | One per cell crossing. SPS.log's busiest minutes show up to 28 cell loads in a minute, under 1/s |
| `iStacksOnOneObject` | 200 | 4,156 distinct stacks on the worst actor over four dumps; 451 or fewer on the next five | Every other actor: 2 stacks or fewer; other scripts 1 to 4 each |
| `iDeepFrames`, `iSameFunctionFrames` | 50, 8 | Stacks of 0 to 4 frames: the flood was not recursion | Nothing above 4 frames in any dump. Recursion Monitor's limit is 1000 |
| `fOtherRate` + `iOtherSustainSeconds` | 100/s for 5 s | no data | No data: a guess, set high because a fast `OnUpdate` on one object is allowed |
| overstress | the game's own | 1,820 to 3,997 frames a minute | 0, or 1 to 63 in a minute with cell loads |

**Not measured:** how many one-shot events one actor takes in a single normal attach (an NPC with many scripted abilities could exceed 15 in one second, which is why 3 s in a row is required), and any real per-second rate: the classifier and watch have never run in game. The first logs' `busiest:` rows are what test this. `Papyrus.*.log` only has dumps, so it shows what piled up, not what passed through.

### Can the re-attach itself be stopped?

Not with what is built, and no. The attach and load events are notifications after the fact. Stopping it would mean a hook on the code that moves or attaches an actor, which has not been found, and would risk an actor with no 3D or in the wrong cell. The cause is still inferred: the watch's `LOOP cause:` line is what would prove it (attach rate tracking the restarts), and whether FEC's own `ResetActor3D_FEC` feeds it. Until then the answer is a patch to the actor, as `Reindeer Herd Stays Put` is, and the log names what to look at.

## The hang recorder

On by default (`bHangRecorder=1`, `fStallSeconds=15`, 5 to 600). A thread of its own samples the engine's frame counter four times a second. No frame for that long writes a report to `SkyrimScriptManager.log`, and a second 60 s later if it lasts. A pausing menu with the VM's update still moving counts as alive, and when frames return the log says how long they were gone, so a long loading screen reads as what it is.

A report holds: the fast reads state; every recorded read call in flight, with its thread, how long, the calling script and its route; the load state; the VM's counts, fields and which of its locks and the forms table's lock are held, read and not taken; the main thread's registers and return addresses; the same, without registers, for the other threads in the exe or an SKSE plugin, busiest first, identical stacks folded; the last 64 recorded read calls.

**It records the 17 calls `bFastReads` lists, on or off.** Each native gets a copy of its vtable with `Call` wrapped, so the two routes are told apart: `tasklet` (the fast route, on a job thread) or `message` (the VM's function messages). With fast reads off every call must say `message`, which makes an off run a real control.

**What it will not do once a stall is seen:** allocate, use spdlog, take the loader lock or call the game, because a stuck thread can hold any of those. It formats into static buffers and writes through its own file handle, reads memory with `ReadProcessMemory` so no fault can reach a crash logger's handler, and holds each thread for microseconds with only kernel calls in between. The stacks are scanned, not walked, since a walk needs the unwind tables' locks; an old value can appear, and each entry at least follows a call instruction. `tools\hangtest.cpp` runs the core with a stuck thread holding the process heap and another the loader lock, and fails if the reports do not finish.

**What a report can and cannot say.** A read in flight for seconds, or a stack through `SkyrimScriptManager.dll`, convicts that call. None in flight, and no job thread in a read, clears fast reads of being stuck at that moment; it does not clear memory a fast read may have damaged earlier, which only the same trip with them off can speak to.

## Test steps

Each needs Main Agent's combined run. DevBench tool: `scriptmanager.state`. `op=report` writes a report on demand; `op=flood` repeats an event from the main thread, test saves only; `op=stall where=main|read seconds=N` stops the main thread or the next recorded read call for N seconds to prove the hang recorder.

Remove `Reindeer Herd Stays Put` from the test profile only for step 13.

1. Load a save. EXPECTED: the log has `VM layout checks: all checks passed`, `Joined the VM's stats event`, `Loop watch: joined the cell attach and object load events`, `Effect handle format checked against the game on N effects: it matches`. `scriptmanager.state` reads `layoutOK:true`, `walkOff:false`, `statsEvents` climbing, `loop.handleFormat:"good"`, `loop.guard:false`.
2. Wait 60 s. EXPECTED: a report. Its FPS within about 5% of the SPS.log line for the same minute, overstress frames in the same range. A very different FPS means the VM updates more or less than once a frame, and the report's FPS line is wrong.
3. Read `taskletCallsOffMain`, `updatesOffMain` and `tasksToJobsCalls`. EXPECTED: `updatesOffMain` 0. The other two say where tasklets run; record them, they decide how far holding events can go.
4. Open `SkyrimScriptManager_Roles.txt`. EXPECTED: read log line with a time under a minute; `fec_deatheffectscr` never touch, `oneffectstart` never touch; `_rass_breath_creature` `onupdate` may wait.
5. Stand where overstress turns on (a loaded herd or a town). EXPECTED: an overstress report naming the scripts, `Waiting on:` and `On:` rows, walk cost at or under the 4 ms cap. FPS within SPS's reading of a run without this mod.
6. `op=report` at 3 s intervals for a minute. EXPECTED: `This report cost` under about 5 ms, no ROLE ERROR for a may-wait script in plain play. Any ROLE ERROR: read it; either the classifier missed a call, which is a bug in the reader, or a false alarm from a same-named function.
7. Set `bFastReads=1` and start the game. EXPECTED: `Fast reads: ON for N`. If PapyrusTweaks.ini has `bSpeedUpNativeCalls = true`: the stand-aside line instead. Repeat step 5. Peak suspended falling is the proof; none, and this stage stays off.
8. Normal play: walk through two cell loads and a town, 5 minutes, `bLoopGuard=0`. EXPECTED: no `LOOP` line. Read the `busiest:` rows: they are the real normal rates, and a one-shot row near 15/s or an attach row near 5/s means raise that setting. Any `LOOP` here is a false alarm: read which event and object.
9. Report path without touching a script: `op=flood event=SSM_TestRepeat rate=150 seconds=10`, on a test save. EXPECTED: `loop.floodResult` running, then about 5 s later a `LOOP: SSM_TestRepeat on <actor>` line at about 150/s with `action: report only`, a `LOOP ended` line after it stops, `loop.detections` up by 1. No script runs.
10. Real pair, guard off: `op=flood event=effect rate=40 seconds=20`, near a creature with FEC. EXPECTED: within 3 s `LOOP: OnEffectStart on <that actor>` and `OnEffectFinish`, scripts named with roles, `action: report only (bLoopGuard=0)`, a `LOOP cause:` line; suspended stacks climbing, overstress likely; `loop.dropped` 0. If there is no overstress, raise `rate`.
11. Real pair, guard on: `bLoopGuard=1`, restart, same flood at the rate that overstressed. EXPECTED: `LOOP guard: dropping repeats`, `loop.dropped` climbing, suspended stacks falling or held down against step 10. After it stops: `LOOP ended` with `dropped N, held starts delivered 1 per scripted effect or fewer`, `loop.armedActors` 0, `loop.held` 0. **Killing condition: a None or wrong-state error from that script in Papyrus.0.log that step 10 does not show.**
12. In step 11's actor afterwards: hit it with a fire spell during a flood, and kill it. EXPECTED: a new spell effect's own script still runs; FEC's death look appears (`OnDying` wakes a held start first). Save during a flood, reload: `Loop guard: a save is starting` in the log, no error, the actor's effects behave.
13. The real thing: without `Reindeer Herd Stays Put`, at Windhelm's bank when the herd is there (luck, on a suitable save). EXPECTED: `LOOP: OnEffectStart` on the `FE41D8xx` reindeer, `LOOP stacks` on them, and a `LOOP cause` whose attach rate says whether the attach theory is right. Chickens and goats nearby stay silent.
14. Recursion: a test script calling itself, no wait, on a test save. EXPECTED: `LOOP recursion` naming it once at or before 50 frames, and Recursion Monitor still ending it at 1000. A stack that never leaves `kRunning` between walks is not seen.

## What would prove this wrong

- Layout check fails on 1.5.97 or 1.6 with an unmodified VM: the header is wrong for that runtime, and the walk stays off.
- `Walk: off for this session: reading a stack raised an exception`: a stack was read while it changed, so the rule for when a stack can be read is wrong.
- FPS loss with the mod on compared with off in the same spot, more than the report's own cost says.
- `LOOP` in plain play at a rate that is really normal: the setting is too low.
- A flood with the guard on and `loop.dropped` staying 0 with `armedActors` above 0: event names are not the same interned pointers the hook compares, or the handles differ from what was read.
- Guard on, flood at a real herd, stacks not falling: the load is not from these events.
