#pragma once

// The hang recorder's core. Own code, GPL-3.0-or-later (2026-11-05). No game header in here, so tools\hangtest.cpp can run it alone.
//
// Two rules hold everything else in place. The per-call part is stores on atomics and two RDTSCs, with no lock and no allocation.
// The watcher thread takes no game lock, calls no game function and never touches the heap, spdlog or the loader once a stall is
// seen, because any of those could be what the game is stuck on: it writes through its own file handle from static buffers.
#include <atomic>
#include <cstddef>
#include <cstdint>

#include <intrin.h>

namespace HangCore
{
	inline constexpr std::size_t kRingSize = 64;   // a power of two
	inline constexpr std::size_t kSlots = 256;     // a power of two: one per thread that has made a recorded call

	struct alignas(64) RingEntry
	{
		std::atomic<std::uint64_t> seq{ 0 };    // 0 while being written, else the call's number
		std::atomic<std::uint64_t> tsc{ 0 };    // when it began
		std::atomic<std::uint64_t> ticks{ 0 };  // how long it ran; 0 until it has returned
		std::atomic<const void*> script{ nullptr };
		std::atomic<std::uint32_t> thread{ 0 };
		std::atomic<std::uint16_t> native{ 0 };
		std::atomic<std::uint8_t> onTasklet{ 0 };
	};

	struct alignas(64) Slot
	{
		std::atomic<std::uint32_t> owner{ 0 };   // the thread this slot belongs to, for good
		std::atomic<std::uint32_t> native{ 0 };  // index + 1 while a call is in flight, else 0
		std::atomic<std::uint64_t> start{ 0 };
		std::atomic<const void*> script{ nullptr };
		std::atomic<std::uint8_t> onTasklet{ 0 };
	};

	struct State
	{
		alignas(64) std::atomic<std::uint64_t> head{ 0 };
		alignas(64) RingEntry ring[kRingSize];
		Slot slots[kSlots];
	};

	struct CallScope
	{
		std::uint64_t start;
		std::uint64_t seq;
		RingEntry* entry;
		Slot* slot;
	};

	inline Slot* SlotFor(State& a_state, std::uint32_t a_thread) noexcept
	{
		auto at = (a_thread >> 2) & (kSlots - 1);
		for (int probe = 0; probe < 16; ++probe)
		{
			auto& slot = a_state.slots[at];
			auto owner = slot.owner.load(std::memory_order_relaxed);
			if (owner == a_thread) { return &slot; }
			if (owner == 0 && slot.owner.compare_exchange_strong(owner, a_thread, std::memory_order_relaxed)) { return &slot; }
			at = (at + 1) & (kSlots - 1);
		}
		return nullptr;  // the ring still has the call; it just cannot show as in flight
	}

	// The hot path. __readgsdword(0x48) is the thread id in the TEB: what GetCurrentThreadId reads, without the call.
	inline void Enter(State& a_state, CallScope& a_call, std::uint16_t a_native, const void* a_script, bool a_onTasklet) noexcept
	{
		const std::uint32_t thread = __readgsdword(0x48);
		a_call.start = __rdtsc();
		a_call.seq = a_state.head.fetch_add(1, std::memory_order_relaxed) + 1;
		auto* entry = &a_state.ring[a_call.seq & (kRingSize - 1)];
		a_call.entry = entry;
		entry->seq.store(0, std::memory_order_relaxed);
		entry->tsc.store(a_call.start, std::memory_order_relaxed);
		entry->ticks.store(0, std::memory_order_relaxed);
		entry->script.store(a_script, std::memory_order_relaxed);
		entry->thread.store(thread, std::memory_order_relaxed);
		entry->native.store(a_native, std::memory_order_relaxed);
		entry->onTasklet.store(a_onTasklet ? 1 : 0, std::memory_order_relaxed);
		entry->seq.store(a_call.seq, std::memory_order_release);

		auto* slot = SlotFor(a_state, thread);
		a_call.slot = slot;
		if (slot)
		{
			slot->start.store(a_call.start, std::memory_order_relaxed);
			slot->script.store(a_script, std::memory_order_relaxed);
			slot->onTasklet.store(a_onTasklet ? 1 : 0, std::memory_order_relaxed);
			slot->native.store(static_cast<std::uint32_t>(a_native) + 1, std::memory_order_release);
		}
	}

	inline void Leave(CallScope& a_call) noexcept
	{
		const auto spent = __rdtsc() - a_call.start;
		if (a_call.slot) { a_call.slot->native.store(0, std::memory_order_release); }
		// A newer call may have taken this entry on a very busy ring: then the time is simply not kept.
		if (a_call.entry->seq.load(std::memory_order_relaxed) == a_call.seq) { a_call.entry->ticks.store(spent ? spent : 1, std::memory_order_relaxed); }
	}

	// What the watcher needs from the game, each of them lock-free. Any may be null. They run on the watcher thread during a stall.
	struct Hooks
	{
		int (*frame)() = nullptr;                                        // the engine's frame counter, or -1
		std::uint64_t (*vmUpdates)() = nullptr;                          // how many times the VM's update has run
		bool (*paused)() = nullptr;                                      // a pausing menu is open
		const char* (*nativeName)(std::uint16_t a_native) = nullptr;
		void (*scriptName)(const void* a_script, char* a_out, std::size_t a_size) = nullptr;
		void (*gameLines)() = nullptr;                                   // load and VM state, through Emit
		const char* (*fastReadsStatus)() = nullptr;
	};

	State& GlobalState();

	// Starts the one watcher thread. a_secondGapSeconds: how long after the first report the second one is written, if frames are still stopped.
	// False: the log could not be opened for appending, and no thread was started.
	bool StartWatcher(const Hooks& a_hooks, double a_stallSeconds, double a_secondGapSeconds, const wchar_t* a_logPath);

	// Which thread is the main one, and how that is known. Atomic: any thread may call it. a_how must be a string that lives for good.
	void SetMainThread(std::uint32_t a_thread, const char* a_how);
	std::uint32_t MainThread();

	// For a hook: a line of a report, as printf. Watcher thread only. No heap.
	void Emit(const char* a_format, ...);

	// A read that cannot fault, and so never reaches a crash logger's handler.
	bool SafeRead(const void* a_from, void* a_to, std::size_t a_size);

	double MillisecondsSince(std::uint64_t a_tsc);

	// Nanoseconds for one Enter and Leave on this machine, one thread, measured on a private State. Takes a few milliseconds.
	double MeasureCost(int a_calls);
	// The same with a_threads threads at once on one State: nanoseconds per call as each thread sees it.
	double MeasureCostThreads(int a_threads, int a_callsEach);

	struct Stats
	{
		std::uint32_t stalls;
		std::uint32_t reports;
		double lastStallSeconds;
		double ringCostNS;
		double worstSuspendMicroseconds;
		double lastReportMS;
		bool armed;
		bool stalled;
	};
	Stats GetStats();
	std::uint32_t InFlightNow();
}
