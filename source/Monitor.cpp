// The listener. Own code, GPL-3.0-or-later (2026-11-04).
//
// It wraps VM virtual functions to time and count them, in the same slots Skyrim Performance Solution uses, and calls the
// original with the original arguments. The one exception is SendEvent, and only with bLoopGuard=1: see LoopGuard.cpp.
//
// What the 1.5.97 code does, read from a decrypted dump of the game's .text (SkyrimVM::Update id 53115, VirtualMachine::Update
// vfunc 4, id 98133 to 98137, the stats sink at rva 0x92A9C0):
//   - VirtualMachine::Update posts a BSScript::StatsEvent at its end with the four counts this mod reports. That event is a
//     public part of the VM, so the counts need no field read. SkyrimVM's own sink turns "suspended > 100" into
//     SetOverstressed(true), so overstress is a COUNT, not a time.
//   - SetOverstressed only stores a byte, so the hook's own copy of that value costs nothing.
//
// Field reads for the stack walk go through CommonLibSSE-NG's layout, and each one a walk relies on is checked at run time in
// CheckLayout() against something that must be true of it. A failed check turns the walk off, says which check, and leaves
// the counters running, because those do not depend on the layout.
//
// When a stack can be read. Stacks in the suspend queues and stacks waiting on another stack are not executing. The walk runs
// on the main thread at the top of VirtualMachine::Update, before the VM moves anything, and not while a tasklet hook is
// active on another thread; if the game ever uses TasksToJobs, which runs stacks on job threads, the walk of the running table
// turns off and says so. Every read is under structured exception handling: a fault turns the walk off for the session.
#include "PCH.h"

#include "LoopGuard.h"
#include "Monitor.h"
#include "Roles.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <Windows.h>
#undef GetObject

namespace Monitor
{
	namespace
	{
		using VM = RE::BSScript::Internal::VirtualMachine;
		using Stack = RE::BSScript::Stack;
		using Frame = RE::BSScript::StackFrame;
		using StackPtr = RE::BSTSmartPointer<Stack>;
		using QueryPtr = RE::BSTSmartPointer<RE::BSScript::Internal::IFuncCallQuery>;

		Settings g_settings;
		double g_ticksPerMS = 1.0;

		std::uint64_t Now()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return static_cast<std::uint64_t>(t.QuadPart);
		}

		double ToMS(std::uint64_t a_ticks) { return static_cast<double>(a_ticks) / g_ticksPerMS; }

		// ---- counters: written where the thing happens, read and cleared at the report ----

		struct Counters
		{
			std::atomic<std::uint32_t> frames{ 0 };
			std::atomic<std::uint32_t> pausedFrames{ 0 };
			std::atomic<std::uint32_t> overstressedFrames{ 0 };
			std::atomic<std::uint32_t> overstressOn{ 0 };
			std::atomic<std::uint32_t> overstressOff{ 0 };
			std::atomic<std::uint64_t> frameTicks{ 0 };
			std::atomic<std::uint64_t> worstFrameTicks{ 0 };
			std::atomic<std::uint64_t> updateTicks{ 0 };
			std::atomic<std::uint64_t> worstUpdateTicks{ 0 };
			std::atomic<std::uint64_t> taskletTicks{ 0 };
			std::atomic<std::uint64_t> worstTaskletTicks{ 0 };
			std::atomic<std::uint32_t> taskletCalls{ 0 };
			std::atomic<std::uint32_t> peakRunning{ 0 };
			std::atomic<std::uint32_t> peakSuspended{ 0 };
			std::atomic<std::uint32_t> peakMessages{ 0 };
			std::atomic<std::uint32_t> eventOverflow{ 0 };
			std::atomic<std::uint32_t> targetOverflow{ 0 };
		};
		Counters g_c;

		std::atomic<std::uint32_t> g_nowRunning{ 0 };
		std::atomic<std::uint32_t> g_nowSuspended{ 0 };
		std::atomic<std::uint32_t> g_nowMessages{ 0 };
		std::atomic<std::uint32_t> g_nowDetached{ 0 };
		std::atomic<std::uint32_t> g_statsEvents{ 0 };
		std::atomic<bool> g_overstressed{ false };
		std::atomic<float> g_budget{ 0.0F };
		std::atomic<int> g_taskletsActive{ 0 };
		std::atomic<std::uint32_t> g_taskletsOffMain{ 0 };
		std::atomic<std::uint32_t> g_updatesOffMain{ 0 };
		std::atomic<std::uint32_t> g_tasksToJobsCalls{ 0 };
		std::atomic<std::uint64_t> g_allFrames{ 0 };
		std::atomic<std::uint32_t> g_reports{ 0 };
		std::atomic<bool> g_forceReport{ false };

		DWORD g_mainThread = 0;
		std::atomic<bool> g_started{ false };   // a game has loaded: reports run
		bool g_layoutOK = false;
		std::string g_layoutResult = "not checked yet";
		std::atomic<bool> g_walkOff{ false };
		std::string g_walkOffReason;

		std::uint64_t g_lastTick = 0;
		std::uint64_t g_lastReport = 0;
		std::uint64_t g_lastWalk = 0;
		bool g_lastOverstressed = false;
		bool g_overstressSinceReport = false;
		RE::VMStackID g_lastStackID = 0;
		bool g_haveStackID = false;

		std::mutex g_textLock;
		std::string g_lastReportText;

		template <class T>
		void Raise(std::atomic<T>& a_peak, T a_value)
		{
			T seen = a_peak.load(std::memory_order_relaxed);
			while (a_value > seen && !a_peak.compare_exchange_weak(seen, a_value, std::memory_order_relaxed)) {}
		}

		// ---- events delivered: a fixed table on the pointer of the interned name, so the hook takes no lock ----

		struct EventSlot
		{
			std::atomic<const char*> key{ nullptr };
			std::atomic<std::uint32_t> count{ 0 };
			char name[56]{};
		};
		inline constexpr std::size_t kEventSlots = 256;
		EventSlot g_events[kEventSlots];

