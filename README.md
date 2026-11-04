# Script Load Monitor

A plain SKSE plugin for Skyrim SE 1.5.97 and AE 1.6 that listens to the Papyrus VM. Three parts:

- **The listener** writes `ScriptLoadMonitor.log`, a short report every 60 s and when overstress begins: FPS and frame times, frames
  overstressed, the VM's time and budget, running, suspended and queued stacks, new stacks per second, events sent, what began the
  waiting stacks and which actor they are on, what they wait on, and how long the longest has been seen waiting.
- **The classifier** reads each script the game has loaded and judges each one, and each event in it, from its code: may drop, may
  wait or never touch. Anything unsure is never touch. `ScriptLoadMonitor_Roles.txt` gives the deciding call for each. No table ships:
  it is worked out from the player's own scripts on each run.
- **The check**: a script labelled may drop or may wait that is seen on a stack calling something its code was not found to call
  gets an error line and turns never touch, and stays so through `ScriptLoadMonitor_Flips.txt`.

As shipped it holds and drops nothing. `bFastReads` (off) is management stage 1. Stage 2, holding event floods, is not built.

See `dist/README.txt` for what to install and `PRIOR-ART.md` for what already exists. The classifier builds outside the game too:
`tools\build-rolecheck.bat`, then `build\rolecheck\rolecheck.exe <folder of .pex> <out.tsv>`.

Version 1.0.0

## Test steps

Each needs Main Agent's combined run. DevBench tool: `scriptmonitor.state`, with `op=report` to write a report on demand.

1. Load a save. EXPECTED: the log has `VM layout checks: all checks passed` and `Joined the VM's stats event`. `scriptmonitor.state` reads `layoutOK:true`, `walkOff:false`, `statsEvents` climbing.
2. Wait 60 s. EXPECTED: a report. Its FPS within about 5% of the SPS.log line for the same minute, overstress frames in the same range. A very different FPS means the VM updates more or less than once a frame, and the report's FPS line is wrong.
3. Read `taskletCallsOffMain`, `updatesOffMain` and `tasksToJobsCalls`. EXPECTED: `updatesOffMain` 0. The other two say where tasklets run; record them, they decide how far stage 2 can go.
4. Open `ScriptLoadMonitor_Roles.txt`. EXPECTED: read log line with a time under a minute; `fec_deatheffectscr` never touch, `oneffectstart` never touch; `_rass_breath_creature` `onupdate` may wait.
5. Stand where overstress turns on (a loaded herd or a town). EXPECTED: an overstress report naming the scripts, `Waiting on:` and `On:` rows, walk cost at or under the 4 ms cap. FPS within SPS's reading of a run without this mod.
6. `op=report` at 3 s intervals for a minute. EXPECTED: `This report cost` under about 5 ms, no ROLE ERROR for a may-wait script in plain play. Any ROLE ERROR: read it; either the classifier missed a call, which is a bug in the reader, or a false alarm from a same-named function.
7. Set `bFastReads=1` and start the game. EXPECTED: `Fast reads: ON for N`. If PapyrusTweaks.ini has `bSpeedUpNativeCalls = true`: the stand-aside line instead. Repeat step 5. Peak suspended falling is the proof; none, and this stage stays off.

## What would prove this wrong

- Layout check fails on 1.5.97 or 1.6 with an unmodified VM: the header is wrong for that runtime, and the walk stays off.
- `Walk: off for this session: reading a stack raised an exception`: a stack was read while it changed, so the rule for when a stack can be read is wrong.
- FPS loss with the mod on compared with off in the same spot, more than the report's own cost says.
