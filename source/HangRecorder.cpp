// The hang recorder's game side. Own code, GPL-3.0-or-later (2026-11-05).
//
// To see the read calls FastReads marks, each of those natives gets a copy of its own vtable with the Call slot pointing here. Both
// routes pass through Call: the VM (1.5.97 id 98130) checks CanBeCalledFromTasklets and either calls it at once on the tasklet thread or
// queues a function message that calls it later. So the recording runs the same with bFastReads on or off, the route is noted on each
// call, and a run with fast reads off is a real control. Nothing is patched in the game's code, and only these natives are touched.
//
// The frame source is the engine's counter (525008 / 411489), a plain int the main loop increments and TestBench reads the same way.
//
// Everything named *Lines or read by the watcher is lock-free: fields read directly, never a game function. The VM's spin locks are
// read, not taken, to show who holds them.
#include "PCH.h"

#include "FastReads.h"
#include "HangCore.h"
#include "HangRecorder.h"
#include "Monitor.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <string_view>

#include <Windows.h>

namespace HangRecorder
{
	namespace
	{
		using VM = RE::BSScript::Internal::VirtualMachine;
		using Function = RE::BSScript::IFunction;
		using Stack = RE::BSScript::Stack;
		using TypeInfoPtr = RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo>;

		inline constexpr double kSecondGapSeconds = 60.0;   // the second report, if frames are still stopped
		inline constexpr std::size_t kVtableSlots = 32;     // more than a native has; copied with a read that stops at the end of mapped memory
		inline constexpr std::size_t kCallSlot = 0x0F;      // IFunction::Call

		bool g_enabled = true;
		float g_stallSeconds = 15.0F;

		std::uint32_t g_installThread = 0;
		std::uint32_t g_dataLoadedThread = 0;
		std::uint32_t g_loadMessageThread = 0;

		// ---- what the watcher reads ----

		std::int32_t* g_frameCounter = nullptr;
		RE::UI* g_ui = nullptr;
		VM* g_vm = nullptr;
		RE::BSReadWriteLock* g_formsLock = nullptr;

		std::atomic<std::uint64_t> g_loadingMenuSince{ 0 };   // RDTSC; 0 when the loading menu is closed
		std::atomic<std::uint64_t> g_loadingMenuClosedAt{ 0 };
		std::atomic<std::uint64_t> g_saveLoadSince{ 0 };      // between the pre-load and post-load messages
		std::atomic<std::uint64_t> g_saveLoadEndedAt{ 0 };
		std::atomic<std::uint32_t> g_loadsSeen{ 0 };

		// ---- the wrapped natives ----

		using CallFn = Function::CallResult (*)(Function*, const RE::BSTSmartPointer<Stack>&, RE::BSScript::ErrorLogger*, VM*, bool);

		struct Wrapped
		{
			CallFn original = nullptr;
			std::uint16_t index = 0;
			void* slots[1 + kVtableSlots]{};   // [0] is the RTTI pointer a vtable keeps before its first function
		};
		inline constexpr std::size_t kMaxWrapped = 32;
		Wrapped g_wrapped[kMaxWrapped];
		char g_names[kMaxWrapped][64];
		std::size_t g_wrappedCount = 0;
		std::size_t g_markedCount = 0;      // of the wrapped, how many the VM reads back as callable from tasklets
		char g_fastStatus[256] = "not applied yet";

		std::atomic<std::uint32_t> g_holdMS{ 0 };

		const Wrapped* WrappedOf(const Function* a_function)
		{
			const auto vtable = reinterpret_cast<std::uintptr_t>(*reinterpret_cast<void* const*>(a_function));
			return reinterpret_cast<const Wrapped*>(vtable - offsetof(Wrapped, slots) - sizeof(void*));
		}

		// The script that called this native: the native's own frame is on top, its caller just under it. These are the frames the native
		// itself is about to read, on the thread that owns them.
		const void* CallingScript(const RE::BSTSmartPointer<Stack>& a_stack)
		{
			const auto* stack = a_stack.get();
			if (!stack || !stack->top || !stack->top->previousFrame) { return nullptr; }
			return stack->top->previousFrame->owningObjectType.get();
		}

