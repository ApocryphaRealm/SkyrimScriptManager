// The role table: reads the scripts the game has actually loaded, judges them, writes the result down, and takes the listener's
// reports of anything the judgement did not account for. Own code, GPL-3.0-or-later (2026-11-04).
//
// Nothing here holds or drops a script. It runs on its own low-priority thread and is rebuilt from the player's own files each
// game, so there is no table to ship and none to go stale when the list changes.
#include "PCH.h"

#include "RoleClassifier.h"
#include "Roles.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Windows.h>
#undef GetObject

namespace Roles
{
	namespace
	{
		using RoleClassifier::Lower;
		using RoleClassifier::Role;
		using RoleClassifier::RoleName;

		inline constexpr const char* kOverridesPath = "Data/SKSE/Plugins/SkyrimScriptManager_Overrides.ini";

		struct Entry
		{
			Role role = Role::kNeverTouch;
			std::string reason;
			std::string parents;
			std::set<std::string> accounted;
			std::map<std::string, RoleClassifier::EventRole> events;
			bool flipped = false;      // the listener saw something its code was not found to do
			bool overridden = false;   // the player's file said so
		};

		bool g_enabled = true;

		std::mutex g_lock;                                 // guards everything below
		std::condition_variable g_wake;
		std::vector<std::string> g_queue;
		std::unordered_set<std::string> g_offered;
		std::unordered_map<std::string, Entry> g_table;
		std::map<std::string, std::string> g_flips;        // script -> what was seen, kept between games
		std::map<std::string, Role> g_overrides;
		bool g_threadStarted = false;
		bool g_dirty = false;
		int g_readFailures = 0;
		bool g_readerFaulted = false;
		std::uint64_t g_edgesChecked = 0;
		std::uint32_t g_errors = 0;

		std::filesystem::path LogFile(const char* a_name)
		{
			auto dir = SKSE::log::log_directory();
			return dir ? *dir / a_name : std::filesystem::path(a_name);
		}

		// ---- reading one script ----

		bool ReadLoose(const std::string& a_name, std::vector<unsigned char>& a_out)
		{
			std::ifstream file(std::filesystem::path("Data/Scripts") / (a_name + ".pex"), std::ios::binary);
			if (!file) { return false; }
			a_out.assign(std::istreambuf_iterator<char>(file), {});
			return !a_out.empty();
		}

		void ReadThroughGame(const std::string& a_name, std::vector<unsigned char>& a_out)
		{
			RE::BSResourceNiBinaryStream stream(std::string("Scripts\\") + a_name + ".pex");
			if (!stream.good()) { return; }
			if (!stream.stream) { return; }
			const auto size = stream.stream->totalSize;
			if (size == 0 || size > 64 * 1024 * 1024) { return; }
			a_out.resize(size);
			if (!stream.read(a_out.data(), size)) { a_out.clear(); }
		}

