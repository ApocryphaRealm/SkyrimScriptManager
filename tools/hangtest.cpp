// Runs the hang recorder's core with no game, on the conditions a real freeze can bring. Own code, GPL-3.0-or-later (2026-11-05).
//
// A "main" thread sits in a wait forever. One thread holds the process heap and another the loader lock, the two things most likely to
// trap a recorder that was careless. A third is stopped inside a recorded call, as a read stuck on a lock would be, and a fourth spins.
// If the recorder touched the heap or the loader, its reports would never appear. It also measures the cost of one recorded call.
//
//   tools\build-hangtest.bat, then build\hangtest\hangtest.exe <log file>
#include "HangCore.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include <Windows.h>

namespace
{
	const char* const kNames[] = { "Form.GetFormID", "Form.HasKeyword", "ObjectReference.GetBaseObject", "ActiveMagicEffect.GetBaseObject" };

	struct FakeType
	{
		const char* name;
	};
	const FakeType kScripts[] = { { "SomeQuestScript" }, { "SomeEffectScript" } };

	std::atomic<int> g_frame{ 100 };
	std::atomic<bool> g_stop{ false };
	HANDLE g_never = nullptr;

	__declspec(noinline) DWORD WINAPI FakeMain(void*)
	{
		HangCore::SetMainThread(GetCurrentThreadId(), "the test's main thread");
		while (!g_stop.load())
		{
			g_frame.fetch_add(1);
			Sleep(16);
		}
		WaitForSingleObject(g_never, INFINITE);
		return 0;
	}

	__declspec(noinline) DWORD WINAPI HoldsHeap(void*)
	{
		while (!g_stop.load()) { Sleep(1); }
		HeapLock(GetProcessHeap());
		WaitForSingleObject(g_never, INFINITE);
		return 0;
	}

	using LockLoader = LONG(NTAPI*)(ULONG, PULONG, PVOID*);

	__declspec(noinline) DWORD WINAPI HoldsLoader(void*)
	{
		const auto lock = reinterpret_cast<LockLoader>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrLockLoaderLock"));
		while (!g_stop.load()) { Sleep(1); }
		ULONG state = 0;
		PVOID cookie = nullptr;
		lock(0, &state, &cookie);
		WaitForSingleObject(g_never, INFINITE);
		return 0;
	}

	__declspec(noinline) DWORD WINAPI StuckInACall(void*)
	{
		HangCore::CallScope call;
		HangCore::Enter(HangCore::GlobalState(), call, 2, &kScripts[1], true);
		WaitForSingleObject(g_never, INFINITE);
		HangCore::Leave(call);
		return 0;
	}

	__declspec(noinline) DWORD WINAPI Spins(void*)
	{
		volatile std::uint64_t n = 0;
		for (;;) { n = n + 1; }
	}

	void Lines()
	{
		HangCore::Emit("Load: (the game's lines go here)");
	}
}

int main(int a_argc, char** a_argv)
{
	wchar_t path[512] = L"hangtest.log";
	if (a_argc > 1) { MultiByteToWideChar(CP_UTF8, 0, a_argv[1], -1, path, 512); }

	printf("Cost of one recorded call, ns:\n");
	for (int i = 0; i < 3; ++i) { printf("  one thread, private ring: %.1f\n", HangCore::MeasureCost(5000000)); }
	for (const int threads : { 1, 2, 4, 8 }) { printf("  %d thread(s) on one ring: %.1f\n", threads, HangCore::MeasureCostThreads(threads, 2000000)); }
	fflush(stdout);

	g_never = CreateEventW(nullptr, TRUE, FALSE, nullptr);

	HangCore::Hooks hooks;
	hooks.frame = [] { return g_frame.load(); };
	hooks.nativeName = [](std::uint16_t a_native) -> const char* { return a_native < 4 ? kNames[a_native] : nullptr; };
	hooks.scriptName = [](const void* a_script, char* a_out, std::size_t a_size) {
		const char* name = nullptr;
		if (HangCore::SafeRead(a_script, &name, sizeof(name)) && name) { strncpy_s(a_out, a_size, name, _TRUNCATE); }
	};
	hooks.gameLines = &Lines;
	hooks.fastReadsStatus = [] { return "ON for 17 read calls (the test says so)"; };

	CreateThread(nullptr, 0, &FakeMain, nullptr, 0, nullptr);
	Sleep(50);
	HangCore::StartWatcher(hooks, 2.0, 2.0, path);

	// Some history for the ring, then a call that never returns.
	for (int i = 0; i < 80; ++i)
	{
		HangCore::CallScope call;
		HangCore::Enter(HangCore::GlobalState(), call, static_cast<std::uint16_t>(i & 1), &kScripts[0], (i & 3) != 0);
		Sleep(1);
		HangCore::Leave(call);
	}
	CreateThread(nullptr, 0, &HoldsHeap, nullptr, 0, nullptr);
	CreateThread(nullptr, 0, &HoldsLoader, nullptr, 0, nullptr);
	CreateThread(nullptr, 0, &StuckInACall, nullptr, 0, nullptr);
	CreateThread(nullptr, 0, &Spins, nullptr, 0, nullptr);
	Sleep(1000);
	g_stop.store(true);

	// From here this thread touches neither heap nor loader either, so it can judge the result.
	const auto started = GetTickCount64();
	while (GetTickCount64() - started < 15000)
	{
		const auto stats = HangCore::GetStats();
		if (stats.reports >= 2 && !stats.stalled) { break; }
		if (stats.reports >= 2 && stats.lastReportMS > 0.0)
		{
			Sleep(300);
			const auto done = HangCore::GetStats();
			char text[256];
			const int length = snprintf(text, sizeof(text), "PASS: two reports written with the heap and the loader lock held; the last took %.1f ms, longest thread hold %.0f us, in flight %u\n", done.lastReportMS, done.worstSuspendMicroseconds, HangCore::InFlightNow());
			DWORD written = 0;
			WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, static_cast<DWORD>(length), &written, nullptr);
			TerminateProcess(GetCurrentProcess(), 0);  // not ExitProcess: that waits for the loader lock this test holds
		}
		Sleep(100);
	}
	const char fail[] = "FAIL: the reports did not finish: the recorder is stuck on something a stuck thread holds\n";
	DWORD written = 0;
	WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), fail, sizeof(fail) - 1, &written, nullptr);
	TerminateProcess(GetCurrentProcess(), 1);
	return 1;
}
