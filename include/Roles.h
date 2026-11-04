#pragma once

#include <string>
#include <utility>
#include <vector>

// What each script may be allowed to do when frames are low. This only classifies and reports: nothing is held or dropped because of a role.
namespace Roles
{
	void Configure(bool a_enabled);
	void OnDataLoaded();                                            // main thread
	void OfferLoadedScripts(std::vector<std::string> a_names);      // main thread: names of scripts the VM has loaded
	std::string Describe(const std::string& a_script, const std::string& a_function);  // for the log: "may wait", "never touch: <reason>", "role pending"
	// The listener's check: each pair is a script and the name of a function it was seen calling on a stack.
	void Audit(const std::vector<std::pair<std::string, std::string>>& a_edges);
	std::string SummaryLine();
	std::string StateJsonFragment();
}