		bool GuardedReadThroughGame(const std::string& a_name, std::vector<unsigned char>& a_out)
		{
			__try
			{
				ReadThroughGame(a_name, a_out);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// ---- the output file ----

		const char* FileRoleName(Role a_role) { return RoleName(a_role); }

		void WriteTable()
		{
			std::vector<std::pair<std::string, Entry>> rows;
			std::map<std::string, std::string> flips;
			{
				std::scoped_lock l(g_lock);
				rows.reserve(g_table.size());
				for (const auto& [name, entry] : g_table) { rows.emplace_back(name, entry); }
				flips = g_flips;
				g_dirty = false;
			}
			std::ranges::sort(rows, [](const auto& a, const auto& b) {
				if (a.second.role != b.second.role) { return a.second.role < b.second.role; }
				return a.first < b.first;
			});

			int counts[3]{};
			int eventCounts[3]{};
			for (const auto& [name, entry] : rows)
			{
				counts[static_cast<int>(entry.role)]++;
				for (const auto& [eventName, event] : entry.events) { eventCounts[static_cast<int>(event.role)]++; }
			}

			std::ofstream out(LogFile("SkyrimScriptManager_Roles.txt"), std::ios::trunc);
			if (!out)
			{
				logger::warn("Could not write SkyrimScriptManager_Roles.txt");
				return;
			}
			out << "Skyrim Script Manager: what each loaded script could tolerate when frames are low. These roles hold and drop nothing by themselves.\n";
			out << "may drop: only reads and one-shot looks or sounds, with nothing left waiting for a second half.\n";
			out << "may wait: the same, but it keeps its own state, waits, or starts something a later event ends. Late is fine, never is not.\n";
			out << "never touch: anything else, and anything unsure. The reason is the first call that decided it.\n";
			out << "An event's role is what running that event can reach; a script's role is everything in it.\n";
			out << "Written from what the scripts' code says. A clean log only means nothing bad was seen in that session.\n\n";
			out << std::format("Scripts: {} may drop, {} may wait, {} never touch\n", counts[0], counts[1], counts[2]);
			out << std::format("Events:  {} may drop, {} may wait, {} never touch\n\n", eventCounts[0], eventCounts[1], eventCounts[2]);
			if (!flips.empty())
			{
				out << "Turned to never touch because the listener saw a call the code was not found to make:\n";
				for (const auto& [script, what] : flips) { out << "  " << script << ": " << what << '\n'; }
				out << '\n';
			}
			for (const auto& [name, entry] : rows)
			{
				out << std::format("{:<12} {}  [{}]  {}{}\n", FileRoleName(entry.role), name, entry.parents, entry.reason, entry.overridden ? " (your override)" : "");
				const bool anyLighter = std::ranges::any_of(entry.events, [](const auto& a) { return a.second.role != Role::kNeverTouch; });
				if (anyLighter)
				{
					for (const auto& [eventName, event] : entry.events)
					{
						out << std::format("    {:<12} {}: {}\n", RoleName(event.role), eventName, event.reason);
					}
				}
			}
			logger::info("Roles written to {}: scripts {} may drop, {} may wait, {} never touch; events {} may drop, {} may wait, {} never touch",
				LogFile("SkyrimScriptManager_Roles.txt").filename().string(), counts[0], counts[1], counts[2], eventCounts[0], eventCounts[1], eventCounts[2]);
		}

		// ---- the worker ----

		void Worker()
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
			std::unordered_map<std::string, RoleClassifier::Pex> scripts;
			std::vector<unsigned char> bytes;

			const auto load = [&](const std::string& a_name) -> const RoleClassifier::Pex* {
				if (const auto found = scripts.find(a_name); found != scripts.end()) { return &found->second; }
				bytes.clear();
				bool have = ReadLoose(a_name, bytes);
				if (!have)
				{
					if (!GuardedReadThroughGame(a_name, bytes))
					{
						std::scoped_lock l(g_lock);
						g_readerFaulted = true;
						return nullptr;
					}
					have = !bytes.empty();
				}
				if (!have) { return nullptr; }
				RoleClassifier::Pex pex;
				std::string error;
				if (!RoleClassifier::Parse(bytes.data(), bytes.size(), pex, error))
				{
					logger::warn("Could not read script {}: {}", a_name, error);
					std::scoped_lock l(g_lock);
					g_readFailures++;
					return nullptr;
				}
				return &scripts.emplace(a_name, std::move(pex)).first->second;
			};

			for (;;)
			{
				std::vector<std::string> batch;
				{
					std::unique_lock l(g_lock);
					g_wake.wait(l, [] { return !g_queue.empty(); });
					batch.swap(g_queue);
					if (g_readerFaulted)
					{
						logger::error("Reading scripts through the game raised an exception; the classifier stopped. Scripts not read stay never touch.");
						return;
					}
				}

				const auto start = std::chrono::steady_clock::now();
				for (const auto& name : batch)
				{
					auto result = RoleClassifier::Classify(name, load);
					Entry entry;
					entry.role = result.role;
					entry.reason = std::move(result.reason);
					entry.parents = std::move(result.parents);
					entry.accounted = std::move(result.accounted);
					for (auto& event : result.events)
					{
						auto key = event.event;
						entry.events.emplace(std::move(key), std::move(event));
					}

					std::scoped_lock l(g_lock);
					if (const auto flip = g_flips.find(name); flip != g_flips.end())
					{
						entry.role = Role::kNeverTouch;
						entry.reason = "the listener saw it: " + flip->second;
						entry.flipped = true;
						for (auto& [eventName, event] : entry.events)
						{
							event.role = Role::kNeverTouch;
							event.reason = entry.reason;
						}
					}
					if (const auto over = g_overrides.find(name); over != g_overrides.end())
					{
						entry.role = over->second;
						entry.overridden = true;
						for (auto& [eventName, event] : entry.events)
						{
							event.role = over->second;
							event.reason = "your override";
						}
					}
					g_table[name] = std::move(entry);
					g_dirty = true;
				}
				scripts.clear();

				const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
				logger::info("Read {} scripts in {:.1f} s on the classifier's thread", batch.size(), seconds);
				WriteTable();
			}
		}

		void LoadOverrides()
		{
			std::ifstream file(kOverridesPath);
			if (!file) { return; }
			std::map<std::string, Role> overrides;
			for (std::string line; std::getline(file, line);)
			{
				const auto trim = [](std::string a_text) {
					const auto first = a_text.find_first_not_of(" \t\r\n");
					if (first == std::string::npos) { return std::string{}; }
					return a_text.substr(first, a_text.find_last_not_of(" \t\r\n") - first + 1);
				};
				line = trim(line);
				if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') { continue; }
				const auto equals = line.find('=');
				if (equals == std::string::npos) { continue; }
				const auto name = Lower(trim(line.substr(0, equals)));
				const auto value = Lower(trim(line.substr(equals + 1)));
				if (value == "may_drop") { overrides[name] = Role::kMayDrop; }
				else if (value == "may_wait") { overrides[name] = Role::kMayWait; }
				else if (value == "never_touch") { overrides[name] = Role::kNeverTouch; }
				else { logger::warn("Overrides: {} has the value {}, which is not may_drop, may_wait or never_touch; ignored", name, value); }
			}
			if (!overrides.empty()) { logger::info("Overrides: {} scripts", overrides.size()); }
			std::scoped_lock l(g_lock);
			g_overrides = std::move(overrides);
		}

		void LoadFlips()
		{
			std::ifstream file(LogFile("SkyrimScriptManager_Flips.txt"));
			if (!file) { return; }
			std::map<std::string, std::string> flips;
			for (std::string line; std::getline(file, line);)
			{
				const auto tab = line.find('\t');
				if (tab == std::string::npos) { continue; }
				flips[line.substr(0, tab)] = line.substr(tab + 1);
			}
			if (!flips.empty()) { logger::info("{} scripts stay never touch from earlier sessions (SkyrimScriptManager_Flips.txt; delete a line to judge one again)", flips.size()); }
			std::scoped_lock l(g_lock);
			g_flips = std::move(flips);
		}
	}

