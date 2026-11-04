#pragma once

// Reads a compiled Papyrus script (.pex) and judges it from what it calls. No game code here: tools\rolecheck builds this same
// file outside the game, so what a test run counts is what the plugin does.
#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace RoleClassifier
{
	enum class Role
	{
		kMayDrop,     // only reads and one-shot looks and sounds, with nothing left waiting for a second half
		kMayWait,     // the same, but it keeps its own state, waits, or starts something a later event ends: late is fine, never is not
		kNeverTouch   // anything else, and anything unsure
	};

	const char* RoleName(Role a_role);

	struct Call
	{
		std::string type;      // lower case; "" when the object's type could not be told
		std::string method;    // lower case
		std::string function;  // the function the call is in, as written
		bool isStatic = false;
		bool isParent = false;
	};

	struct Pex
	{
		std::string name;       // lower case
		std::string parent;     // lower case, "" for none
		std::set<std::string> functions;   // lower case, those with code in this script (any state)
		std::set<std::string> events;      // lower case, functions with code whose name starts with "on"
		std::set<std::string> autoProperties;  // lower case: plain stored properties, so reading one runs no code
		std::vector<Call> calls;
		struct PropertyUse
		{
			std::string what;      // "type.property"
			std::string function;  // lower case: the function it is in
		};
		std::vector<PropertyUse> foreignPropertyWrites;
		std::vector<PropertyUse> foreignPropertyReads;
		std::vector<std::string> undecoded;              // anything the reader could not account for
		bool writesOwnState = false;
		bool changesState = false;
		bool hasGlobalFunctions = false;
		bool hasCode = false;
	};

	// False when the bytes are not a readable .pex. a_error says why.
	bool Parse(const unsigned char* a_data, std::size_t a_size, Pex& a_out, std::string& a_error);

	// One event a script answers, judged by what running it can reach. An event is what a manager would hold or thin, so this is
	// the role that matters; the script's own role is its worst event and everything else it holds.
	struct EventRole
	{
		std::string event;   // lower case
		Role role = Role::kNeverTouch;
		std::string reason;
	};

	struct Result
	{
		Role role = Role::kNeverTouch;
		std::string reason;                 // the first call that decided it, and how many more would have
		std::vector<std::string> blockers;  // each distinct thing that makes it never touch
		std::string parents;                // "a > b > c"
		std::vector<EventRole> events;
		std::set<std::string> accounted;    // every function name this script's code was found to call or define, lower case
	};

	// a_lookup returns the parsed script for a lower-case name, or nullptr when there is no such file.
	using Lookup = std::function<const Pex*(const std::string&)>;
	Result Classify(const std::string& a_name, const Lookup& a_lookup);

	std::string Lower(std::string a_text);
}
