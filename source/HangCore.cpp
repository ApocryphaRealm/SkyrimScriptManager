// The hang recorder's watcher and report. Own code, GPL-3.0-or-later (2026-11-05).
//
// What it may do once a stall is seen: read memory with ReadProcessMemory (no fault can reach a crash logger's handler), make plain
// kernel calls, and format into static buffers. What it may not do: allocate, use spdlog, take the loader lock, call the game. A stuck
// thread can hold any of those, and then the report would stick with it.
//
// While a thread is suspended, only GetThreadContext, VirtualQuery and ReadProcessMemory run, then it is resumed. It is held for
// microseconds and one thread at a time, and the report gives the longest hold. The stack is SCANNED, not walked: a walk needs the
// unwind tables' locks, which a suspended thread could hold. So old values can appear; each entry at least follows a call instruction.
#include "HangCore.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>

#pragma comment(lib, "user32.lib")

namespace HangCore
{
	namespace
	{
		inline constexpr std::size_t kMaxModules = 1024;
		inline constexpr std::size_t kMaxThreads = 512;
		inline constexpr std::size_t kMainStackBytes = 128 * 1024;
		inline constexpr std::size_t kOtherStackBytes = 32 * 1024;
		inline constexpr std::size_t kMainFrames = 64;
		inline constexpr std::size_t kOtherFrames = 20;
		inline constexpr std::size_t kFullStacksShown = 40;   // distinct stacks past the main thread's
		inline constexpr std::uint32_t kMaxFullReports = 200; // every slow load or alt-tab can write one; this keeps the log under a few MB and a real freeze still in it

		State g_state;
		Hooks g_hooks;
		HANDLE g_log = INVALID_HANDLE_VALUE;
		double g_stallSeconds = 15.0;
		double g_secondGapSeconds = 60.0;

		std::atomic<std::uint32_t> g_mainThread{ 0 };
		std::atomic<const char*> g_mainHow{ "not known yet" };

		std::atomic<std::uint32_t> g_stalls{ 0 };
		std::atomic<std::uint32_t> g_reports{ 0 };
		std::atomic<double> g_lastStallSeconds{ 0.0 };
		std::atomic<double> g_ringCost{ 0.0 };
		std::atomic<double> g_worstSuspend{ 0.0 };
		std::atomic<double> g_lastReportMS{ 0.0 };
		std::atomic<bool> g_armed{ false };
		std::atomic<bool> g_stalled{ false };

		double g_tscPerMS = 1.0;
		double g_qpcPerMS = 1.0;

		// ---- the output: one static buffer, its own handle, flushed to disk at each stage ----

		char g_out[256 * 1024];
		std::size_t g_outUsed = 0;

		void Flush()
		{
			if (g_outUsed == 0) { return; }
			if (g_log != INVALID_HANDLE_VALUE)
			{
				DWORD written = 0;
				WriteFile(g_log, g_out, static_cast<DWORD>(g_outUsed), &written, nullptr);
				FlushFileBuffers(g_log);
			}
			g_outUsed = 0;
		}

		std::uint64_t Qpc()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return static_cast<std::uint64_t>(t.QuadPart);
		}

		double QpcMS(std::uint64_t a_from, std::uint64_t a_to) { return static_cast<double>(a_to - a_from) / g_qpcPerMS; }

		// ---- the module table: built while the game is healthy, read during a stall ----

		struct Module
		{
			std::uintptr_t base;
			std::uintptr_t end;
			bool ours;   // the game's exe, SKSE or an SKSE plugin
			char name[48];
		};
		Module g_modules[kMaxModules];
		std::size_t g_moduleCount = 0;   // this thread only