		Function::CallResult CallThunk(Function* a_function, const RE::BSTSmartPointer<Stack>& a_stack, RE::BSScript::ErrorLogger* a_logger, VM* a_vm, bool a_onTasklet)
		{
			const auto* wrapped = WrappedOf(a_function);
			HangCore::CallScope call;
			HangCore::Enter(HangCore::GlobalState(), call, wrapped->index, CallingScript(a_stack), a_onTasklet);
			if (g_holdMS.load(std::memory_order_relaxed) != 0) [[unlikely]]
			{
				if (const auto ms = g_holdMS.exchange(0); ms != 0)
				{
					logger::warn("TEST: {} on thread {} now sleeps {} ms inside the recorder's wrapper", g_names[wrapped->index], GetCurrentThreadId(), ms);
					Sleep(ms);
				}
			}
			const auto result = wrapped->original(a_function, a_stack, a_logger, a_vm, a_onTasklet);
			HangCore::Leave(call);
			return result;
		}

		bool Wrap(Function* a_function, const char* a_type, const char* a_name)
		{
			if (g_wrappedCount >= kMaxWrapped || !a_function || !a_function->GetIsNative()) { return false; }
			auto& wrapped = g_wrapped[g_wrappedCount];
			const auto* vtable = *reinterpret_cast<void* const* const*>(a_function);
			SIZE_T got = 0;
			ReadProcessMemory(GetCurrentProcess(), vtable - 1, wrapped.slots, sizeof(wrapped.slots), &got);
			if (got < (kCallSlot + 2) * sizeof(void*)) { return false; }
			wrapped.original = reinterpret_cast<CallFn>(wrapped.slots[1 + kCallSlot]);
			wrapped.index = static_cast<std::uint16_t>(g_wrappedCount);
			wrapped.slots[1 + kCallSlot] = reinterpret_cast<void*>(&CallThunk);
			snprintf(g_names[g_wrappedCount], sizeof(g_names[0]), "%s.%s", a_type, a_name);
			if (a_function->CanBeCalledFromTasklets()) { ++g_markedCount; }
			++g_wrappedCount;
			*reinterpret_cast<void***>(a_function) = &wrapped.slots[1];
			return true;
		}

		// The same walk SetCallableFromTasklets does: the type by name under the VM's type lock, then its functions, which are still in the
		// unlinked list if the script has not been linked yet. A native's object is the same one before and after linking.
		Function* FindNative(RE::BSScript::ObjectTypeInfo* a_type, const char* a_name)
		{
			const auto named = [&](Function* a_function) { return a_function && _stricmp(a_function->GetName().c_str(), a_name) == 0; };
			if (a_type->linkedValid < RE::BSScript::ObjectTypeInfo::LinkValidState::kLinkedInvalid)
			{
				for (auto* link = static_cast<RE::BSScript::ObjectTypeInfo::UnlinkedNativeFunction*>(a_type->data); link; link = link->next)
				{
					if (named(link->func.get())) { return link->func.get(); }
				}
				return nullptr;
			}
			auto* member = a_type->GetMemberFuncIter();
			for (std::uint32_t i = 0; i < a_type->GetNumMemberFuncs(); ++i)
			{
				if (named(member[i].func.get())) { return member[i].func.get(); }
			}
			auto* global = a_type->GetGlobalFuncIter();
			for (std::uint32_t i = 0; i < a_type->GetNumGlobalFuncs(); ++i)
			{
				if (named(global[i].func.get())) { return global[i].func.get(); }
			}
			return nullptr;
		}

		void WrapReads()
		{
			if (!g_vm) { return; }
			RE::BSSpinLockGuard guard(g_vm->typeInfoLock);
			for (const auto& read : FastReads::Reads())
			{
				const auto found = g_vm->objectTypeMap.find(RE::BSFixedString(read.type));
				if (found == g_vm->objectTypeMap.end() || !found->second)
				{
					logger::warn("Hang recorder: no script type {}, so {}.{} is not recorded", read.type, read.type, read.function);
					continue;
				}
				if (!Wrap(FindNative(found->second.get(), read.function), read.type, read.function)) { logger::warn("Hang recorder: {}.{} not found or not native, so it is not recorded", read.type, read.function); }
			}
		}

		// ---- the hooks the watcher calls: lock-free, no game function ----

		int HookFrame() { return g_frameCounter ? std::atomic_ref<std::int32_t>(*g_frameCounter).load(std::memory_order_relaxed) : -1; }

