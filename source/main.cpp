// Script Load Monitor. Own code, GPL-3.0-or-later (2026-11-04).
//
// A plain SKSE plugin that listens to the Papyrus VM and writes down what it sees. As shipped it holds, drops and changes nothing;
// the one thing that can change the VM, FastReads, is off unless bFastReads=1.
#include "PCH.h"

#include "DevBench/DevBenchAPI.h"
#include "FastReads.h"
#include "Monitor.h"
#include "Roles.h"
#include "utils/AddressLibraryGuard.h"
#include "utils/Logger.h"

#include <algorithm>
#include <filesystem>
#include <string>

#include <Windows.h>

namespace
{
	inline constexpr const char* kDisplayName = "Script Load Monitor";
	inline constexpr const char* kLogName = "ScriptLoadMonitor";
	inline constexpr const char* kIniPath = "Data/SKSE/Plugins/ScriptLoadMonitor.ini";

	bool g_fastReads = false;

	void DevBenchHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		const std::string args = a_argsJson ? a_argsJson : "";
		if (args.find("\"report\"") != std::string::npos) { Monitor::RequestReport(); }
		a_write(a_sink, Monitor::StateJson().c_str());
	}

	void RegisterDevBench(bool a_lastAttempt)
	{
		static bool registered = false;
		if (registered) { return; }
		auto* devBench = DevBenchAPI::GetDevBenchInterface001();
		if (!devBench)
		{
			if (a_lastAttempt) { logger::debug("DevBench not detected; no state tool"); }
			return;
		}
		constexpr const char* descriptor =
			"{\"description\":\"Script Load Monitor. Default: whether the VM layout checks passed, whether the stack walk is on, overstress, the stack counts, the budget the VM was given, the classifier's counts and the text of the last report. "
			"op=report writes a report at the next VM update. Nothing here changes the game.\","
			"\"inputSchema\":{\"type\":\"object\",\"properties\":{\"op\":{\"type\":\"string\"}}},"
			"\"readOnly\":true}";
		registered = devBench->RegisterTool("scriptmonitor.state", descriptor, &DevBenchHandler, nullptr);
		logger::debug("DevBench tool scriptmonitor.state: {}", registered ? "registered" : "refused");
	}

	void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type)
		{
		case SKSE::MessagingInterface::kPostLoad:
			RegisterDevBench(false);
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			RegisterDevBench(true);
			Monitor::OnDataLoaded();
			Roles::OnDataLoaded();
			FastReads::Apply(g_fastReads);
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			Monitor::OnGameLoaded();
			break;
		default:
			break;
		}
	}

	float ReadFloat(const wchar_t* a_section, const wchar_t* a_key, const wchar_t* a_default, float a_min, float a_max)
	{
		const auto ini = std::filesystem::absolute(kIniPath);
		wchar_t text[32]{};
		GetPrivateProfileStringW(a_section, a_key, a_default, text, 32, ini.c_str());
		return std::clamp(static_cast<float>(_wtof(text)), a_min, a_max);
	}

	int ReadInt(const wchar_t* a_section, const wchar_t* a_key, int a_default, int a_min, int a_max)
	{
		const auto ini = std::filesystem::absolute(kIniPath);
		return std::clamp(static_cast<int>(GetPrivateProfileIntW(a_section, a_key, a_default, ini.c_str())), a_min, a_max);
	}

	void LoadSettings()
	{
		Monitor::Settings settings;
		settings.intervalSeconds = ReadFloat(L"Report", L"fIntervalSeconds", L"60", 5.0F, 3600.0F);
		settings.overstressGapSeconds = ReadFloat(L"Report", L"fOverstressGapSeconds", L"10", 1.0F, 600.0F);
		settings.overstressSampleSeconds = ReadFloat(L"Report", L"fOverstressSampleSeconds", L"2", 0.5F, 60.0F);
		settings.topRows = ReadInt(L"Report", L"iTopRows", 10, 1, 50);
		settings.maxStacksPerWalk = ReadInt(L"Walk", L"iMaxStacksPerWalk", 4000, 16, 100000);
		settings.maxWalkMS = ReadFloat(L"Walk", L"fMaxWalkMS", L"4", 0.5F, 50.0F);
		Monitor::Configure(settings);
		Roles::Configure(ReadInt(L"Roles", L"bClassifyScripts", 1, 0, 1) != 0);
		g_fastReads = ReadInt(L"Manage", L"bFastReads", 0, 0, 1) != 0;
		logger::info("Settings: report every {:.0f} s, on overstress with {:.0f} s between, sample every {:.1f} s during it, {} rows; a walk stops at {} stacks or {:.1f} ms",
			settings.intervalSeconds, settings.overstressGapSeconds, settings.overstressSampleSeconds, settings.topRows, settings.maxStacksPerWalk, settings.maxWalkMS);
	}

	spdlog::level::level_enum ConfiguredLogLevel()
	{
		const auto ini = std::filesystem::absolute(kIniPath);
		const UINT level = GetPrivateProfileIntW(L"Debug", L"uLogLevel", 2, ini.c_str());
		return static_cast<spdlog::level::level_enum>(level <= 6 ? level : 2);
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::log::init(kLogName);
	if (!AddressLibraryGuard::Guard(kDisplayName))
	{
		return true;
	}
	SKSE::Init(a_skse);
	const auto level = ConfiguredLogLevel();
	SKSE::log::set_level(level, level);
	spdlog::set_pattern("[%H:%M:%S] [%l] %v");
	SKSE::log::describe_level("ScriptLoadMonitor.ini");
	LoadSettings();
	logger::info("{} {} loading. It holds and drops nothing.", kDisplayName, SKSE::PluginDeclaration::GetSingleton()->GetVersion().string("."));
	Monitor::InstallHooks();
	SKSE::GetMessagingInterface()->RegisterListener(MessageHandler);
	return true;
}