		void RefreshModules()
		{
			static HMODULE handles[kMaxModules];
			static wchar_t path[1024];
			DWORD need = 0;
			const auto self = GetCurrentProcess();
			if (!K32EnumProcessModulesEx(self, handles, sizeof(handles), &need, LIST_MODULES_ALL)) { return; }
			const auto count = (std::min)(static_cast<std::size_t>(need / sizeof(HMODULE)), kMaxModules);
			const auto exe = GetModuleHandleW(nullptr);
			std::size_t used = 0;
			for (std::size_t i = 0; i < count; ++i)
			{
				MODULEINFO info{};
				if (!K32GetModuleInformation(self, handles[i], &info, sizeof(info))) { continue; }
				if (K32GetModuleFileNameExW(self, handles[i], path, 1024) == 0) { continue; }
				const wchar_t* base = path;
				for (const wchar_t* p = path; *p; ++p)
				{
					if (*p == L'\\' || *p == L'/') { base = p + 1; }
				}
				auto& module = g_modules[used++];
				module.base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
				module.end = module.base + info.SizeOfImage;
				module.name[0] = 0;
				WideCharToMultiByte(CP_UTF8, 0, base, -1, module.name, sizeof(module.name), nullptr, nullptr);
				module.name[sizeof(module.name) - 1] = 0;
				_wcslwr_s(path);
				module.ours = handles[i] == exe || wcsstr(path, L"\\skse\\plugins\\") != nullptr || wcsncmp(base, L"skse64", 6) == 0;
			}
			std::sort(g_modules, g_modules + used, [](const Module& a, const Module& b) { return a.base < b.base; });
			g_moduleCount = used;
		}

		const Module* ModuleOf(std::uintptr_t a_address)
		{
			const Module* first = g_modules;
			const Module* end = g_modules + g_moduleCount;
			const Module* at = std::upper_bound(first, end, a_address, [](std::uintptr_t v, const Module& m) { return v < m.base; });
			if (at == first) { return nullptr; }
			--at;
			return a_address < at->end ? at : nullptr;
		}

		// Is there a call instruction ending right at this address?
		bool FollowsCall(std::uintptr_t a_address)
		{
			std::uint8_t b[8];
			if (!SafeRead(reinterpret_cast<const void*>(a_address - 8), b, 8)) { return false; }
			const auto at = [&](int a_back) { return b[8 - a_back]; };
			if (at(5) == 0xE8) { return true; }                                                     // call rel32
			if (at(6) == 0xFF && at(5) == 0x15) { return true; }                                    // call [rip+disp32]
			if (at(6) == 0xFF && (at(5) & 0xF8) == 0x90 && (at(5) & 7) != 4) { return true; }       // call [reg+disp32]
			if (at(7) == 0xFF && at(6) == 0x94) { return true; }                                    // call [sib+disp32]
			if (at(3) == 0xFF && (at(2) & 0xF8) == 0x50 && (at(2) & 7) != 4) { return true; }       // call [reg+disp8]
			if (at(4) == 0xFF && at(3) == 0x54) { return true; }                                    // call [sib+disp8]
			if (at(2) == 0xFF && (at(1) & 0xF8) == 0xD0) { return true; }                           // call reg
			if (at(2) == 0xFF && (at(1) & 0xF8) == 0x10 && (at(1) & 7) != 4 && (at(1) & 7) != 5) { return true; }  // call [reg]
			if (at(3) == 0xFF && at(2) == 0x14) { return true; }                                    // call [sib]
			return false;
		}

		// ---- reading the threads ----

		struct ThreadRecord
		{
			std::uint32_t tid;
			std::uint64_t hash;
			std::uintptr_t start;
			std::uintptr_t rip;
			double cpuMS;
			double kernelMS;
			std::uint16_t frames;
			std::uintptr_t frame[kOtherFrames];
		};
		ThreadRecord g_threads[kMaxThreads];
		std::uintptr_t g_mainFrame[kMainFrames];
		CONTEXT g_context;
		std::uint32_t g_tids[kMaxThreads];
		alignas(16) std::uint8_t g_stack[kMainStackBytes];

		using QueryThread = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
		QueryThread g_queryThread = nullptr;

		// Between SuspendThread and ResumeThread: kernel calls only.
		bool ReadThread(std::uint32_t a_tid, std::size_t a_cap, std::size_t& a_got, ThreadRecord& a_record, double& a_heldUS)
		{
			const auto thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, a_tid);
			if (!thread) { return false; }