		std::uint64_t HookVMUpdates() { return Monitor::GetHangSnapshot().vmUpdates; }

		bool HookPaused() { return g_ui && g_ui->numPausesGame != 0; }

		const char* HookNativeName(std::uint16_t a_native) { return a_native < g_wrappedCount ? g_names[a_native] : nullptr; }

		void HookScriptName(const void* a_type, char* a_out, std::size_t a_size)
		{
			const char* text = nullptr;
			const auto* field = reinterpret_cast<const std::byte*>(a_type) + offsetof(RE::BSScript::ObjectTypeInfo, name);
			if (!HangCore::SafeRead(field, &text, sizeof(text)) || !text) { return; }
			char copy[64]{};
			for (std::size_t i = 0; i + 1 < sizeof(copy) && i + 1 < a_size; ++i)
			{
				if (!HangCore::SafeRead(text + i, &copy[i], 1)) { return; }
				if (copy[i] == 0) { break; }
			}
			strncpy_s(a_out, a_size, copy, _TRUNCATE);
		}

		const char* HookFastStatus() { return g_fastStatus; }

		// A BSSpinLock and a BSReadWriteLock are two 32-bit fields, private in the headers: the holder's thread, then a count.
		struct LockView
		{
			std::uint32_t owner;
			std::uint32_t count;
		};
		static_assert(sizeof(RE::BSSpinLock) == sizeof(LockView));
		static_assert(sizeof(RE::BSReadWriteLock) == sizeof(LockView));

		void EmitHeldLock(const char* a_name, const void* a_lock, std::uint32_t& a_held)
		{
			const auto* view = reinterpret_cast<const volatile LockView*>(a_lock);
			const std::uint32_t owner = view->owner;
			const std::uint32_t count = view->count;
			if (count == 0 && owner == 0) { return; }
			++a_held;
			HangCore::Emit("  %s: held by thread %u, count %u", a_name, owner, count);
		}

		void GameLines()
		{
			const auto since = [](std::uint64_t a_tsc) { return HangCore::MillisecondsSince(a_tsc) / 1000.0; };

			if (const auto open = g_loadingMenuSince.load(std::memory_order_relaxed); open != 0) { HangCore::Emit("Load: the Loading Menu has been open %.1f s", since(open)); }
			else if (const auto closed = g_loadingMenuClosedAt.load(std::memory_order_relaxed); closed != 0) { HangCore::Emit("Load: the Loading Menu is closed; it last closed %.1f s ago", since(closed)); }
			else { HangCore::Emit("Load: the Loading Menu has not opened yet"); }
			if (const auto load = g_saveLoadSince.load(std::memory_order_relaxed); load != 0) { HangCore::Emit("Load: a save load began %.1f s ago and has not finished", since(load)); }
			else if (const auto ended = g_saveLoadEndedAt.load(std::memory_order_relaxed); ended != 0) { HangCore::Emit("Load: no save load is running; the last ended %.1f s ago (%u this session)", since(ended), g_loadsSeen.load(std::memory_order_relaxed)); }

			const auto snapshot = Monitor::GetHangSnapshot();
			HangCore::Emit("VM counts at the last stats event: %u running, %u suspended, %u queued calls, %u detached | overstressed %s, budget %.2f ms", snapshot.running, snapshot.suspended, snapshot.messages, snapshot.detached, snapshot.overstressed ? "yes" : "no", snapshot.budgetMS);
			if (snapshot.updateThread != 0) { HangCore::Emit("VM update: RUNNING NOW on thread %u, for %.0f ms", snapshot.updateThread, snapshot.updateRunningMS); }
			else { HangCore::Emit("VM update: not running; the last ended %.0f ms ago on thread %u", snapshot.sinceUpdateEndedMS, snapshot.lastUpdateThread); }
			HangCore::Emit("VM tasklets: %d passes running now, last on thread %u | tasks to jobs %u times | updates off the load-message thread %u, tasklet passes %u", snapshot.taskletsActive, snapshot.lastTaskletThread, snapshot.tasksToJobs, snapshot.updatesOffMain, snapshot.taskletsOffMain);

			if (g_vm)
			{
				const auto* vm = g_vm;
				HangCore::Emit("VM fields, read with no lock so they may be mid-change: freezeState %u, frozen stacks %u, overstressed %u, initialized %u, waiting function messages %u, suspend queues %u and %u", static_cast<std::uint32_t>(vm->freezeState.underlying()), vm->frozenStacksCount, vm->overstressed ? 1u : 0u, vm->initialized ? 1u : 0u, vm->uiWaitingFunctionMessages, vm->suspendQueue1.numEntries, vm->suspendQueue2.numEntries);
				HangCore::Emit("Locks read, not taken (only the held ones are listed):");
				std::uint32_t held = 0;
				EmitHeldLock("VM typeInfoLock", &vm->typeInfoLock, held);
				EmitHeldLock("VM funcQueueLock", &vm->funcQueueLock, held);
				EmitHeldLock("VM suspendQueueLock", &vm->suspendQueueLock, held);
				EmitHeldLock("VM runningStacksLock", &vm->runningStacksLock, held);
				EmitHeldLock("VM frozenStacksLock", &vm->frozenStacksLock, held);
				EmitHeldLock("VM attachedScriptsLock", &vm->attachedScriptsLock, held);
				EmitHeldLock("VM arraysLock", &vm->arraysLock, held);
				EmitHeldLock("VM objectResetLock", &vm->objectResetLock, held);
				EmitHeldLock("VM objectLock", &vm->objectLock, held);
				EmitHeldLock("VM queuedUnbindLock", &vm->queuedUnbindLock, held);
				if (g_formsLock) { EmitHeldLock("the forms table (writer's thread, then the lock's count)", g_formsLock, held); }
				if (held == 0) { HangCore::Emit("  none of the VM's locks or the forms table's is held"); }
			}
			else { HangCore::Emit("VM: no VM yet"); }
		}