		void CountEvent(const char* a_name)
		{
			if (!a_name) { return; }
			const auto start = (reinterpret_cast<std::uintptr_t>(a_name) >> 4) % kEventSlots;
			for (std::size_t i = 0; i < 16; ++i)
			{
				auto& slot = g_events[(start + i) % kEventSlots];
				const char* key = slot.key.load(std::memory_order_acquire);
				if (key == a_name)
				{
					slot.count.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				if (!key)
				{
					const char* expected = nullptr;
					if (slot.key.compare_exchange_strong(expected, a_name))
					{
						strncpy_s(slot.name, a_name, _TRUNCATE);
						slot.count.fetch_add(1, std::memory_order_relaxed);
						return;
					}
					if (expected == a_name)
					{
						slot.count.fetch_add(1, std::memory_order_relaxed);
						return;
					}
				}
			}
			g_c.eventOverflow.fetch_add(1, std::memory_order_relaxed);
		}

		struct TargetSlot
		{
			std::atomic<RE::VMHandle> key{ 0 };
			std::atomic<std::uint32_t> count{ 0 };
		};
		inline constexpr std::size_t kTargetSlots = 2048;
		TargetSlot g_targets[kTargetSlots];

		void CountTarget(RE::VMHandle a_handle)
		{
			if (a_handle == 0) { return; }
			const auto start = static_cast<std::size_t>((a_handle ^ (a_handle >> 32)) * 2654435761u) % kTargetSlots;
			for (std::size_t i = 0; i < 16; ++i)
			{
				auto& slot = g_targets[(start + i) % kTargetSlots];
				RE::VMHandle key = slot.key.load(std::memory_order_acquire);
				if (key == 0)
				{
					RE::VMHandle expected = 0;
					if (slot.key.compare_exchange_strong(expected, a_handle)) { key = a_handle; }
					else { key = expected; }
				}
				if (key == a_handle)
				{
					slot.count.fetch_add(1, std::memory_order_relaxed);
					return;
				}
			}
			g_c.targetOverflow.fetch_add(1, std::memory_order_relaxed);
		}

		// ---- the stats event: per frame, four stores and three comparisons ----

		class StatsSink final : public RE::BSTEventSink<RE::BSScript::StatsEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::BSScript::StatsEvent* a_event, RE::BSTEventSource<RE::BSScript::StatsEvent>*) override
			{
				if (a_event)
				{
					g_nowRunning.store(a_event->runningStacksCount, std::memory_order_relaxed);
					g_nowSuspended.store(a_event->suspendedStacksCount, std::memory_order_relaxed);
					g_nowMessages.store(a_event->functionMsgCount, std::memory_order_relaxed);
					g_nowDetached.store(a_event->detachedObjectCount, std::memory_order_relaxed);
					Raise(g_c.peakRunning, a_event->runningStacksCount);
					Raise(g_c.peakSuspended, a_event->suspendedStacksCount);
					Raise(g_c.peakMessages, a_event->functionMsgCount);
					g_statsEvents.fetch_add(1, std::memory_order_relaxed);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		StatsSink g_statsSink;

		// ---- layout checks ----

		bool QueueLooksRight(const RE::BSTCommonStaticMessageQueue<RE::BSScript::Internal::SuspendedStack, 128>& a_queue)
		{
			return a_queue.numEntries <= 128 && a_queue.pushIdx < 128 && a_queue.popIdx < 128;
		}

		bool CheckLayoutChecks(VM* a_vm, const char*& a_failed)
		{
			// The object really is the VirtualMachine.
			a_failed = "the VM's first vtable pointer is not VirtualMachine's";
			REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_BSScript__Internal__VirtualMachine[0] };
			if (*reinterpret_cast<std::uintptr_t*>(a_vm) != vtable.address()) { return false; }

			// handlePolicy sits at the offset the header says, because it points inside SkyrimVM.
			a_failed = "handlePolicy does not point at SkyrimVM's handle policy";
			auto* skyrimVM = RE::SkyrimVM::GetSingleton();
#if RUNTIME_LINE == 17
			auto* expectedPolicy = &skyrimVM->GetVMRuntimeData().handlePolicy;
#else
			auto* expectedPolicy = &skyrimVM->handlePolicy;
#endif
			if (static_cast<void*>(a_vm->handlePolicy) != static_cast<void*>(expectedPolicy)) { return false; }

			// The four queue pointers point at the queues the header places beside them, one each way.
			a_failed = "the suspend queue pointers do not point at the two suspend queues";
			const bool queuesA = a_vm->stacksToSuspend == &a_vm->suspendQueue1 && a_vm->stacksToResume == &a_vm->suspendQueue2;
			const bool queuesB = a_vm->stacksToSuspend == &a_vm->suspendQueue2 && a_vm->stacksToResume == &a_vm->suspendQueue1;
			if (!queuesA && !queuesB) { return false; }

			a_failed = "the overflow pointers do not point at the two overflow arrays";
			const bool overflowA = a_vm->stacksToSuspendOverflow == &a_vm->overflowSuspendArray1 && a_vm->stacksToResumeOverflow == &a_vm->overflowSuspendArray2;
			const bool overflowB = a_vm->stacksToSuspendOverflow == &a_vm->overflowSuspendArray2 && a_vm->stacksToResumeOverflow == &a_vm->overflowSuspendArray1;
			if (!overflowA && !overflowB) { return false; }

			a_failed = "a suspend queue's counters are out of range";
			if (!QueueLooksRight(a_vm->suspendQueue1) || !QueueLooksRight(a_vm->suspendQueue2)) { return false; }

			// The script type table holds a type we know, under its own name.
			a_failed = "the script type table does not hold Actor under its own name";
			{
				RE::BSSpinLockGuard guard(a_vm->typeInfoLock);
				const auto found = a_vm->objectTypeMap.find(RE::BSFixedString("Actor"));
				if (found == a_vm->objectTypeMap.end() || !found->second || _stricmp(found->second->GetName(), "Actor") != 0) { return false; }
			}

			// Each running stack in the table carries its own key, and its top frame knows its stack.
			a_failed = "a running stack does not match its table entry";
			{
				RE::BSSpinLockGuard guard(a_vm->runningStacksLock);
				int checked = 0;
				for (const auto& [id, stack] : a_vm->allRunningStacks)
				{
					if (!stack || stack->stackID != id) { return false; }
					if (stack->state.get() != Stack::State::kRunning && stack->frames > 0 && stack->top && stack->top->parent != stack.get()) { return false; }
					if (++checked >= 64) { break; }
				}
			}
			a_failed = "";
			return true;
		}