			FILETIME created, exited, kernel, user;
			if (GetThreadTimes(thread, &created, &exited, &kernel, &user))
			{
				const auto toMS = [](const FILETIME& a_time) { return static_cast<double>((static_cast<std::uint64_t>(a_time.dwHighDateTime) << 32) | a_time.dwLowDateTime) / 10000.0; };
				a_record.kernelMS = toMS(kernel);
				a_record.cpuMS = toMS(kernel) + toMS(user);
			}
			PVOID start = nullptr;
			if (g_queryThread) { g_queryThread(thread, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), nullptr); }
			a_record.start = reinterpret_cast<std::uintptr_t>(start);

			bool ok = false;
			a_got = 0;
			const auto before = Qpc();
			if (SuspendThread(thread) != static_cast<DWORD>(-1))
			{
				g_context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
				if (GetThreadContext(thread, &g_context))
				{
					ok = true;
					MEMORY_BASIC_INFORMATION region{};
					if (VirtualQuery(reinterpret_cast<const void*>(g_context.Rsp), &region, sizeof(region)) != 0)
					{
						const auto end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
						auto length = end > g_context.Rsp ? static_cast<std::size_t>(end - g_context.Rsp) : 0;
						length = (std::min)(length, a_cap);
						SIZE_T got = 0;
						ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(g_context.Rsp), g_stack, length, &got);
						a_got = got;
					}
				}
				ResumeThread(thread);
			}
			a_heldUS = QpcMS(before, Qpc()) * 1000.0;
			CloseHandle(thread);
			return ok;
		}

		std::size_t ScanStack(std::size_t a_bytes, std::uintptr_t* a_out, std::size_t a_max, bool& a_ours, std::uint64_t& a_hash)
		{
			std::size_t count = 0;
			const auto* words = reinterpret_cast<const std::uintptr_t*>(g_stack);
			for (std::size_t i = 0; i < a_bytes / sizeof(std::uintptr_t) && count < a_max; ++i)
			{
				const auto value = words[i];
				if (value < 0x10000) { continue; }
				const auto* module = ModuleOf(value);
				if (!module || !FollowsCall(value)) { continue; }
				a_out[count++] = value;
				if (module->ours) { a_ours = true; }
				a_hash = (a_hash ^ value) * 0x100000001B3ull;
			}
			return count;
		}

		const char* Where(std::uintptr_t a_address, char* a_out, std::size_t a_size)
		{
			if (const auto* module = ModuleOf(a_address)) { snprintf(a_out, a_size, "%s+%llX", module->name, static_cast<unsigned long long>(a_address - module->base)); }
			else { snprintf(a_out, a_size, "%llX", static_cast<unsigned long long>(a_address)); }
			return a_out;
		}

		// A set of frames as lines of up to six, so a stack is readable and a line is short enough to grep.
		void EmitFrames(const char* a_lead, const std::uintptr_t* a_frames, std::size_t a_count)
		{
			static char line[600];
			std::size_t used = 0;
			line[0] = 0;
			char where[80];
			for (std::size_t i = 0; i < a_count; ++i)
			{
				Where(a_frames[i], where, sizeof(where));
				used += static_cast<std::size_t>(snprintf(line + used, sizeof(line) - used, "%s%s", used ? " < " : "", where));
				if ((i + 1) % 6 == 0 || i + 1 == a_count || used > sizeof(line) - 96)
				{
					Emit("%s%s", a_lead, line);
					used = 0;
					line[0] = 0;
				}
			}
		}

		std::size_t ListThreads()
		{
			// tools\hangtest.cpp proves this passes with the process heap and the loader lock both held by a stuck thread.
			const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snapshot == INVALID_HANDLE_VALUE) { return 0; }
			const auto pid = GetCurrentProcessId();
			THREADENTRY32 entry{};
			entry.dwSize = sizeof(entry);
			std::size_t count = 0;
			if (Thread32First(snapshot, &entry))
			{
				do
				{
					if (entry.th32OwnerProcessID == pid && count < kMaxThreads) { g_tids[count++] = entry.th32ThreadID; }
					entry.dwSize = sizeof(entry);
				} while (Thread32Next(snapshot, &entry));
			}
			CloseHandle(snapshot);
			return count;
		}

		void ThreadLines()
		{
			const auto self = GetCurrentThreadId();
			const auto main = g_mainThread.load(std::memory_order_relaxed);
			double worstHeld = 0.0;
			char where[80];
			char where2[80];

			bool mainRead = false;
			if (main != 0)
			{
				ThreadRecord record{};
				std::size_t got = 0;
				double held = 0.0;
				if (ReadThread(main, kMainStackBytes, got, record, held))
				{
					mainRead = true;
					worstHeld = (std::max)(worstHeld, held);
					bool ours = false;
					std::uint64_t hash = 0;
					const auto frames = ScanStack(got, g_mainFrame, kMainFrames, ours, hash);
					const auto& c = g_context;
					Emit("MAIN thread %u: at %s, started at %s, cpu %.0f ms (%.0f in the kernel), held %.0f us to read %zu bytes of stack", main, Where(c.Rip, where, sizeof(where)), Where(record.start, where2, sizeof(where2)), record.cpuMS, record.kernelMS, held, got);
					Emit("  rax %llX rbx %llX rcx %llX rdx %llX rsi %llX rdi %llX rbp %llX rsp %llX", c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp);
					Emit("  r8 %llX r9 %llX r10 %llX r11 %llX r12 %llX r13 %llX r14 %llX r15 %llX", c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15);
					Emit("  return addresses found on its stack, top first (a scan, so an old value can appear):");
					EmitFrames("    ", g_mainFrame, frames);
				}
				else { Emit("MAIN thread %u could not be read (error %lu)", main, GetLastError()); }
			}
			else { Emit("MAIN thread not known yet: the game has not run a queued task since this plugin asked"); }
			Flush();

			const auto tidCount = ListThreads();
			std::size_t kept = 0;
			for (std::size_t i = 0; i < tidCount; ++i)
			{
				const auto tid = g_tids[i];
				if (tid == self || (mainRead && tid == main)) { continue; }
				auto& record = g_threads[kept];
				record = ThreadRecord{};
				record.tid = tid;
				std::size_t got = 0;
				double held = 0.0;
				if (!ReadThread(tid, kOtherStackBytes, got, record, held)) { continue; }
				worstHeld = (std::max)(worstHeld, held);
				record.rip = g_context.Rip;
				record.hash = 0xCBF29CE484222325ull;
				bool ours = false;
				if (const auto* module = ModuleOf(g_context.Rip); module && module->ours) { ours = true; }
				if (const auto* module = ModuleOf(record.start); module && module->ours) { ours = true; }
				record.frames = static_cast<std::uint16_t>(ScanStack(got, record.frame, kOtherFrames, ours, record.hash));
				if (ours) { ++kept; }
			}

			// The busiest first: a thread spinning on a lock has the most CPU, and the second report shows whether it is still climbing.
			std::sort(g_threads, g_threads + kept, [](const ThreadRecord& a, const ThreadRecord& b) { return a.cpuMS > b.cpuMS; });
			Emit("Other threads in Skyrim or in an SKSE plugin: %zu of %zu in the process, busiest first", kept, tidCount);
			std::size_t full = 0;
			for (std::size_t i = 0; i < kept; ++i)
			{
				const auto& record = g_threads[i];
				std::size_t same = kept;
				for (std::size_t j = 0; j < i; ++j)
				{
					if (g_threads[j].hash == record.hash && g_threads[j].frames == record.frames)
					{
						same = j;
						break;
					}
				}
				if (same != kept) { Emit("thread %u: cpu %.0f ms, at %s, same stack as thread %u", record.tid, record.cpuMS, Where(record.rip, where, sizeof(where)), g_threads[same].tid); }
				else if (full >= kFullStacksShown) { Emit("thread %u: cpu %.0f ms, at %s (stack left out, %zu shown)", record.tid, record.cpuMS, Where(record.rip, where, sizeof(where)), kFullStacksShown); }
				else
				{
					++full;
					Emit("thread %u: cpu %.0f ms (%.0f in the kernel), started at %s, at %s", record.tid, record.cpuMS, record.kernelMS, Where(record.start, where2, sizeof(where2)), Where(record.rip, where, sizeof(where)));
					EmitFrames("    ", record.frame, record.frames);
				}
			}
			if (worstHeld > g_worstSuspend.load(std::memory_order_relaxed)) { g_worstSuspend.store(worstHeld, std::memory_order_relaxed); }
			Emit("Longest any thread was held to be read: %.0f us", worstHeld);
			Flush();
		}

		// ---- reading the ring ----

		const char* NativeName(std::uint32_t a_native)
		{
			const char* name = g_hooks.nativeName ? g_hooks.nativeName(static_cast<std::uint16_t>(a_native)) : nullptr;
			return name ? name : "?";
		}

		void ScriptName(const void* a_script, char* a_out, std::size_t a_size)
		{
			a_out[0] = 0;
			if (a_script && g_hooks.scriptName) { g_hooks.scriptName(a_script, a_out, a_size); }
			if (!a_out[0]) { strncpy_s(a_out, a_size, "?", _TRUNCATE); }
		}

		void InFlightLines()
		{
			std::uint32_t inFlight = 0;
			char script[80];
			const auto calls = g_state.head.load(std::memory_order_relaxed);
			for (auto& slot : g_state.slots)
			{
				const auto owner = slot.owner.load(std::memory_order_relaxed);
				if (owner == 0) { continue; }
				const auto native = slot.native.load(std::memory_order_acquire);
				if (native == 0) { continue; }
				++inFlight;
				ScriptName(slot.script.load(std::memory_order_relaxed), script, sizeof(script));
				Emit("IN FLIGHT: %s on thread %u for %.1f ms, called from script %s, %s", NativeName(native - 1), owner, MillisecondsSince(slot.start.load(std::memory_order_relaxed)), script,
					slot.onTasklet.load(std::memory_order_relaxed) ? "on the tasklet (the fast route)" : "from the VM's function messages (the normal route)");
			}

			double lastBegan = -1.0;
			if (calls != 0)
			{
				const auto& entry = g_state.ring[calls & (kRingSize - 1)];
				if (entry.seq.load(std::memory_order_acquire) != 0) { lastBegan = MillisecondsSince(entry.tsc.load(std::memory_order_relaxed)) / 1000.0; }
			}
			if (inFlight != 0) { Emit("In flight: %u recorded read call(s) running now. %llu calls recorded since start.", inFlight, static_cast<unsigned long long>(calls)); }
			else if (lastBegan >= 0.0) { Emit("In flight: none of the recorded read calls is running. The last one began %.1f s ago. %llu calls recorded since start.", lastBegan, static_cast<unsigned long long>(calls)); }
			else { Emit("In flight: none of the recorded read calls is running, and none has been recorded since start."); }
		}

		// The local time a_ageMS ago, as the log's own clock would have shown it.
		void TimeOfDay(double a_ageMS, char* a_out, std::size_t a_size)
		{
			SYSTEMTIME t;
			GetLocalTime(&t);
			constexpr std::int64_t kDay = 24 * 3600 * 1000;
			auto ms = (t.wHour * 3600000ll + t.wMinute * 60000ll + t.wSecond * 1000ll + t.wMilliseconds - static_cast<std::int64_t>(a_ageMS)) % kDay;
			if (ms < 0) { ms += kDay; }
			snprintf(a_out, a_size, "%02d:%02d:%02d.%03d", static_cast<int>(ms / 3600000), static_cast<int>(ms / 60000 % 60), static_cast<int>(ms / 1000 % 60), static_cast<int>(ms % 1000));
		}

		void RingLines()
		{
			const auto head = g_state.head.load(std::memory_order_relaxed);
			Emit("The last %zu recorded read calls, newest first (when it began, the call, its thread, its route, how long it ran, the calling script):", kRingSize);
			char script[80];
			for (std::size_t back = 0; back < kRingSize && back < head; ++back)
			{
				const auto seq = head - back;
				const auto& entry = g_state.ring[seq & (kRingSize - 1)];
				if (entry.seq.load(std::memory_order_acquire) != seq) { continue; }
				const auto tsc = entry.tsc.load(std::memory_order_relaxed);
				const auto ticks = entry.ticks.load(std::memory_order_relaxed);
				const auto caller = entry.script.load(std::memory_order_relaxed);
				const auto thread = entry.thread.load(std::memory_order_relaxed);
				const auto native = entry.native.load(std::memory_order_relaxed);
				const auto onTasklet = entry.onTasklet.load(std::memory_order_relaxed);
				if (entry.seq.load(std::memory_order_acquire) != seq) { continue; }
				ScriptName(caller, script, sizeof(script));
				const auto route = onTasklet ? "tasklet" : "message";
				const auto age = MillisecondsSince(tsc);
				char when[32];
				TimeOfDay(age, when, sizeof(when));
				if (ticks) { Emit("  %s (-%.3f s)  %s  thread %u  %s  ran %.1f us  %s", when, age / 1000.0, NativeName(native), thread, route, static_cast<double>(ticks) / g_tscPerMS * 1000.0, script); }
				else { Emit("  %s (-%.3f s)  %s  thread %u  %s  NOT RETURNED  %s", when, age / 1000.0, NativeName(native), thread, route, script); }
			}
			Flush();
		}

		const char* WindowInFront()
		{
			DWORD pid = 0;
			if (const auto window = GetForegroundWindow()) { GetWindowThreadProcessId(window, &pid); }
			return pid == GetCurrentProcessId() ? "yes" : "no";
		}

		void Report(std::uint32_t a_stall, int a_which, double a_since, int a_frame, std::uint64_t a_vm, double a_vmStillMS)
		{
			const auto start = Qpc();
			const auto main = g_mainThread.load(std::memory_order_relaxed);
			Emit("==== stall %u, report %d of 2: no frame for %.1f s ====", a_stall, a_which, a_since);
			Emit("Frames: engine counter %d, VM updates %llu (last moved %.1f s ago) | a pausing menu open: %s | game window in front: %s", a_frame, static_cast<unsigned long long>(a_vm), a_vmStillMS / 1000.0,
				g_hooks.paused ? (g_hooks.paused() ? "yes" : "no") : "?", WindowInFront());
			Emit("Main thread %u (%s). By hand: py -3 hang_stack.py %lu %u", main, g_mainHow.load(std::memory_order_relaxed), GetCurrentProcessId(), main);
			Emit("Fast reads: %s", g_hooks.fastReadsStatus ? g_hooks.fastReadsStatus() : "?");
			InFlightLines();
			Flush();
			if (g_hooks.gameLines) { g_hooks.gameLines(); }
			Flush();
			ThreadLines();
			RingLines();
			const auto ms = QpcMS(start, Qpc());
			g_lastReportMS.store(ms, std::memory_order_relaxed);
			Emit("==== end of report %d of 2, stall %u: it took %.1f ms ====", a_which, a_stall, ms);
			Flush();
		}

		// ---- the watcher ----

		DWORD WINAPI Watch(void*)
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

			LARGE_INTEGER frequency;
			QueryPerformanceFrequency(&frequency);
			g_qpcPerMS = static_cast<double>(frequency.QuadPart) / 1000.0;
			{
				const auto q0 = Qpc();
				const auto t0 = __rdtsc();
				Sleep(100);
				const auto q1 = Qpc();
				const auto t1 = __rdtsc();
				g_tscPerMS = static_cast<double>(t1 - t0) / QpcMS(q0, q1);
			}
			g_queryThread = reinterpret_cast<QueryThread>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));

			RefreshModules();
			g_ringCost.store(MeasureCost(200000), std::memory_order_relaxed);
			Emit("Watching: a report when no frame comes for %.0f s, a second %.0f s later if there is still none. One recorded call costs %.0f ns on this machine. %zu modules known.", g_stallSeconds, g_secondGapSeconds, g_ringCost.load(), g_moduleCount);
			Flush();

			int lastFrame = g_hooks.frame ? g_hooks.frame() : -1;
			std::uint64_t lastVM = g_hooks.vmUpdates ? g_hooks.vmUpdates() : 0;
			auto lastAlive = Qpc();
			auto lastVMMoved = lastAlive;
			auto lastRefresh = lastAlive;
			std::uint64_t nextReportAt = 0;
			int reportsThisStall = 0;

			for (;;)
			{
				Sleep(250);
				const auto now = Qpc();
				const int frame = g_hooks.frame ? g_hooks.frame() : -1;
				const std::uint64_t vm = g_hooks.vmUpdates ? g_hooks.vmUpdates() : 0;
				const bool frameMoved = frame != lastFrame;
				const bool vmMoved = vm != lastVM;
				lastFrame = frame;
				lastVM = vm;
				if (vmMoved) { lastVMMoved = now; }

				// The VM's update keeps running behind a pausing menu (7,196 paused frames in a minute, log of 2026-11-05), whether or not
				// the engine's counter does, so a pause with the VM moving is a live game. No counter on this runtime: the VM alone.
				const bool paused = g_hooks.paused && g_hooks.paused();
				if (frameMoved || (vmMoved && (paused || frame < 0)))
				{
					if (g_stalled.exchange(false, std::memory_order_relaxed))
					{
						const auto seconds = QpcMS(lastAlive, now) / 1000.0;
						g_lastStallSeconds.store(seconds, std::memory_order_relaxed);
						Emit("Frames came back after %.1f s (engine counter %d): stall %u is over, so the game was slow or waiting, not frozen for good.", seconds, frame, g_stalls.load(std::memory_order_relaxed));
						Flush();
					}
					g_armed.store(true, std::memory_order_relaxed);
					lastAlive = now;
					reportsThisStall = 0;
					// Only while the game is healthy: enumerating modules is the one step here that is not plainly lock-free.
					if (QpcMS(lastRefresh, now) >= 60000.0)
					{
						RefreshModules();
						lastRefresh = now;
					}
					continue;
				}
				if (!g_armed.load(std::memory_order_relaxed)) { continue; }

				const auto sinceMS = QpcMS(lastAlive, now);
				if (reportsThisStall == 0 && sinceMS >= g_stallSeconds * 1000.0)
				{
					g_stalled.store(true, std::memory_order_relaxed);
					const auto stall = g_stalls.fetch_add(1, std::memory_order_relaxed) + 1;
					reportsThisStall = 1;
					nextReportAt = now + static_cast<std::uint64_t>(g_secondGapSeconds * 1000.0 * g_qpcPerMS);
					g_lastStallSeconds.store(sinceMS / 1000.0, std::memory_order_relaxed);
					if (g_reports.load(std::memory_order_relaxed) < kMaxFullReports)
					{
						g_reports.fetch_add(1, std::memory_order_relaxed);
						Report(stall, 1, sinceMS / 1000.0, frame, vm, QpcMS(lastVMMoved, now));
					}
					else
					{
						Emit("Stall %u: no frame for %.1f s. %u full reports are already written this session, so no more.", stall, sinceMS / 1000.0, kMaxFullReports);
						Flush();
					}
				}
				else if (reportsThisStall == 1 && now >= nextReportAt)
				{
					reportsThisStall = 2;
					g_lastStallSeconds.store(sinceMS / 1000.0, std::memory_order_relaxed);
					if (g_reports.load(std::memory_order_relaxed) < kMaxFullReports)
					{
						g_reports.fetch_add(1, std::memory_order_relaxed);
						Report(g_stalls.load(std::memory_order_relaxed), 2, sinceMS / 1000.0, frame, vm, QpcMS(lastVMMoved, now));
					}
				}
			}
		}
	}

	State& GlobalState() { return g_state; }

	void Emit(const char* a_format, ...)
	{
		if (sizeof(g_out) - g_outUsed < 2048) { Flush(); }
		SYSTEMTIME t;
		GetLocalTime(&t);
		auto used = g_outUsed;
		used += static_cast<std::size_t>(snprintf(g_out + used, sizeof(g_out) - used, "[%02u:%02u:%02u] [warning] HANG ", t.wHour, t.wMinute, t.wSecond));
		va_list args;
		va_start(args, a_format);
		const int length = vsnprintf(g_out + used, sizeof(g_out) - used - 3, a_format, args);
		va_end(args);
		if (length > 0) { used += (std::min)(static_cast<std::size_t>(length), sizeof(g_out) - used - 3); }
		g_out[used++] = '\r';
		g_out[used++] = '\n';
		g_outUsed = used;
	}

	bool SafeRead(const void* a_from, void* a_to, std::size_t a_size)
	{
		SIZE_T got = 0;
		return ReadProcessMemory(GetCurrentProcess(), a_from, a_to, a_size, &got) && got == a_size;
	}

	double MillisecondsSince(std::uint64_t a_tsc) { return static_cast<double>(__rdtsc() - a_tsc) / g_tscPerMS; }

	void SetMainThread(std::uint32_t a_thread, const char* a_how)
	{
		g_mainHow.store(a_how, std::memory_order_relaxed);
		g_mainThread.store(a_thread, std::memory_order_relaxed);
	}

	std::uint32_t MainThread() { return g_mainThread.load(std::memory_order_relaxed); }

	bool StartWatcher(const Hooks& a_hooks, double a_stallSeconds, double a_secondGapSeconds, const wchar_t* a_logPath)
	{
		g_hooks = a_hooks;
		g_stallSeconds = a_stallSeconds;
		g_secondGapSeconds = a_secondGapSeconds;
		// FILE_APPEND_DATA with every share mode: spdlog holds the same file open for appending, and each write lands whole at the end.
		g_log = CreateFileW(a_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (g_log == INVALID_HANDLE_VALUE) { return false; }
		const auto thread = CreateThread(nullptr, 256 * 1024, &Watch, nullptr, 0, nullptr);
		if (!thread) { return false; }
		CloseHandle(thread);
		return true;
	}

	double MeasureCost(int a_calls)
	{
		static State local;
		LARGE_INTEGER frequency, t0, t1, t2;
		QueryPerformanceFrequency(&frequency);
		volatile std::uint64_t sink = 0;
		const int value = 0x1234;
		QueryPerformanceCounter(&t0);
		for (int i = 0; i < a_calls; ++i) { sink = sink + static_cast<std::uint64_t>(i); }
		QueryPerformanceCounter(&t1);
		for (int i = 0; i < a_calls; ++i)
		{
			CallScope call;
			Enter(local, call, static_cast<std::uint16_t>(i & 15), &value, (i & 1) != 0);
			sink = sink + static_cast<std::uint64_t>(i);
			Leave(call);
		}
		QueryPerformanceCounter(&t2);
		const auto ns = [&](const LARGE_INTEGER& a, const LARGE_INTEGER& b) { return static_cast<double>(b.QuadPart - a.QuadPart) * 1e9 / static_cast<double>(frequency.QuadPart); };
		return (ns(t1, t2) - ns(t0, t1)) / static_cast<double>(a_calls);
	}

	double MeasureCostThreads(int a_threads, int a_callsEach)
	{
		static State shared;
		struct Job
		{
			HANDLE go;
			int calls;
			double ns;
		};
		static Job jobs[64];
		a_threads = (std::min)(a_threads, 64);
		const auto go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		HANDLE threads[64];
		for (int t = 0; t < a_threads; ++t)
		{
			jobs[t] = Job{ go, a_callsEach, 0.0 };
			threads[t] = CreateThread(
				nullptr, 0, [](void* a_job) -> DWORD {
					auto& job = *static_cast<Job*>(a_job);
					WaitForSingleObject(job.go, INFINITE);
					LARGE_INTEGER frequency, t0, t1;
					QueryPerformanceFrequency(&frequency);
					const int value = 0x1234;
					QueryPerformanceCounter(&t0);
					for (int i = 0; i < job.calls; ++i)
					{
						CallScope call;
						Enter(shared, call, static_cast<std::uint16_t>(i & 15), &value, true);
						Leave(call);
					}
					QueryPerformanceCounter(&t1);
					job.ns = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e9 / static_cast<double>(frequency.QuadPart) / static_cast<double>(job.calls);
					return 0;
				},
				&jobs[t], 0, nullptr);
		}
		SetEvent(go);
		WaitForMultipleObjects(static_cast<DWORD>(a_threads), threads, TRUE, INFINITE);
		double total = 0.0;
		for (int t = 0; t < a_threads; ++t)
		{
			total += jobs[t].ns;
			CloseHandle(threads[t]);
		}
		CloseHandle(go);
		return total / a_threads;
	}

	Stats GetStats()
	{
		return Stats{ g_stalls.load(std::memory_order_relaxed), g_reports.load(std::memory_order_relaxed), g_lastStallSeconds.load(std::memory_order_relaxed), g_ringCost.load(std::memory_order_relaxed),
			g_worstSuspend.load(std::memory_order_relaxed), g_lastReportMS.load(std::memory_order_relaxed), g_armed.load(std::memory_order_relaxed), g_stalled.load(std::memory_order_relaxed) };
	}

	std::uint32_t InFlightNow()
	{
		std::uint32_t count = 0;
		for (auto& slot : g_state.slots)
		{
			if (slot.native.load(std::memory_order_relaxed) != 0) { ++count; }
		}
		return count;
	}
}