		// ---- load state ----

		class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME)
				{
					if (a_event->opening) { g_loadingMenuSince.store(__rdtsc(), std::memory_order_relaxed); }
					else
					{
						g_loadingMenuSince.store(0, std::memory_order_relaxed);
						g_loadingMenuClosedAt.store(__rdtsc(), std::memory_order_relaxed);
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		MenuSink g_menuSink;

		// ---- the thread ids ----

		void LogThreadIDs()
		{
			DWORD windowThread = 0;
			struct Find
			{
				DWORD pid;
				DWORD thread;
			} find{ GetCurrentProcessId(), 0 };
			EnumWindows(
				[](HWND a_window, LPARAM a_find) -> BOOL {
					auto& target = *reinterpret_cast<Find*>(a_find);
					DWORD pid = 0;
					const auto thread = GetWindowThreadProcessId(a_window, &pid);
					if (pid == target.pid && IsWindowVisible(a_window))
					{
						target.thread = thread;
						return FALSE;
					}
					return TRUE;
				},
				reinterpret_cast<LPARAM>(&find));
			windowThread = find.thread;
			logger::info("Hang recorder thread ids, process {}: main (runs the game's queued tasks) {}, game window {}, plugin install {}, data loaded {}, load messages {}. By hand: py -3 hang_stack.py {} {}",
				GetCurrentProcessId(), HangCore::MainThread(), windowThread, g_installThread, g_dataLoadedThread, g_loadMessageThread, GetCurrentProcessId(), HangCore::MainThread());
		}

		// SKSE runs a queued task from the main loop, so whichever thread runs this one is the thread TestBench's queued tasks wait for.
		void LearnMainThread()
		{
			auto* tasks = SKSE::GetTaskInterface();
			if (!tasks)
			{
				logger::warn("Hang recorder: no task interface, so the main thread is taken from the load message");
				return;
			}
			tasks->AddTask([] {
				HangCore::SetMainThread(GetCurrentThreadId(), "the thread that runs the game's queued tasks");
				LogThreadIDs();
			});
		}
	}

	void Configure(bool a_enabled, float a_stallSeconds)
	{
		g_enabled = a_enabled;
		g_stallSeconds = a_stallSeconds;
	}

	void NoteInstallThread() { g_installThread = GetCurrentThreadId(); }

