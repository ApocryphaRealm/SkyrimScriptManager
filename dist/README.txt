Skyrim Script Manager
===================
Version 1.0.0

Tells you what Papyrus is doing while you play, and catches script loops as they happen. As shipped it holds and drops nothing.

WHAT YOU GET, in your SKSE log folder (Documents\My Games\Skyrim Special Edition\SKSE)
- SkyrimScriptManager.log: a short report every minute and when the game says scripts are overstressed. Frame rate, how many frames were
  overstressed, how much time the script engine took, how many stacks are running, suspended and queued, which scripts began them,
  which actors they are on and what they are waiting on.
- SkyrimScriptManager_Roles.txt: every script your game has loaded, and what it could tolerate when frames are low, worked out from what
  its code calls: may drop, may wait or never touch. Unsure is always never touch. Each line gives the call that decided it.
- SkyrimScriptManager_Flips.txt: only if the monitor saw a script do something its code was not found to do. That script stays never touch.

THE LOOP WATCH, on as shipped
An object taking the same event over and over gets a line in the log saying which object, which event, how fast, which scripts and
what was done. Magic effects restarting on an actor, stacks piling on one object and scripts recursing are all caught this way, for
any mod and any actor; nothing here names one. If the actor is re-attaching, the log says so and gives its package, so you know what
to patch.

THE LOOP GUARD, off as shipped (bLoopGuard)
Set bLoopGuard=1 and, while the game says scripts are overstressed, a caught actor's repeated magic effect restarts stop reaching its
scripts until the storm ends. A script sees its first start and finish, and a final start if its effect is still running. A new effect
on that actor still goes through, and no other event is ever dropped. It removes the load, not the cause: the actor keeps re-attaching
until something changes what moves it.

A clean log only means nothing bad was seen in that session. The monitor looks at stacks at moments, not at every call.

REQUIREMENTS
SKSE and Address Library for SKSE Plugins. Skyrim SE 1.5.97, AE 1.6, or 1.7. Two builds ship: install the one for your game. No plugin slot.

SETTINGS
SKSE\Plugins\SkyrimScriptManager.ini: how often it reports, the limits on reading stacks, and the loop watch's rates.
SKSE\Plugins\SkyrimScriptManager_Overrides.ini: ships empty, and nothing depends on it. It forces a script's role.

bFastReads, off as shipped, lets a short list of read-only calls answer without waiting for the frame. It stands aside if Papyrus Tweaks'
bSpeedUpNativeCalls is on.

COMPATIBILITY
Shares three VM functions with Skyrim Performance Solution. Both call through, in either load order.
Recursion Monitor and Papyrus Tweaks end a stack past 1000 frames. This reports far shallower ones and ends nothing, so keep them.

SOURCE
https://github.com/ApocryphaRealm/ScriptLoadMonitor (GPL-3.0-or-later; see LICENSE, NOTICE.md and THIRD_PARTY_NOTICES.md)
