// Skyrim Script Manager. Own code, GPL-3.0-or-later (2026-11-04).
//
// A plain SKSE plugin that listens to the Papyrus VM and writes down what it sees. As shipped it holds, drops and changes nothing;
// the two things that can change the VM are off unless set: FastReads (bFastReads=1) and the loop guard (bLoopGuard=1).
#include "PCH.h"

#include "DevBench/DevBenchAPI.h"
#include "FastReads.h"
#include "LoopGuard.h"
#include "Monitor.h"
#include "Roles.h"
#include "utils/AddressLibraryGuard.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <Windows.h>

namespace
{
	inline constexpr const char* kDisplayName = "Skyrim Script Manager";
	inline constexpr const char* kLogName = "SkyrimScriptManager";
	inline constexpr const char* kIniPath = "Data/SKSE/Plugins/SkyrimScriptManager.ini";

	bool g_fastReads = false;

	// The value of a key in the flat arguments object DevBench passes: a string without its quotes, or a number as text.
	std::string ArgOf(const std::string& a_args, const char* a_key)
	{
		const auto key = std::string("\"") + a_key + "\"";
		auto at = a_args.find(key);
		if (at == std::string::npos) { return {}; }
		at = a_args.find(':', at + key.size());
		if (at == std::string::npos) { return {}; }
		at = a_args.find_first_not_of(" \t", at + 1);
		if (at == std::string::npos) { return {}; }
		if (a_args[at] == '"')
		{
			const auto end = a_args.find('"', at + 1);
			return end == std::string::npos ? std::string{} : a_args.substr(at + 1, end - at - 1);
		}
		const auto end = a_args.find_first_of(",} \t", at);
		return a_args.substr(at, end == std::string::npos ? std::string::npos : end - at);
	}

	void DevBenchHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		const std::string args = a_argsJson ? a_argsJson : "";
		const auto op = ArgOf(args, "op");
		if (op == "report") { Monitor::RequestReport(); }
		else if (op == "flood")
		{
			const auto rate = ArgOf(args, "rate");
			const auto seconds = ArgOf(args, "seconds");
			LoopGuard::StartFlood(ArgOf(args, "target"), ArgOf(args, "event"), rate.empty() ? 40.0F : static_cast<float>(std::atof(rate.c_str())), seconds.empty() ? 20.0F : static_cast<float>(std::atof(seconds.c_str())));
		}
		else if (op == "floodstop") { LoopGuard::StopFlood(); }
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
			"{\"description\":\"Skyrim Script Manager. Default: whether the VM layout checks passed, whether the stack walk is on, overstress, the stack counts, the budget the VM was given, the classifier's counts and the text of the last report. "
			"loop holds the loop watch and guard: detections, what they did, the last lines. op=report writes a report at the next VM update. "
			"op=flood repeats an event on a test actor from the main thread so a detection can be proved, for test saves only: target = hex form id (default: the nearest actor with a scripted effect), "
			"event = effect (default: real OnEffectFinish and OnEffectStart in turn to each of its scripted effects) or any other name (sent to the actor with no arguments, so a name no script has runs nothing), "
			"rate = per second (default 40, up to 500), seconds (default 20, up to 300). op=floodstop ends it.\","
			"\"inputSchema\":{\"type\":\"object\",\"properties\":{\"op\":{\"type\":\"string\"},\"target\":{\"type\":\"string\"},\"event\":{\"type\":\"string\"},\"rate\":{\"type\":\"number\"},\"seconds\":{\"type\":\"number\"}}},"
			"\"readOnly\":false}";
		registered = devBench->RegisterTool("scriptmanager.state", descriptor, &DevBenchHandler, nullptr);
		logger::debug("DevBench tool scriptmanager.state: {}", registered ? "registered" : "refused");
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
			LoopGuard::OnDataLoaded();
			Roles::OnDataLoaded();
			FastReads::Apply(g_fastReads);
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			Monitor::OnGameLoaded();
			LoopGuard::OnGameLoaded();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			LoopGuard::OnPreLoad();
			break;
		case SKSE::MessagingInterface::kSaveGame:
			LoopGuard::OnSave();
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

		LoopGuard::Settings loop;
		loop.watch = ReadInt(L"LoopWatch", L"bLoopWatch", 1, 0, 1) != 0;
		loop.oneShotRate = ReadFloat(L"LoopWatch", L"fOneShotRate", L"15", 1.0F, 10000.0F);
		loop.attachRate = ReadFloat(L"LoopWatch", L"fAttachRate", L"5", 1.0F, 10000.0F);
		loop.otherRate = ReadFloat(L"LoopWatch", L"fOtherRate", L"100", 1.0F, 100000.0F);
		loop.sustainSeconds = ReadInt(L"LoopWatch", L"iSustainSeconds", 3, 1, 60);
		loop.otherSustainSeconds = ReadInt(L"LoopWatch", L"iOtherSustainSeconds", 5, 1, 60);
		loop.releaseSeconds = ReadInt(L"LoopWatch", L"iReleaseSeconds", 3, 1, 60);
		loop.stacksOnOneObject = ReadInt(L"LoopWatch", L"iStacksOnOneObject", 200, 10, 100000);
		loop.deepFrames = ReadInt(L"LoopWatch", L"iDeepFrames", 50, 8, 100000);
		loop.sameFunctionFrames = ReadInt(L"LoopWatch", L"iSameFunctionFrames", 8, 2, 64);
		loop.guard = loop.watch && ReadInt(L"Manage", L"bLoopGuard", 0, 0, 1) != 0;
		LoopGuard::Configure(loop);
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
	SKSE::log::describe_level("SkyrimScriptManager.ini");
	LoadSettings();
	logger::info("{} {} loading.", kDisplayName, SKSE::PluginDeclaration::GetSingleton()->GetVersion().string("."));
	Monitor::InstallHooks();
	SKSE::GetMessagingInterface()->RegisterListener(MessageHandler);
	return true;
}