		bool CheckLayoutGuarded(VM* a_vm, const char*& a_failed)
		{
			__try
			{
				return CheckLayoutChecks(a_vm, a_failed);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		void CheckLayout()
		{
			g_layoutOK = false;
			if (!RE::SkyrimVM::GetSingleton()) { g_layoutResult = "SkyrimVM singleton is null"; return; }
			auto* vm = VM::GetSingleton();
			if (!vm) { g_layoutResult = "VirtualMachine singleton is null"; return; }
			const char* failed = "the check itself faulted";
			const bool ok = CheckLayoutGuarded(vm, failed);
			g_layoutOK = ok;
			g_layoutResult = ok ? "all checks passed" : failed;
		}

		// ---- the walk ----

		inline constexpr std::array<const char*, 9> kStateNames{
			"running", "finished", "waiting on memory", "waiting on a latent function", "waiting on another stack for a call",
			"waiting on another stack for a return", "waiting on another stack for a return (no pop)", "retrying a return", "retrying a call"
		};

		struct Tally
		{
			std::uint32_t samples = 0;
			std::uint64_t stacksSeen = 0;
			std::uint64_t unnamed = 0;
			std::uint32_t capped = 0;
			std::uint64_t walkTicks = 0;
			std::uint64_t worstWalkTicks = 0;
			std::array<std::uint64_t, kStateNames.size()> states{};
			std::unordered_map<std::string, std::uint32_t> byFunction;   // "script.function": what began the stack
			std::unordered_map<std::string, std::uint32_t> byCall;       // "script -> function": what a stack is calling right now
			std::unordered_map<RE::VMHandle, std::uint32_t> byHandle;
			std::unordered_map<RE::VMHandle, std::uint32_t> perWalk;     // stacks per object at the current walk, an effect counting for its actor
			std::unordered_map<std::string, std::pair<std::uint32_t, std::uint32_t>> deep;   // "script.function": frames, frames read in one function
			std::unordered_set<std::string> edgeKeys;
			std::vector<std::pair<std::string, std::string>> edges;
			std::uint64_t longestWaitTicks = 0;
			std::string longestWaitWhat;
		};
		Tally g_tally;

		// When each stack was first seen waiting. A stack carries no clock, so an age is "seen at an earlier walk and still here":
		// it is at least this old, and a stack that came and went between walks has no age at all.
		std::unordered_map<RE::VMStackID, std::uint64_t> g_firstSeen;
		std::unordered_map<RE::VMStackID, std::uint64_t> g_firstSeenNext;
		std::uint64_t g_walkNow = 0;

		struct Entry
		{
			StackPtr stack;
			QueryPtr query;
		};

		// A type check is not a validity check, and GetObject() writes a refcount through the value (AMF 2.2.1's crash). So the
		// value word is judged first and read without a copy.
		RE::VMHandle HandleOf(const RE::BSScript::Variable& a_var)
		{
			static_assert(sizeof(RE::BSScript::Variable) == 0x10);
			if (!a_var.IsObject()) { return 0; }
			const auto word = reinterpret_cast<const std::uintptr_t*>(&a_var)[1];
			if (word < 0x10000 || word >= 0x0000800000000000ULL || (word & 7) != 0) { return 0; }
			return reinterpret_cast<const RE::BSScript::Object*>(word)->GetHandle();
		}

		void NoteOn(RE::VMHandle a_handle)
		{
			g_tally.byHandle[a_handle]++;
			g_tally.perWalk[LoopGuard::StackKey(a_handle)]++;
		}

		void AddEdge(const char* a_caller, const char* a_callee)
		{
			if (!a_caller || !a_callee) { return; }
			std::string key = std::string(a_caller) + "|" + a_callee;
			if (g_tally.edgeKeys.insert(std::move(key)).second) { g_tally.edges.emplace_back(a_caller, a_callee); }
		}

		void NoteAge(RE::VMStackID a_id, const std::string& a_what)
		{
			const auto found = g_firstSeen.find(a_id);
			const auto first = found != g_firstSeen.end() ? found->second : g_walkNow;
			g_firstSeenNext[a_id] = first;
			const auto age = g_walkNow - first;
			if (age > g_tally.longestWaitTicks)
			{
				g_tally.longestWaitTicks = age;
				g_tally.longestWaitWhat = a_what;
			}
		}

		// Called under SEH. Everything read from the stack is read here.
		void TallyEntry(const Stack* a_stack, RE::BSScript::Internal::IFuncCallQuery* a_query)
		{
			g_tally.stacksSeen++;
			const auto state = static_cast<std::size_t>(a_stack->state.underlying());
			if (state < g_tally.states.size()) { g_tally.states[state]++; }

			RE::BSFixedString requested;
			RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> requestedType;
			RE::BSScript::Variable requestedSelf;
			RE::BSScrapArray<RE::BSScript::Variable> requestedArgs;
			bool haveRequest = false;
			if (a_query)
			{
				RE::BSScript::Internal::IFuncCallQuery::CallType type{};
				haveRequest = a_query->GetFunctionCallInfo(type, requestedType, requested, requestedSelf, requestedArgs);
			}

			const Frame* top = a_stack->frames > 0 ? a_stack->top : nullptr;
			if (top && top->parent != a_stack) { top = nullptr; }

			if (top)
			{
				// The bottom frame says what began this stack, the top frame what it is doing. Frames are read top down.
				const Frame* bottom = top;
				const Frame* frame = top;
				int depth = 0;
				std::array<const RE::BSScript::IFunction*, 64> functions{};
				const char* calleeName = haveRequest && requested.data() ? requested.data() : nullptr;
				bool topOfStack = true;
				while (frame && depth < 64)
				{
					const auto* fn = frame->owningFunction.get();
					functions[static_cast<std::size_t>(depth)] = fn;
					if (fn)
					{
						if (calleeName)
						{
							AddEdge(fn->GetObjectTypeName().data(), calleeName);
							if (topOfStack) { g_tally.byCall[std::format("{} -> {}", fn->GetObjectTypeName().c_str(), calleeName)]++; }
						}
						calleeName = fn->GetName().data();
						topOfStack = false;
					}
					bottom = frame;
					frame = frame->previousFrame;
					++depth;
				}
				const auto* fn = bottom->owningFunction.get();
				if (fn)
				{
					auto what = std::format("{}.{}", fn->GetObjectTypeName().c_str(), fn->GetName().c_str());
					NoteAge(a_stack->stackID, what);
					g_tally.byFunction[std::move(what)]++;
				}
				else
				{
					g_tally.unnamed++;
				}
				// An effect that has ended leaves self empty on a stack still waiting to start (nearly every stack of the 2026-11-04
				// flood), but the start's first parameter is still its target.
				auto handle = HandleOf(bottom->self);
				if (handle == 0 && fn && fn->GetParamCount() > 0 && (_stricmp(fn->GetName().c_str(), "OnEffectStart") == 0 || _stricmp(fn->GetName().c_str(), "OnEffectFinish") == 0))
				{
					auto* writable = const_cast<Stack*>(a_stack);
					handle = HandleOf(writable->GetStackFrameVariable(bottom, 0, writable->GetPageForFrame(bottom)));
				}
				if (handle) { NoteOn(handle); }

				// Recursion: a deep stack, or one function many times on it. The frames read stop at 64.
				const auto& loop = LoopGuard::GetSettings();
				if (fn && (a_stack->frames >= static_cast<std::uint32_t>(loop.deepFrames) || depth >= loop.sameFunctionFrames))
				{
					std::uint32_t most = 0;
					for (int i = 0; i < depth; ++i)
					{
						std::uint32_t same = 0;
						for (int j = i; j < depth; ++j)
						{
							if (functions[static_cast<std::size_t>(j)] == functions[static_cast<std::size_t>(i)]) { same++; }
						}
						most = std::max(most, same);
					}
					if (a_stack->frames >= static_cast<std::uint32_t>(loop.deepFrames) || most >= static_cast<std::uint32_t>(loop.sameFunctionFrames))
					{
						g_tally.deep[std::format("{}.{}", fn->GetObjectTypeName().c_str(), fn->GetName().c_str())] = { a_stack->frames, most };
					}
				}
				return;
			}

			if (haveRequest && requestedType)
			{
				auto what = std::format("{}.{}", requestedType->GetName(), requested.c_str());
				NoteAge(a_stack->stackID, what);
				g_tally.byFunction[std::move(what)]++;
				auto handle = HandleOf(requestedSelf);
				if (handle == 0 && !requestedArgs.empty()) { handle = HandleOf(requestedArgs[0]); }
				if (handle) { NoteOn(handle); }
				return;
			}
			g_tally.unnamed++;
		}

		bool SafeTally(const Stack* a_stack, RE::BSScript::Internal::IFuncCallQuery* a_query)
		{
			__try
			{
				TallyEntry(a_stack, a_query);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		void TurnWalkOff(std::string a_reason)
		{
			if (!g_walkOff.exchange(true))
			{
				g_walkOffReason = std::move(a_reason);
				logger::error("Stack walk turned off for this session: {}. The counters carry on; scripts and targets will not be listed.", g_walkOffReason);
			}
		}

		void CopySuspended(VM* a_vm, std::vector<Entry>& a_out, std::size_t a_cap)
		{
			RE::BSSpinLockGuard guard(a_vm->suspendQueueLock);
			const auto take = [&](const RE::BSTCommonStaticMessageQueue<RE::BSScript::Internal::SuspendedStack, 128>* a_queue, const RE::BSTArray<RE::BSScript::Internal::SuspendedStack>* a_overflow) {
				if (a_queue && QueueLooksRight(*a_queue))
				{
					const auto* items = reinterpret_cast<const RE::BSScript::Internal::SuspendedStack*>(a_queue->queueBuffer);
					for (std::uint32_t i = 0; i < a_queue->numEntries && a_out.size() < a_cap; ++i)
					{
						const auto& item = items[(a_queue->popIdx + i) % 128];
						a_out.push_back({ item.stack, item.funcCallQuery });
					}
				}
				if (a_overflow)
				{
					for (const auto& item : *a_overflow)
					{
						if (a_out.size() >= a_cap) { break; }
						a_out.push_back({ item.stack, item.funcCallQuery });
					}
				}
			};
			take(a_vm->stacksToSuspend, a_vm->stacksToSuspendOverflow);
			take(a_vm->stacksToResume, a_vm->stacksToResumeOverflow);
		}

		void CopyWaitingMessages(VM* a_vm, std::vector<Entry>& a_out, std::size_t a_cap)
		{
			RE::BSSpinLockGuard guard(a_vm->funcQueueLock);
			for (const auto& message : a_vm->overflowFuncMsgs)
			{
				if (a_out.size() >= a_cap) { break; }
				if (message.stack) { a_out.push_back({ message.stack, message.funcQuery }); }
			}
		}

		void CopyWaitingStacks(VM* a_vm, std::vector<Entry>& a_out, std::size_t a_cap, const std::unordered_set<RE::VMStackID>& a_skip)
		{
			RE::BSSpinLockGuard guard(a_vm->runningStacksLock);
			for (const auto& [id, stack] : a_vm->allRunningStacks)
			{
				if (a_out.size() >= a_cap) { break; }
				if (!stack || a_skip.contains(id)) { continue; }
				if (stack->state.get() == Stack::State::kRunning) { continue; }   // could be executing: leave it alone
				if (stack->frames == 0) { continue; }                             // nothing to read but a request, which only the queues hold
				a_out.push_back({ stack, {} });
			}
		}

		void Walk(VM* a_vm)
		{
			if (g_walkOff.load() || !g_layoutOK) { return; }
			if (GetCurrentThreadId() != g_mainThread) { return; }
			if (g_taskletsActive.load(std::memory_order_acquire) != 0) { return; }
			if (a_vm->freezeState.get() != VM::FreezeState::kNotFrozen) { return; }

			const auto start = Now();
			g_walkNow = start;
			const auto deadline = start + static_cast<std::uint64_t>(g_settings.maxWalkMS * g_ticksPerMS);
			const auto cap = static_cast<std::size_t>(g_settings.maxStacksPerWalk);

			g_tally.perWalk.clear();
			std::vector<Entry> entries;
			entries.reserve(std::min<std::size_t>(cap, 1024));
			CopySuspended(a_vm, entries, cap);
			CopyWaitingMessages(a_vm, entries, cap);
			std::unordered_set<RE::VMStackID> seen;
			seen.reserve(entries.size());
			for (const auto& entry : entries)
			{
				if (entry.stack) { seen.insert(entry.stack->stackID); }
			}
			if (g_tasksToJobsCalls.load(std::memory_order_relaxed) == 0) { CopyWaitingStacks(a_vm, entries, cap, seen); }

			bool capped = entries.size() >= cap;
			std::size_t done = 0;
			g_firstSeenNext.clear();
			for (const auto& entry : entries)
			{
				if (!entry.stack) { continue; }
				if (!SafeTally(entry.stack.get(), entry.query.get()))
				{
					TurnWalkOff("reading a stack raised an exception");
					break;
				}
				if ((++done & 63) == 0 && Now() > deadline)
				{
					capped = true;
					break;
				}
			}
			g_firstSeen.swap(g_firstSeenNext);

			LoopGuard::OnWalk(a_vm, g_tally.perWalk);
			for (const auto& [function, numbers] : g_tally.deep) { LoopGuard::NoteDeepStack(function, numbers.first, numbers.second); }

			const auto spent = Now() - start;
			g_tally.samples++;
			g_tally.walkTicks += spent;
			g_tally.worstWalkTicks = std::max(g_tally.worstWalkTicks, spent);
			if (capped) { g_tally.capped++; }
			logger::debug("Walk: {} entries, {} read, {:.2f} ms{}", entries.size(), done, ToMS(spent), capped ? ", capped" : "");
		}

		// ---- the report ----

		// Names the object a handle points at. Plain data only, so a stale handle cannot leave anything half done.
		bool DescribeRaw(RE::BSScript::IObjectHandlePolicy* a_policy, RE::VMHandle a_handle, char* a_out, std::size_t a_size)
		{
			__try
			{
				RE::Actor* player = RE::PlayerCharacter::GetSingleton();
				const auto effectType = RE::ActiveEffect::VMTYPEID;
				RE::TESObjectREFR* ref = nullptr;
				const char* what = "";
				if (a_policy->HandleIsType(effectType, a_handle))
				{
					auto* effect = static_cast<RE::ActiveEffect*>(a_policy->GetObjectForHandle(effectType, a_handle));
					ref = effect ? effect->GetTargetActor() : nullptr;
					if (!ref)
					{
						strcpy_s(a_out, a_size, "an active effect that has ended");
						return true;
					}
					what = "effect on ";
				}
				else
				{
					const RE::VMTypeID types[] = { static_cast<RE::VMTypeID>(RE::FormType::ActorCharacter), static_cast<RE::VMTypeID>(RE::FormType::Reference) };
					for (const auto type : types)
					{
						if (a_policy->HandleIsType(type, a_handle))
						{
							ref = static_cast<RE::TESObjectREFR*>(a_policy->GetObjectForHandle(type, a_handle));
							if (ref) { break; }
						}
					}
				}
				if (!ref) { return false; }
				const char* name = ref->GetName();
				char distance[40] = "";
				if (player && ref->GetParentCell() && player->GetParentCell())
				{
					sprintf_s(distance, ", %.0f units away", player->GetPosition().GetDistance(ref->GetPosition()));
				}
				sprintf_s(a_out, a_size, "%s%s (%08X)%s", what, name && name[0] ? name : "unnamed", ref->GetFormID(), distance);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		std::string DescribeHandle(VM* a_vm, RE::VMHandle a_handle)
		{
			auto* policy = a_vm ? a_vm->handlePolicy : nullptr;
			if (!policy || !g_layoutOK) { return std::format("handle {:016X}", a_handle); }
			char text[160] = "";
			if (DescribeRaw(policy, a_handle, text, sizeof(text))) { return text; }
			// Quests, aliases and the rest: the game's own text for a handle, as in its stack dumps.
			RE::BSFixedString asText;
			policy->ConvertHandleToString(a_handle, asText);
			return asText.empty() ? std::format("handle {:016X}", a_handle) : std::string(asText.c_str());
		}

		template <class Map>
		std::vector<std::pair<typename Map::key_type, std::uint64_t>> TopOf(const Map& a_map, std::size_t a_rows)
		{
			std::vector<std::pair<typename Map::key_type, std::uint64_t>> rows;
			rows.reserve(a_map.size());
			for (const auto& [key, count] : a_map) { rows.emplace_back(key, count); }
			const auto n = std::min(a_rows, rows.size());
			std::partial_sort(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(n), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			rows.resize(n);
			return rows;
		}

		std::vector<std::string> LoadedScriptNames(VM* a_vm)
		{
			std::vector<std::string> names;
			RE::BSSpinLockGuard guard(a_vm->typeInfoLock);
			names.reserve(a_vm->objectTypeMap.size());
			for (const auto& [name, type] : a_vm->objectTypeMap) { names.emplace_back(name.c_str()); }
			return names;
		}

		void Report(VM* a_vm, const char* a_why, std::uint64_t a_now)
		{
			const auto reportStart = Now();
			const double seconds = ToMS(a_now - g_lastReport) / 1000.0;
			const auto rows = static_cast<std::size_t>(g_settings.topRows);

			const auto frames = g_c.frames.exchange(0);
			const auto paused = g_c.pausedFrames.exchange(0);
			const auto overFrames = g_c.overstressedFrames.exchange(0);
			const auto overOn = g_c.overstressOn.exchange(0);
			const auto overOff = g_c.overstressOff.exchange(0);
			const auto frameTicks = g_c.frameTicks.exchange(0);
			const auto worstFrame = g_c.worstFrameTicks.exchange(0);
			const auto updateTicks = g_c.updateTicks.exchange(0);
			const auto worstUpdate = g_c.worstUpdateTicks.exchange(0);
			const auto taskletTicks = g_c.taskletTicks.exchange(0);
			const auto worstTasklet = g_c.worstTaskletTicks.exchange(0);
			const auto taskletCalls = g_c.taskletCalls.exchange(0);
			const auto peakRunning = g_c.peakRunning.exchange(0);
			const auto peakSuspended = g_c.peakSuspended.exchange(0);
			const auto peakMessages = g_c.peakMessages.exchange(0);
			const auto allFrames = frames + paused;

			std::string text;
			const auto line = [&](std::string a_line) {
				logger::info("{}", a_line);
				text += a_line;
				text += '\n';
			};

			line(std::format("==== Report: {} ({:.0f} s since the last) ====", a_why, seconds));
			line(std::format("Frames: {} playing, {} paused | {:.1f} FPS | frame avg {:.2f} ms, worst {:.1f} ms",
				frames, paused, frameTicks > 0 ? allFrames * 1000.0 / ToMS(frameTicks) : 0.0, allFrames ? ToMS(frameTicks) / allFrames : 0.0, ToMS(worstFrame)));
			line(std::format("Overstress: {} frames of {} ({:.0f}%) | turned on {} times, off {} | on now: {}",
				overFrames, allFrames, allFrames ? overFrames * 100.0 / allFrames : 0.0, overOn, overOff, g_overstressed.load() ? "yes" : "no"));
			line(std::format("VM time: update avg {:.3f} ms, worst {:.2f} ms, budget it was given {:.2f} ms | tasklets avg {:.3f} ms per frame, worst call {:.2f} ms, {} calls",
				allFrames ? ToMS(updateTicks) / allFrames : 0.0, ToMS(worstUpdate), g_budget.load(), allFrames ? ToMS(taskletTicks) / allFrames : 0.0, ToMS(worstTasklet), taskletCalls));

			std::string extra;
			if (g_layoutOK && a_vm)
			{
				const auto nextID = a_vm->nextStackID;
				if (g_haveStackID)
				{
					const auto made = nextID - g_lastStackID;
					extra = std::format(" | new stacks {} ({:.0f}/s)", made, seconds > 0.0 ? made / seconds : 0.0);
				}
				g_lastStackID = nextID;
				g_haveStackID = true;
				extra += std::format(" | waiting on latent {} | tasklets {}", a_vm->waitingLatentReturns.size(), a_vm->vmTasks.size());
			}
			line(std::format("Stacks: running {} (peak {}) | suspended {} (peak {}) | queued calls {} (peak {}) | objects to clean {}{}",
				g_nowRunning.load(), peakRunning, g_nowSuspended.load(), peakSuspended, g_nowMessages.load(), peakMessages, g_nowDetached.load(), extra));

			// Events delivered through SendEvent, and the objects they went to.
			std::vector<std::pair<std::string, std::uint64_t>> events;
			std::uint64_t eventTotal = 0;
			for (auto& slot : g_events)
			{
				const auto count = slot.count.exchange(0, std::memory_order_relaxed);
				if (count)
				{
					events.emplace_back(slot.name, count);
					eventTotal += count;
				}
			}
			std::ranges::sort(events, [](const auto& a, const auto& b) { return a.second > b.second; });
			if (events.size() > rows) { events.resize(rows); }
			std::string eventText;
			for (const auto& [name, count] : events)
			{
				if (!eventText.empty()) { eventText += ", "; }
				eventText += std::format("{} {}", name, count);
			}
			const auto eventOver = g_c.eventOverflow.exchange(0);
			line(std::format("Events sent: {}{}{}", eventTotal, eventText.empty() ? "" : " | ", eventText) + (eventOver ? std::format(" | {} not counted, table full", eventOver) : ""));

			std::vector<std::pair<RE::VMHandle, std::uint64_t>> targets;
			for (auto& slot : g_targets)
			{
				const auto handle = slot.key.exchange(0, std::memory_order_relaxed);
				const auto count = slot.count.exchange(0, std::memory_order_relaxed);
				if (handle && count) { targets.emplace_back(handle, count); }
			}
			const auto topTargetsN = std::min<std::size_t>(5, targets.size());
			std::partial_sort(targets.begin(), targets.begin() + static_cast<std::ptrdiff_t>(topTargetsN), targets.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			targets.resize(topTargetsN);
			for (const auto& [handle, count] : targets)
			{
				line(std::format("  events to {}: {}", DescribeHandle(a_vm, handle), count));
			}

			// The stack walk.
			if (g_walkOff.load())
			{
				line(std::format("Walk: off for this session: {}", g_walkOffReason));
			}
			else if (!g_layoutOK)
			{
				line(std::format("Walk: off, layout check failed: {}", g_layoutResult));
			}
			else if (g_tally.samples == 0)
			{
				line("Walk: not run in this period");
			}
			else
			{
				line(std::format("Walk: {} samples, {} stacks seen, {} with no name, {} stopped at the cap | cost avg {:.2f} ms, worst {:.2f} ms",
					g_tally.samples, g_tally.stacksSeen, g_tally.unnamed, g_tally.capped, ToMS(g_tally.walkTicks) / g_tally.samples, ToMS(g_tally.worstWalkTicks)));
				if (g_tasksToJobsCalls.load(std::memory_order_relaxed) != 0)
				{
					line(std::format("Walk covers the suspend queues and overflowed calls only: the game ran its tasks as jobs ({} times since start), so the table of running stacks is left alone", g_tasksToJobsCalls.load()));
				}

				std::string waiting;
				for (std::size_t i = 0; i < g_tally.states.size(); ++i)
				{
					if (g_tally.states[i] == 0) { continue; }
					if (!waiting.empty()) { waiting += ", "; }
					waiting += std::format("{} {}", kStateNames[i], g_tally.states[i]);
				}
				if (!waiting.empty()) { line(std::format("Waiting on: {}", waiting)); }
				if (g_tally.longestWaitTicks > 0)
				{
					line(std::format("Longest seen waiting: at least {:.1f} s, {}", ToMS(g_tally.longestWaitTicks) / 1000.0, g_tally.longestWaitWhat));
				}

				line("Began by:");
				for (const auto& [name, count] : TopOf(g_tally.byFunction, rows))
				{
					const auto dot = name.find('.');
					line(std::format("  {:>7}  {}  [{}]", count, name, dot == std::string::npos ? std::string("role pending") : Roles::Describe(name.substr(0, dot), name.substr(dot + 1))));
				}
				const auto calls = TopOf(g_tally.byCall, rows);
				if (!calls.empty())
				{
					line("Calling now (caller -> what it waits on):");
					for (const auto& [name, count] : calls) { line(std::format("  {:>7}  {}", count, name)); }
				}
				line("On:");
				for (const auto& [handle, count] : TopOf(g_tally.byHandle, rows))
				{
					line(std::format("  {:>7}  {}", count, DescribeHandle(a_vm, handle)));
				}
			}
			for (auto& loopLine : LoopGuard::ReportLines()) { line(std::move(loopLine)); }
			Roles::Audit(g_tally.edges);
			g_tally = Tally{};

			if (a_vm && g_layoutOK)
			{
				// New script types since the last time. The classifier takes them on its own thread.
				Roles::OfferLoadedScripts(LoadedScriptNames(a_vm));
			}
			line(Roles::SummaryLine());
			line(std::format("This report cost {:.2f} ms", ToMS(Now() - reportStart)));
			spdlog::default_logger()->flush();

			g_reports.fetch_add(1, std::memory_order_relaxed);
			g_lastReport = a_now;
			{
				std::scoped_lock l(g_textLock);
				g_lastReportText = std::move(text);
			}
		}

		// ---- the hooks ----

		void Tick(VM* a_vm, float a_budget)
		{
			const auto now = Now();
			g_budget.store(a_budget, std::memory_order_relaxed);
			g_allFrames.fetch_add(1, std::memory_order_relaxed);
			if (GetCurrentThreadId() != g_mainThread)
			{
				g_updatesOffMain.fetch_add(1, std::memory_order_relaxed);
				return;
			}

			if (g_lastTick)
			{
				const auto delta = now - g_lastTick;
				g_c.frameTicks.fetch_add(delta, std::memory_order_relaxed);
				Raise(g_c.worstFrameTicks, delta);
			}
			g_lastTick = now;

			auto* ui = RE::UI::GetSingleton();
			if (ui && ui->GameIsPaused()) { g_c.pausedFrames.fetch_add(1, std::memory_order_relaxed); }
			else { g_c.frames.fetch_add(1, std::memory_order_relaxed); }

			const bool over = g_overstressed.load(std::memory_order_relaxed);
			if (over) { g_c.overstressedFrames.fetch_add(1, std::memory_order_relaxed); }

			if (!g_started.load(std::memory_order_relaxed)) { return; }
			if (g_lastReport == 0) { g_lastReport = now; }

			LoopGuard::Tick(a_vm, over, g_layoutOK, now, g_ticksPerMS * 1000.0);

			const auto ticksPerSecond = g_ticksPerMS * 1000.0;
			const auto since = [&](std::uint64_t a_then) { return static_cast<double>(now - a_then) / ticksPerSecond; };
			const bool began = over && !g_lastOverstressed;
			g_lastOverstressed = over;
			if (began) { g_overstressSinceReport = true; }

			const bool forced = g_forceReport.exchange(false, std::memory_order_relaxed);
			const bool intervalUp = since(g_lastReport) >= g_settings.intervalSeconds;
			const bool overstressReport = g_overstressSinceReport && since(g_lastReport) >= g_settings.overstressGapSeconds;

			if (began || (over && since(g_lastWalk) >= g_settings.overstressSampleSeconds) || intervalUp || forced || overstressReport)
			{
				Walk(a_vm);
				g_lastWalk = now;
			}
			if (forced) { Report(a_vm, "asked for", now); }
			else if (overstressReport) { Report(a_vm, "overstress", now); }
			else if (intervalUp) { Report(a_vm, "interval", now); }
			if (overstressReport) { g_overstressSinceReport = false; }
		}

		struct UpdateHook
		{
			static void thunk(VM* a_vm, float a_budget)
			{
				Tick(a_vm, a_budget);
				const auto start = Now();
				func(a_vm, a_budget);
				const auto spent = Now() - start;
				g_c.updateTicks.fetch_add(spent, std::memory_order_relaxed);
				Raise(g_c.worstUpdateTicks, spent);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct TaskletsHook
		{
			static void thunk(VM* a_vm, float a_budget)
			{
				g_taskletsActive.fetch_add(1, std::memory_order_acq_rel);
				if (GetCurrentThreadId() != g_mainThread) { g_taskletsOffMain.fetch_add(1, std::memory_order_relaxed); }
				const auto start = Now();
				func(a_vm, a_budget);
				const auto spent = Now() - start;
				g_taskletsActive.fetch_sub(1, std::memory_order_acq_rel);
				g_c.taskletTicks.fetch_add(spent, std::memory_order_relaxed);
				g_c.taskletCalls.fetch_add(1, std::memory_order_relaxed);
				Raise(g_c.worstTaskletTicks, spent);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct OverstressHook
		{
			static void thunk(VM* a_vm, bool a_set)
			{
				const bool was = g_overstressed.exchange(a_set, std::memory_order_relaxed);
				if (a_set && !was) { g_c.overstressOn.fetch_add(1, std::memory_order_relaxed); }
				if (!a_set && was) { g_c.overstressOff.fetch_add(1, std::memory_order_relaxed); }
				func(a_vm, a_set);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct TasksToJobsHook
		{
			static void thunk(VM* a_vm, RE::BSJobs::JobList& a_list)
			{
				g_tasksToJobsCalls.fetch_add(1, std::memory_order_relaxed);
				func(a_vm, a_list);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct SendEventHook
		{
			static void thunk(VM* a_vm, RE::VMHandle a_handle, const RE::BSFixedString& a_name, RE::BSScript::IFunctionArguments* a_args)
			{
				CountEvent(a_name.data());
				CountTarget(a_handle);
				if (LoopGuard::OnSendEvent(a_vm, a_handle, a_name.data())) { return; }
				func(a_vm, a_handle, a_name, a_args);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct SendEventAllHook
		{
			static void thunk(VM* a_vm, const RE::BSFixedString& a_name, RE::BSScript::IFunctionArguments* a_args)
			{
				CountEvent(a_name.data());
				func(a_vm, a_name, a_args);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Configure(const Settings& a_settings) { g_settings = a_settings; }

	void InstallHooks()
	{
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		g_ticksPerMS = static_cast<double>(frequency.QuadPart) / 1000.0;

		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_BSScript__Internal__VirtualMachine[0] };
		UpdateHook::func = vtable.write_vfunc(0x4, UpdateHook::thunk);
		TaskletsHook::func = vtable.write_vfunc(0x5, TaskletsHook::thunk);
		OverstressHook::func = vtable.write_vfunc(0x6, OverstressHook::thunk);
		TasksToJobsHook::func = vtable.write_vfunc(0x12, TasksToJobsHook::thunk);
		SendEventHook::func = vtable.write_vfunc(0x24, SendEventHook::thunk);
		LoopGuard::SetOriginalSend(reinterpret_cast<LoopGuard::SendFn>(SendEventHook::func.address()));
		SendEventAllHook::func = vtable.write_vfunc(0x25, SendEventAllHook::thunk);
		logger::info("Hooks installed on VirtualMachine at {:X}: Update (4), UpdateTasklets (5), SetOverstressed (6), TasksToJobs (12), SendEvent (24), SendEventAll (25); each calls the original unchanged, SendEvent unless the loop guard is on and drops a repeat", vtable.address());
	}

	void OnDataLoaded()
	{
		g_mainThread = GetCurrentThreadId();
		CheckLayout();
		if (g_layoutOK) { logger::info("VM layout checks: {}", g_layoutResult); }
		else { logger::error("VM layout check FAILED: {}. The stack walk is off and this log will say so; the counters still run.", g_layoutResult); }

		if (auto* vm = VM::GetSingleton())
		{
			vm->RegisterForStatsEvent(&g_statsSink);
			logger::info("Joined the VM's stats event");
		}
	}

	void OnGameLoaded()
	{
		// Checked again: the type table is fuller now, and a stack may exist to compare.
		CheckLayout();
		if (!g_layoutOK) { logger::error("VM layout check FAILED after loading: {}. The stack walk is off.", g_layoutResult); }
		else { logger::debug("VM layout checks after loading: {}", g_layoutResult); }

		if (auto* vm = VM::GetSingleton(); vm && g_layoutOK)
		{
			auto names = LoadedScriptNames(vm);
			logger::info("{} script types loaded", names.size());
			Roles::OfferLoadedScripts(std::move(names));
		}
		g_started.store(true);
	}

	void RequestReport() { g_forceReport.store(true); }

	std::string StateJson()
	{
		// Runs on DevBench's thread: only atomics and copied text, nothing from the VM itself.
		std::string report;
		{
			std::scoped_lock l(g_textLock);
			report = g_lastReportText;
		}
		std::string escaped;
		escaped.reserve(report.size() + 32);
		for (const char c : report)
		{
			switch (c)
			{
			case '"': escaped += "\\\""; break;
			case '\\': escaped += "\\\\"; break;
			case '\n': escaped += "\\n"; break;
			case '\r': break;
			case '\t': escaped += "\\t"; break;
			default:
				if (static_cast<unsigned char>(c) >= 0x20) { escaped += c; }
				break;
			}
		}
		return std::format(
			R"({{"ok":true,"started":{},"layoutOK":{},"layoutResult":"{}","walkOff":{},"overstressed":{},"budgetMS":{:.3f},"running":{},"suspended":{},"queuedCalls":{},"detached":{},"statsEvents":{},"frames":{},"updatesOffMain":{},"taskletCallsOffMain":{},"tasksToJobsCalls":{},"reports":{},{},{},"lastReport":"{}"}})",
			g_started.load(), g_layoutOK, g_layoutResult, g_walkOff.load(), g_overstressed.load(), g_budget.load(),
			g_nowRunning.load(), g_nowSuspended.load(), g_nowMessages.load(), g_nowDetached.load(), g_statsEvents.load(), g_allFrames.load(),
			g_updatesOffMain.load(), g_taskletsOffMain.load(), g_tasksToJobsCalls.load(), g_reports.load(), Roles::StateJsonFragment(), LoopGuard::StateJsonFragment(), escaped);
	}
}
