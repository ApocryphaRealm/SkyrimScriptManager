Script Load Monitor
===================
Version 1.0.0

Tells you what Papyrus is doing while you play. It holds and drops nothing.

WHAT YOU GET, in your SKSE log folder (Documents\My Games\Skyrim Special Edition\SKSE)
- ScriptLoadMonitor.log: a short report every minute and when the game says scripts are overstressed. Frame rate, how many frames were
  overstressed, how much time the script engine took, how many stacks are running, suspended and queued, which scripts began them,
  which actors they are on and what they are waiting on.
- ScriptLoadMonitor_Roles.txt: every script your game has loaded, and what it could tolerate when frames are low, worked out from what
  its code calls: may drop, may wait or never touch. Unsure is always never touch. Each line gives the call that decided it.
- ScriptLoadMonitor_Flips.txt: only if the monitor saw a script do something its code was not found to do. That script stays never touch.

A clean log only means nothing bad was seen in that session. The monitor looks at stacks at moments, not at every call.

REQUIREMENTS
SKSE and Address Library for SKSE Plugins. Skyrim SE 1.5.97 or AE 1.6. No plugin slot.

SETTINGS
SKSE\Plugins\ScriptLoadMonitor.ini: how often it reports, and the limits on reading stacks.
SKSE\Plugins\ScriptLoadMonitor_Overrides.ini: ships empty, and nothing depends on it. It forces a script's role.

bFastReads, off as shipped, lets a short list of read-only calls answer without waiting for the frame. It stands aside if Papyrus Tweaks'
bSpeedUpNativeCalls is on.

COMPATIBILITY
Shares three VM functions with Skyrim Performance Solution. Both call through, in either load order.

SOURCE
https://github.com/ApocryphaRealm/ScriptLoadMonitor (GPL-3.0-or-later; see LICENSE, NOTICE.md and THIRD_PARTY_NOTICES.md)
