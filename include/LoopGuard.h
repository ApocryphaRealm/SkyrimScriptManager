#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// The loop watch and the loop guard. No mod, actor or script is named anywhere in it.
//
// The watch counts each event per object at SendEvent, in a fixed table, and once a second looks for an object taking the same
// event at an abnormal rate, for seconds on end. It reports; it is on by default. The guard, off by default (bLoopGuard), drops
// repeats of one pair of events, OnEffectStart and OnEffectFinish, on an actor the watch has caught, while the VM is overstressed.
namespace LoopGuard
{
	struct Settings
	{
		bool watch = true;             // bLoopWatch
		bool guard = false;            // bLoopGuard
		float oneShotRate = 15.0F;     // events an object should take once: per second, per object
		float attachRate = 5.0F;       // cell attach or detach and 3D load or unload of one object, per second
		float otherRate = 100.0F;      // any other event, per second, per object
		int sustainSeconds = 3;        // one-shot and attach rates must hold this long
		int otherSustainSeconds = 5;   // any other rate must hold this long
		int releaseSeconds = 3;        // below the rate this long: the detection ends and the guard lets go
		int stacksOnOneObject = 200;   // stacks on one object at a single walk
		int deepFrames = 50;           // a stack this many frames deep
		int sameFunctionFrames = 8;    // the same function on one stack this many times
	};

	using SendFn = void (*)(RE::BSScript::Internal::VirtualMachine*, RE::VMHandle, const RE::BSFixedString&, RE::BSScript::IFunctionArguments*);

	void Configure(const Settings& a_settings);
	const Settings& GetSettings();
	void SetOriginalSend(SendFn a_original);   // what a held start is delivered through, so it is not counted or filtered again

	void OnDataLoaded();   // main thread: the interned names and the attach and load sinks
	void OnGameLoaded();   // main thread: checks the effect handle format on the player
	void OnPreLoad();      // main thread: a save is about to load, everything held belongs to the old world
	void OnSave();         // main thread: a save is about to be written, held starts go out first

	// The SendEvent hook, on any thread. True: this event is dropped.
	bool OnSendEvent(RE::BSScript::Internal::VirtualMachine* a_vm, RE::VMHandle a_handle, const char* a_name);

	// The VM update hook, main thread, every frame.
	void Tick(RE::BSScript::Internal::VirtualMachine* a_vm, bool a_overstressed, bool a_layoutOK, std::uint64_t a_now, double a_ticksPerSecond);

	// From the stack walk.
	RE::VMHandle StackKey(RE::VMHandle a_handle);                                     // an effect or an actor both count for the actor
	void OnWalk(RE::BSScript::Internal::VirtualMachine* a_vm, const std::unordered_map<RE::VMHandle, std::uint32_t>& a_stacksByObject);
	void NoteDeepStack(const std::string& a_function, std::uint32_t a_frames, std::uint32_t a_sameFunction);

	// DevBench: repeats an event on a test actor from the main thread, so a detection can be proved without waiting on luck.
	// a_target: a hex form id, or empty for the nearest actor with a scripted effect. a_event: "effect" sends OnEffectFinish and
	// OnEffectStart in turn to each scripted effect on it; any other name goes to the actor's own handle, with no arguments.
	void StartFlood(const std::string& a_target, const std::string& a_event, float a_ratePerSecond, float a_seconds);
	void StopFlood();

	std::vector<std::string> ReportLines();    // main thread, at a report; clears the period's numbers
	std::string StateJsonFragment();           // any thread
}
