# Prior art searched before this mod (rule 24)

Searched on 2026-11-04. The browser was in use by another job, so Nexus was searched through web searches; Nexus pages refuse a
plain fetch (403), so Nexus details below come from search summaries and were not read on the pages. GitHub sources were read.

## Nothing found that manages scripts

Searches for a scheduler, throttler, priority system or event filter for Papyrus on Skyrim SE found none, on Nexus or GitHub. The
closest things are below; each either watches, or changes the VM as a whole rather than script by script.

## Nexus Mods (Skyrim Special Edition)

- Papyrus Stack Stalker NG (151940): live window of active stacks, state and age, a dump button, a session profiler, a graph of VM
  time per frame. Watches only. Source not found.
- SKSE - Real Time Script Profiler (185960): a profiler shown through SKSE Menu Framework. Watches only. Page not read; hook unknown.
- Papyrus Profiler (82770, github.com/DennisSoemers/PapyrusProfiler, Apache-2.0, source read): write_call on 98130/104853 + 0x7F,
  VirtualMachine::AttemptFunctionCall, reading each call with GetFunctionCallInfo and walking the frames, for flame graphs. Watches
  only, in planned runs.
- Papyrus Tweaks NG (77779, github.com/Nightfallstorm/PapyrusTweaks, GPL-3.0, source read): iMaxOpsPerFrame raises a constant; no
  memory limit; stack dump summary; fixes. bSpeedUpNativeCalls (off by default) hooks the callable-from-tasklets check to let every
  native through except a block list by class and name prefix, and takes a mutex around AttemptFunctionCall. It speeds up the whole
  VM's natives; it does not order, hold or drop anything. Our stage 1 stands aside when that option is on.
- Skyrim Performance Solution (test build 1.0.71 in the list, DLL strings and log read only): wraps VirtualMachine Update,
  UpdateTasklets and SetOverstressed in vtable slots 4, 5, 6 to time and count, and writes a per-minute line. Its settings and
  strings have nothing about scripts beyond that. The distance rules are rendering.
- SSE Display Tweaks (34705): OSD warning when overstressed, optional dynamic budget. Scales the budget by frame rate, per the
  project's earlier reading of its settings.
- Papyrus Log Monitor (Oldrim 91791, a PowerShell script), Log Watcher (163979), Papyrus Profiling Parser (39107): read logs.
- Native rewrites of single mods exist as a route, for example Survival Mode Improved - SKSE Edition (its page says it replaces the
  scripts with a plugin and an ESP that strips them), Campfire - Script Optimization (149913).
- Vanilla: bEnableProfiling=1 with StartScriptProfiling and StartStackProfiling writes profiling logs.

## GitHub

`gh search repos` for "papyrus profiler skyrim", "skyrim papyrus vm stacks monitor", "papyrus tweaks", "skyrim script scheduler
papyrus event throttle", "StackStalker"; `gh search code` for SendEvent hooks on VirtualMachine. Found the two repos above, a Fallout 4
port of Papyrus Tweaks (KeryoWolfe/PapyrusOverdrive), and plugins that hook SendEvent for their own use (clayne/GTS_Plugin,
clayne/DevourmentPlugin). No repo that classifies scripts by what they call, none that filters or defers events by role.

## How this mod stands to them

The monitor half overlaps Stack Stalker, the Real Time Script Profiler and SPS's minute line, so it stays as thin as a manager needs:
a log left running, not a rival profiler. No code was copied from any of them. The classifier and any management are the new part.

Project precedent used: `6. current wip mods\BowComboControls` (repo layout and build), `.MD\scripts\gamecode.py` (reading the VM
from a decrypted 1.5.97 dump).