	void OnDataLoaded()
	{
		g_dataLoadedThread = GetCurrentThreadId();
		if (!g_enabled)
		{
			logger::info("Hang recorder: off (bHangRecorder=0)");
			return;
		}

		g_vm = VM::GetSingleton();
		g_ui = RE::UI::GetSingleton();
		try
		{
			g_frameCounter = reinterpret_cast<std::int32_t*>(REL::RelocationID(525008, 411489).address());
		}
		catch (...)
		{
			g_frameCounter = nullptr;
		}
		if (!g_frameCounter) { logger::warn("Hang recorder: no engine frame counter on this runtime, so frames are read from the VM's update alone"); }
		{
			auto forms = RE::TESForm::GetAllForms();
			g_formsLock = &forms.second.get();
		}
		if (g_ui) { g_ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink); }
		else { logger::warn("Hang recorder: no UI singleton, so the Loading Menu and pausing menus are not seen"); }

		WrapReads();
		snprintf(g_fastStatus, sizeof(g_fastStatus), "%s | %zu read calls recorded, %zu of them marked callable from tasklets", FastReads::Status(), g_wrappedCount, g_markedCount);
		logger::info("Hang recorder: {}", g_fastStatus);

		const auto directory = SKSE::log::log_directory();
		if (!directory)
		{
			logger::error("Hang recorder: no log folder, so the watcher did not start");
			return;
		}
		const auto path = *directory / "SkyrimScriptManager.log";

		HangCore::SetMainThread(GetCurrentThreadId(), "the thread that delivered data loaded; not confirmed yet");
		HangCore::Hooks hooks;
		hooks.frame = &HookFrame;
		hooks.vmUpdates = &HookVMUpdates;
		hooks.paused = &HookPaused;
		hooks.nativeName = &HookNativeName;
		hooks.scriptName = &HookScriptName;
		hooks.gameLines = &GameLines;
		hooks.fastReadsStatus = &HookFastStatus;
		if (!HangCore::StartWatcher(hooks, g_stallSeconds, kSecondGapSeconds, path.c_str()))
		{
			logger::error("Hang recorder: could not open the log for its own writes or start its thread (error {}), so it is off", GetLastError());
			return;
		}
		logger::info("Hang recorder: on, {:.0f} s without a frame writes a report here, and again {:.0f} s later if it lasts", g_stallSeconds, kSecondGapSeconds);
		LearnMainThread();
	}

	void OnPreLoad()
	{
		if (!g_enabled) { return; }
		g_saveLoadSince.store(__rdtsc(), std::memory_order_relaxed);
	}

	void OnGameLoaded()
	{
		g_loadMessageThread = GetCurrentThreadId();
		if (!g_enabled) { return; }
		g_saveLoadSince.store(0, std::memory_order_relaxed);
		g_saveLoadEndedAt.store(__rdtsc(), std::memory_order_relaxed);
		g_loadsSeen.fetch_add(1, std::memory_order_relaxed);
		LearnMainThread();
	}

	void TestStall(const std::string& a_where, float a_seconds)
	{
		const auto ms = static_cast<std::uint32_t>(std::clamp(a_seconds, 1.0F, 180.0F) * 1000.0F);
		if (a_where == "read")
		{
			logger::warn("TEST: the next recorded read call will sleep {} ms inside the recorder's wrapper", ms);
			g_holdMS.store(ms);
			return;
		}
		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks)
		{
			logger::error("TEST: no task interface, so no stall");
			return;
		}
		logger::warn("TEST: the main thread will sleep {} ms in a queued task", ms);
		tasks->AddTask([ms] {
			logger::warn("TEST: main thread {} sleeps {} ms now", GetCurrentThreadId(), ms);
			Sleep(ms);
			logger::warn("TEST: main thread awake");
		});
	}

	std::string StateJsonFragment()
	{
		const auto stats = HangCore::GetStats();
		return std::format(
			R"("hang":{{"on":{},"stallSeconds":{:.0f},"armed":{},"stalledNow":{},"stalls":{},"reports":{},"lastStallSeconds":{:.1f},"lastReportMS":{:.1f},"worstHoldUS":{:.0f},"ringCostNS":{:.1f},"readCallsRecorded":{},"readsWrapped":{},"readsMarked":{},"inFlight":{},"mainThread":{}}})",
			g_enabled, g_stallSeconds, stats.armed, stats.stalled, stats.stalls, stats.reports, stats.lastStallSeconds, stats.lastReportMS, stats.worstSuspendMicroseconds, stats.ringCostNS,
			HangCore::GlobalState().head.load(std::memory_order_relaxed), g_wrappedCount, g_markedCount, HangCore::InFlightNow(), HangCore::MainThread());
	}
}
