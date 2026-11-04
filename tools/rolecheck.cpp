// The classifier outside the game, on a folder of .pex files: the same RoleClassifier.cpp the plugin builds.
// rolecheck <folder> <output.tsv>
#include "RoleClassifier.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::puts("rolecheck <folder of .pex> <output.tsv>");
		return 2;
	}
	namespace fs = std::filesystem;
	std::map<std::string, RoleClassifier::Pex> scripts;
	std::map<std::string, std::string> unreadable;
	std::vector<unsigned char> bytes;
	for (const auto& entry : fs::directory_iterator(argv[1]))
	{
		if (entry.path().extension() != ".pex") { continue; }
		std::ifstream file(entry.path(), std::ios::binary);
		bytes.assign(std::istreambuf_iterator<char>(file), {});
		RoleClassifier::Pex pex;
		std::string error;
		const auto key = RoleClassifier::Lower(entry.path().stem().string());
		if (RoleClassifier::Parse(bytes.data(), bytes.size(), pex, error)) { scripts.emplace(key, std::move(pex)); }
		else { unreadable[key] = error; }
	}
	const RoleClassifier::Lookup lookup = [&](const std::string& a_name) -> const RoleClassifier::Pex* {
		const auto found = scripts.find(a_name);
		return found == scripts.end() ? nullptr : &found->second;
	};
	std::map<std::string, int> counts;
	std::map<std::string, int> eventCounts;
	std::ofstream eventsOut(std::string(argv[2]) + ".events");
	eventsOut << "role\tscript\tevent\treason\n";
	std::ofstream out(argv[2]);
	out << "role\tscript\tparents\treason\n";
	for (const auto& [name, pex] : scripts)
	{
		const auto result = RoleClassifier::Classify(name, lookup);
		counts[RoleClassifier::RoleName(result.role)]++;
		out << RoleClassifier::RoleName(result.role) << '\t' << name << '\t' << result.parents << '\t' << result.reason;
		for (const auto& blocker : result.blockers) { out << '\t' << blocker; }
		out << '\n';
		for (const auto& event : result.events)
		{
			eventCounts[RoleClassifier::RoleName(event.role)]++;
			eventsOut << RoleClassifier::RoleName(event.role) << '\t' << name << '\t' << event.event << '\t' << event.reason << '\n';
		}
	}
	for (const auto& [name, error] : unreadable)
	{
		counts["unreadable"]++;
		out << "never touch\t" << name << "\t\tunreadable: " << error << '\n';
	}
	for (const auto& [role, count] : counts) { std::printf("%s: %d\n", role.c_str(), count); }
	for (const auto& [role, count] : eventCounts) { std::printf("events %s: %d\n", role.c_str(), count); }
	return 0;
}