	void Configure(bool a_enabled) { g_enabled = a_enabled; }

	void OnDataLoaded()
	{
		if (!g_enabled)
		{
			logger::info("Classifier off by bClassifyScripts");
			return;
		}
		LoadOverrides();
		LoadFlips();
	}

	void OfferLoadedScripts(std::vector<std::string> a_names)
	{
		if (!g_enabled) { return; }
		std::scoped_lock l(g_lock);
		for (auto& name : a_names)
		{
			auto lowered = Lower(std::move(name));
			if (g_offered.insert(lowered).second) { g_queue.push_back(std::move(lowered)); }
		}
		if (g_queue.empty()) { return; }
		if (!g_threadStarted)
		{
			g_threadStarted = true;
			std::thread(Worker).detach();
		}
		g_wake.notify_one();
	}

	std::string Describe(const std::string& a_script, const std::string& a_function)
	{
		if (!g_enabled) { return "not classified"; }
		std::scoped_lock l(g_lock);
		const auto found = g_table.find(Lower(a_script));
		if (found == g_table.end()) { return "role pending"; }
		const auto& entry = found->second;
		if (const auto event = entry.events.find(Lower(a_function)); event != entry.events.end())
		{
			if (event->second.role == Role::kNeverTouch) { return std::format("never touch: {}", event->second.reason); }
			return std::format("this event {}; script {}", RoleName(event->second.role), RoleName(entry.role));
		}
		if (entry.role == Role::kNeverTouch) { return std::format("never touch: {}", entry.reason); }
		return RoleName(entry.role);
	}

	void Audit(const std::vector<std::pair<std::string, std::string>>& a_edges)
	{
		if (!g_enabled || a_edges.empty()) { return; }
		std::vector<std::pair<std::string, std::string>> flipped;
		{
			std::scoped_lock l(g_lock);
			g_edgesChecked += a_edges.size();
			for (const auto& [script, callee] : a_edges)
			{
				const auto key = Lower(script);
				const auto found = g_table.find(key);
				if (found == g_table.end() || found->second.role == Role::kNeverTouch || found->second.overridden) { continue; }
				const auto lowered = Lower(callee);
				if (found->second.accounted.contains(lowered)) { continue; }

				const auto what = std::format("calling {}", callee);
				logger::error("ROLE ERROR: {} was labelled {}, but was seen {}, which its code was not found to do. It is now never touch.", key, RoleName(found->second.role), what);
				auto& entry = found->second;
				entry.role = Role::kNeverTouch;
				entry.reason = "the listener saw it: " + what;
				entry.flipped = true;
				for (auto& [eventName, event] : entry.events)
				{
					event.role = Role::kNeverTouch;
					event.reason = entry.reason;
				}
				g_flips[key] = what;
				g_errors++;
				g_dirty = true;
				flipped.emplace_back(key, what);
			}
		}
		if (flipped.empty()) { return; }
		std::ofstream file(LogFile("SkyrimScriptManager_Flips.txt"), std::ios::app);
		for (const auto& [script, what] : flipped) { file << script << '\t' << what << '\n'; }
		WriteTable();
	}

	std::string SummaryLine()
	{
		if (!g_enabled) { return "Roles: off"; }
		std::scoped_lock l(g_lock);
		int counts[3]{};
		for (const auto& [name, entry] : g_table) { counts[static_cast<int>(entry.role)]++; }
		return std::format("Roles: {} scripts read ({} may drop, {} may wait, {} never touch), {} waiting to be read, {} unreadable | listener: {} calls checked, {} errors",
			g_table.size(), counts[0], counts[1], counts[2], g_queue.size(), g_readFailures, g_edgesChecked, g_errors);
	}

	std::string StateJsonFragment()
	{
		std::scoped_lock l(g_lock);
		int counts[3]{};
		for (const auto& [name, entry] : g_table) { counts[static_cast<int>(entry.role)]++; }
		return std::format(R"("roles":{{"read":{},"mayDrop":{},"mayWait":{},"neverTouch":{},"waiting":{},"unreadable":{},"callsChecked":{},"errors":{}}})",
			g_table.size(), counts[0], counts[1], counts[2], g_queue.size(), g_readFailures, g_edgesChecked, g_errors);
	}
}
