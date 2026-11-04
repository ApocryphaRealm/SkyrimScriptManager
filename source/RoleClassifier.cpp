// Own code, GPL-3.0-or-later (2026-11-04). The .pex layout follows the format as documented on the UESP wiki and by Champollion's
// reader; no code is taken from either.
//
// The judgement is a short allow list, and the rule is that anything not on it is never touched. So a mistake in a list can only
// make a script more careful than it needed to be, unless a call on a list does more than its name says.
#include "RoleClassifier.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace RoleClassifier
{
	const char* RoleName(Role a_role)
	{
		switch (a_role)
		{
		case Role::kMayDrop: return "may drop";
		case Role::kMayWait: return "may wait";
		default: return "never touch";
		}
	}

	std::string Lower(std::string a_text)
	{
		std::ranges::transform(a_text, a_text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return a_text;
	}

	namespace
	{
		class Reader
		{
		public:
			Reader(const unsigned char* a_data, std::size_t a_size) : _data(a_data), _size(a_size) {}

			bool ok() const { return _ok; }

			std::uint8_t u8()
			{
				if (_at + 1 > _size) { _ok = false; return 0; }
				return _data[_at++];
			}
			std::uint16_t u16()
			{
				if (_at + 2 > _size) { _ok = false; return 0; }
				const auto v = static_cast<std::uint16_t>((_data[_at] << 8) | _data[_at + 1]);
				_at += 2;
				return v;
			}
			std::uint32_t u32()
			{
				if (_at + 4 > _size) { _ok = false; return 0; }
				const auto v = (static_cast<std::uint32_t>(_data[_at]) << 24) | (static_cast<std::uint32_t>(_data[_at + 1]) << 16) | (static_cast<std::uint32_t>(_data[_at + 2]) << 8) | _data[_at + 3];
				_at += 4;
				return v;
			}
			void skip(std::size_t a_count)
			{
				if (_at + a_count > _size) { _ok = false; return; }
				_at += a_count;
			}
			std::string text()
			{
				const auto length = u16();
				if (_at + length > _size) { _ok = false; return {}; }
				std::string s(reinterpret_cast<const char*>(_data + _at), length);
				_at += length;
				return s;
			}

		private:
			const unsigned char* _data;
			std::size_t _size;
			std::size_t _at = 0;
			bool _ok = true;
		};

		struct Value
		{
			std::uint8_t kind = 0;   // 0 none, 1 identifier, 2 string, 3 int, 4 float, 5 bool
			std::uint16_t index = 0;
			std::int32_t number = 0;
		};

		Value ReadValue(Reader& a_in)
		{
			Value v;
			v.kind = a_in.u8();
			switch (v.kind)
			{
			case 0: break;
			case 1:
			case 2: v.index = a_in.u16(); break;
			case 3: v.number = static_cast<std::int32_t>(a_in.u32()); break;
			case 4: a_in.u32(); break;
			case 5: a_in.u8(); break;
			default: break;
			}
			return v;
		}

		// Fixed arguments of each instruction. Those that write name their target at the index in kWrites (-1 for none).
		struct Op
		{
			int fixed;
			bool variable;
			int writes;
		};
		constexpr Op kOps[] = {
			{ 0, false, -1 },  // nop
			{ 3, false, 0 }, { 3, false, 0 }, { 3, false, 0 }, { 3, false, 0 }, { 3, false, 0 }, { 3, false, 0 }, { 3, false, 0 }, { 3, false, 0 }, { 3, false, 0 },  // iadd .. imod
			{ 2, false, 0 }, { 2, false, 0 }, { 2, false, 0 },  // not, ineg, fneg
			{ 2, false, 0 },  // assign
			{ 2, false, 0 },  // cast
			{ 3, false, 0 }, { 3, false, 0 }, { 3, false, 0 }, { 3, false, 0 }, { 3, false, 0 },  // cmp
			{ 1, false, -1 },  // jmp
			{ 2, false, -1 }, { 2, false, -1 },  // jmpt, jmpf
			{ 3, true, 2 },  // callmethod: name, object, result, args
			{ 2, true, 1 },  // callparent: name, result, args
			{ 3, true, 2 },  // callstatic: type, name, result, args
			{ 1, false, -1 },  // return
			{ 3, false, 0 },  // strcat
			{ 3, false, 2 },  // propget: name, object, result
			{ 3, false, -1 },  // propset: name, object, value
			{ 2, false, 0 },  // array_create
			{ 2, false, 0 },  // array_length
			{ 3, false, 0 },  // array_getelement
			{ 3, false, -1 },  // array_setelement
			{ 4, false, 0 },  // array_findelement
			{ 4, false, 0 },  // array_rfindelement
		};
		constexpr int kCallMethod = 23, kCallParent = 24, kCallStatic = 25, kPropGet = 28, kPropSet = 29;
		constexpr int kOpCount = static_cast<int>(sizeof(kOps) / sizeof(kOps[0]));

		struct Parsing
		{
			std::vector<std::string> strings;
			std::unordered_map<std::string, std::string> objectVars;   // lower name -> lower type
			std::unordered_set<std::string> properties;                // lower name
			Pex* out = nullptr;
		};

		const std::string& Str(const Parsing& a_p, std::uint16_t a_index)
		{
			static const std::string empty;
			return a_index < a_p.strings.size() ? a_p.strings[a_index] : empty;
		}

		bool ReadFunction(Reader& a_in, Parsing& a_p, const std::string& a_name)
		{
			a_in.u16();  // return type
			a_in.u16();  // doc string
			a_in.u32();  // user flags
			const auto flags = a_in.u8();
			const bool isGlobal = (flags & 1) != 0;
			const bool isNative = (flags & 2) != 0;

			std::unordered_map<std::string, std::string> types;
			const auto params = a_in.u16();
			for (int i = 0; i < params && a_in.ok(); ++i)
			{
				const auto name = a_in.u16();
				const auto type = a_in.u16();
				types[Lower(Str(a_p, name))] = Lower(Str(a_p, type));
			}
			const auto locals = a_in.u16();
			std::unordered_set<std::string> localNames;
			for (int i = 0; i < locals && a_in.ok(); ++i)
			{
				const auto name = a_in.u16();
				const auto type = a_in.u16();
				auto lowered = Lower(Str(a_p, name));
				types[lowered] = Lower(Str(a_p, type));
				localNames.insert(std::move(lowered));
			}
			for (const auto& [name, type] : types) { (void)name; (void)type; }

			auto& pex = *a_p.out;
			const auto typeOf = [&](const std::string& a_lowerName) -> std::string {
				if (a_lowerName == "self") { return pex.name; }
				if (const auto found = types.find(a_lowerName); found != types.end()) { return found->second; }
				if (const auto found = a_p.objectVars.find(a_lowerName); found != a_p.objectVars.end()) { return found->second; }
				return {};
			};

			const auto instructions = a_in.u16();
			// The compiler writes GetState and GotoState into scripts itself, with calls to OnBeginState and OnEndState. They are
			// the state machinery, not something the script chose to do; a call of GotoState elsewhere is what counts.
			const auto loweredName = Lower(a_name);
			const bool compilerMade = loweredName == "gotostate" || loweredName == "getstate";
			if (instructions > 0 && !isNative && !compilerMade)
			{
				const auto lowered = Lower(a_name);
				pex.functions.insert(lowered);
				pex.hasCode = true;
				if (lowered.starts_with("on")) { pex.events.insert(lowered); }
			}
			if (isGlobal && !isNative) { pex.hasGlobalFunctions = true; }

			std::vector<Value> args;
			for (int i = 0; i < instructions && a_in.ok(); ++i)
			{
				const int op = a_in.u8();
				if (op >= kOpCount)
				{
					pex.undecoded.push_back("an unknown instruction");
					return false;
				}
				args.clear();
				for (int k = 0; k < kOps[op].fixed; ++k) { args.push_back(ReadValue(a_in)); }
				if (kOps[op].variable)
				{
					const auto count = ReadValue(a_in);
					for (int k = 0; k < count.number && a_in.ok(); ++k) { args.push_back(ReadValue(a_in)); }
				}

				const auto identifier = [&](int a_at) -> std::string {
					if (a_at >= static_cast<int>(args.size()) || args[a_at].kind != 1) { return {}; }
					return Lower(Str(a_p, args[a_at].index));
				};

				if (compilerMade) { continue; }

				if (kOps[op].writes >= 0)
				{
					const auto target = identifier(kOps[op].writes);
					if (!target.empty() && !localNames.contains(target) && !types.contains(target) && a_p.objectVars.contains(target)) { pex.writesOwnState = true; }
				}

				switch (op)
				{
				case kCallMethod:
					{
						Call call;
						call.method = identifier(0);
						call.type = typeOf(identifier(1));
						call.function = a_name;
						if (call.method == "gotostate") { pex.changesState = true; }
						pex.calls.push_back(std::move(call));
						break;
					}
				case kCallParent:
					{
						Call call;
						call.method = identifier(0);
						call.type = pex.parent;
						call.function = a_name;
						call.isParent = true;
						pex.calls.push_back(std::move(call));
						break;
					}
				case kCallStatic:
					{
						Call call;
						call.type = identifier(0);
						call.method = identifier(1);
						call.function = a_name;
						call.isStatic = true;
						pex.calls.push_back(std::move(call));
						break;
					}
				case kPropGet:
					{
						const auto object = identifier(1);
						if (object != "self") { pex.foreignPropertyReads.push_back({ typeOf(object) + "." + identifier(0), loweredName }); }
						break;
					}
				case kPropSet:
					{
						const auto object = identifier(1);
						if (object != "self") { pex.foreignPropertyWrites.push_back({ typeOf(object) + "." + identifier(0), loweredName }); }
						else { pex.writesOwnState = true; }
						break;
					}
				default:
					break;
				}
			}
			return a_in.ok();
		}

		// ---- the lists ----

		// The game's own script types, which hold native functions. Their code is not read: a call on them is judged by the lists.
		const std::unordered_map<std::string, std::string>& NativeTypes()
		{
			static const std::unordered_map<std::string, std::string> types = {
				{ "form", "" }, { "objectreference", "form" }, { "actor", "objectreference" }, { "activemagiceffect", "" }, { "alias", "" }, { "referencealias", "alias" }, { "locationalias", "alias" },
				{ "actorbase", "form" }, { "race", "form" }, { "faction", "form" }, { "keyword", "form" }, { "locationreftype", "keyword" }, { "magiceffect", "form" }, { "spell", "form" }, { "enchantment", "form" }, { "ingredient", "form" }, { "potion", "form" }, { "scroll", "form" }, { "shout", "form" }, { "wordofpower", "form" },
				{ "effectshader", "form" }, { "visualeffect", "form" }, { "sound", "form" }, { "sounddescriptor", "form" }, { "soundcategory", "form" }, { "musictype", "form" }, { "imagespacemodifier", "form" }, { "impactdataset", "form" }, { "explosion", "form" }, { "projectile", "form" }, { "hazard", "form" }, { "art", "form" }, { "shaderparticlegeometry", "form" }, { "imagespace", "form" },
				{ "quest", "form" }, { "scene", "form" }, { "topic", "form" }, { "topicinfo", "form" }, { "package", "form" }, { "message", "form" }, { "globalvariable", "form" }, { "formlist", "form" }, { "leveledactor", "form" }, { "leveleditem", "form" }, { "leveledspell", "form" }, { "cell", "form" }, { "worldspace", "form" }, { "location", "form" }, { "encounterzone", "form" }, { "weather", "form" }, { "idle", "form" }, { "perk", "form" }, { "outfit", "form" }, { "armor", "form" }, { "armoraddon", "form" }, { "weapon", "form" }, { "ammo", "form" }, { "book", "form" }, { "key", "form" }, { "miscobject", "form" }, { "soulgem", "form" }, { "apparatus", "form" }, { "constructibleobject", "form" }, { "container", "form" }, { "activator", "form" }, { "door", "form" }, { "flora", "form" }, { "furniture", "form" }, { "static", "form" }, { "tree", "form" }, { "light", "form" }, { "voicetype", "form" }, { "class", "form" }, { "colorform", "form" }, { "combatstyle", "form" }, { "defaultobjectmanager", "form" }, { "equipslot", "form" }, { "headpart", "form" }, { "textureset", "form" }, { "treeobject", "form" }, { "talkingactivator", "activator" }, { "associationtype", "form" }, { "dlc1scwispwallscript", "" },
				{ "game", "" }, { "utility", "" }, { "debug", "" }, { "math", "" }, { "ui", "" }, { "input", "" }, { "skse", "" }, { "modevent", "" }, { "stringutil", "" }, { "netimmerse", "" }, { "wornobject", "" },
			};
			return types;
		}

		// What a script may start from. A quest, an alias, a scene or a fragment stays out whatever it calls: their events drive progress.
		const std::set<std::string> kAllowedRoots = { "activemagiceffect", "objectreference", "actor" };

		std::unordered_set<std::string> MakeSet(std::initializer_list<const char*> a_items)
		{
			std::unordered_set<std::string> set;
			for (const auto* item : a_items) { set.insert(item); }
			return set;
		}

		// Reads, and work on the script's own numbers: nothing outside the script changes.
		const std::unordered_set<std::string>& ReadCalls()
		{
			static const auto set = MakeSet({
				"form.getformid", "form.getname", "form.getweight", "form.getgoldvalue", "form.haskeyword", "form.getnumkeywords", "form.getnthkeyword", "form.gettype", "form.isplayable", "form.getworldmodelpath",
				"objectreference.getdistance", "objectreference.getpositionx", "objectreference.getpositiony", "objectreference.getpositionz", "objectreference.getanglex", "objectreference.getangley", "objectreference.getanglez",
				"objectreference.getbaseobject", "objectreference.getparentcell", "objectreference.getworldspace", "objectreference.is3dloaded", "objectreference.isdisabled", "objectreference.isdeleted", "objectreference.isnearplayer", "objectreference.isininterior",
				"objectreference.hasnode", "objectreference.getitemcount", "objectreference.getcurrentlocation", "objectreference.getlinkedref", "objectreference.getheight", "objectreference.getwidth", "objectreference.getlength", "objectreference.getscale", "objectreference.getvoicetype", "objectreference.getmass", "objectreference.getnumitems", "objectreference.isfurnitureinuse", "objectreference.isactivationblocked", "objectreference.isincombat",
				"actor.getactorbase", "actor.getleveledactorbase", "actor.getrace", "actor.isdead", "actor.isincombat", "actor.getcombattarget", "actor.getcombatstate", "actor.getav", "actor.getactorvalue", "actor.getbaseav", "actor.getbaseactorvalue", "actor.getavpercentage", "actor.getactorvaluepercentage", "actor.getlevel", "actor.hasspell", "actor.hasmagiceffect", "actor.hasmagiceffectwithkeyword", "actor.hasperk", "actor.isbleedingout", "actor.iscommandedactor", "actor.isweapondrawn", "actor.issneaking", "actor.issprinting", "actor.isrunning", "actor.isswimming", "actor.isunconscious", "actor.isinfaction", "actor.isequipped", "actor.isonmount", "actor.getsitstate", "actor.getsleepstate", "actor.getflyingstate", "actor.isflying", "actor.isghost", "actor.isessential", "actor.getequippedweapon", "actor.getequippedshield", "actor.getequippedspell", "actor.getwornform", "actor.getequippedarmorinslot", "actor.isdoingfavor", "actor.getcurrentpackage", "actor.isplayerteammate", "actor.isarrested", "actor.getrelationshiprank", "actor.ishostiletoactor", "actor.isininterior", "actor.isblocking", "actor.isbeingridden", "actor.isdetectedby", "actor.isguard", "actor.isinkillmove", "actor.isoverencumbered", "actor.istrespassing", "actor.getcrimefaction", "actor.getfactionrank", "actor.getfactionreaction", "actor.getgoldamount", "actor.getlightlevel", "actor.getvoicerecoverytime", "actor.getwarmthrating", "actor.iswinterproof",
				"actorbase.getsex", "actorbase.getrace", "actorbase.getclass", "actorbase.isunique", "actorbase.isessential", "actorbase.isprotected", "actorbase.getoutfit", "actorbase.getnumheadparts",
				"race.isracevampire", "race.getspellcount", "race.ispurchasable",
				"activemagiceffect.gettargetactor", "activemagiceffect.getcasteractor", "activemagiceffect.getbaseobject", "activemagiceffect.getduration", "activemagiceffect.gettimeelapsed", "activemagiceffect.getmagnitude",
				"magiceffect.getcastingtype", "magiceffect.getassociatedskill", "magiceffect.getarea", "magiceffect.getskilllevel", "magiceffect.geteffectshader", "magiceffect.getenchantshader", "spell.getmagickacost", "spell.getnumeffects", "spell.getnthmagiceffect", "spell.getnthduration", "spell.getnthmagnitude",
				"weapon.getweapontype", "weapon.getbasedamage", "weapon.isbow", "armor.getslotmask", "armor.isheavyarmor", "armor.islightarmor",
				"game.getplayer", "game.getform", "game.getformfromfile", "game.getgamesettingfloat", "game.getgamesettingint", "game.getgamesettingstring", "game.getrealhourspassed", "game.getcameraobject", "game.getcurrentcrosshairref", "game.getmodbyname", "game.isfighttingcontrolsenabled", "game.ismovementcontrolsenabled", "game.isjournalcontrolsenabled", "game.getcameraobject", "game.getplayergrabbedref", "game.getcurrentconsoleref", "game.getlightmodcount",
				"utility.randomint", "utility.randomfloat", "utility.getcurrentrealtime", "utility.getcurrentgametime", "utility.isinmenumode", "utility.gameTimeToString", "utility.getinisetting", "utility.getinifloat", "utility.getiniint", "utility.getinibool", "utility.getinistring", "utility.getcurrentweather",
				"math.abs", "math.acos", "math.asin", "math.atan", "math.ceiling", "math.cos", "math.degreestoradians", "math.floor", "math.pow", "math.radianstodegrees", "math.sin", "math.sqrt", "math.tan", "math.logicaland", "math.logicalor", "math.logicalxor", "math.logicalnot", "math.leftshift", "math.rightshift",
				"debug.trace", "debug.tracestack", "debug.traceuser", "debug.getplatformname", "debug.getversionnumber", "debug.traceandbox",
				"globalvariable.getvalue", "globalvariable.getvalueint", "formlist.getat", "formlist.getsize", "formlist.hasform", "formlist.find", "keyword.getstring", "quest.isrunning", "quest.getstage", "quest.getstagedone", "quest.isstagedone", "quest.iscompleted", "quest.isactive", "quest.isstarting", "quest.isstopping", "quest.isstopped", "location.ischild", "location.isloaded", "location.haskeyword", "cell.isinterior", "cell.isattached", "weather.getcurrentweather", "weather.getclassification", "weather.getoutgoingweather", "weather.getcurrentweathertransition", "weather.getskymode", "stringutil.getlength", "stringutil.substring", "stringutil.find", "stringutil.getnthchar", "input.iskeypressed",
			});
			return set;
		}

		// A look or a sound that plays once and ends by itself.
		const std::unordered_set<std::string>& OneShotCalls()
		{
			static const auto set = MakeSet({
				"sound.play", "objectreference.playimpacteffect", "game.shakecamera", "game.shakecontroller", "debug.notification", "sound.setinstancevolume",
			});
			return set;
		}

		// A look that stays on until something ends it, and what a script does with its own time and state. Fine late, not fine never.
		const std::unordered_set<std::string>& PairedCalls()
		{
			static const auto set = MakeSet({
				"effectshader.play", "effectshader.stop", "visualeffect.play", "visualeffect.stop", "imagespacemodifier.apply", "imagespacemodifier.applycrossfade", "imagespacemodifier.remove", "imagespacemodifier.popto", "imagespacemodifier.removecrossfade", "sound.playandwait", "sound.stopinstance",
				"utility.wait", "utility.waitmenumode", "utility.waitgametime",
				"form.registerforupdate", "form.registerforsingleupdate", "form.unregisterforupdate", "form.registerforupdategametime", "form.registerforsingleupdategametime", "form.unregisterforupdategametime",
				"activemagiceffect.registerforupdate", "activemagiceffect.registerforsingleupdate", "activemagiceffect.unregisterforupdate", "activemagiceffect.registerforupdategametime", "activemagiceffect.registerforsingleupdategametime", "activemagiceffect.unregisterforupdategametime",
				"form.registerforanimationevent", "form.unregisterforanimationevent", "activemagiceffect.registerforanimationevent", "activemagiceffect.unregisterforanimationevent",
				"objectreference.gotostate", "actor.gotostate", "activemagiceffect.gotostate", "form.gotostate", "objectreference.getstate", "activemagiceffect.getstate", "form.getstate",
				"objectreference.setalpha", "actor.setalpha",
			});
			return set;
		}

		// Events someone is waiting on or that set an object up. A late answer to these is itself a bug, so they stay out, whatever they call.
		const std::set<std::string> kNeverEvents = {
			"oninit", "onreset", "onactivate", "ontrigger", "ontriggerenter", "ontriggerleave", "onopen", "onclose", "onread", "onequipped", "onunequipped", "oncontaineritemadded", "onitemadded", "onitemremoved",
			"ondeath", "ondying", "onenterbleedout", "onlocationchange", "onsleepstart", "onsleepstop", "onstoryactivateactor", "onplayerloadgame", "onlockstatechanged", "onsit", "ongetup", "onrelease", "ongrab", "oncombatstatechanged", "onchangelocation", "ondestructionstagechanged",
		};

		enum class Kind { kRead, kOneShot, kPaired, kUnknown };

		// Finds a native call on a type and its native parents.
		Kind JudgeNative(std::string a_type, const std::string& a_method)
		{
			const auto& natives = NativeTypes();
			for (int guard = 0; guard < 8 && !a_type.empty(); ++guard)
			{
				const auto key = a_type + "." + a_method;
				if (ReadCalls().contains(key)) { return Kind::kRead; }
				if (OneShotCalls().contains(key)) { return Kind::kOneShot; }
				if (PairedCalls().contains(key)) { return Kind::kPaired; }
				const auto found = natives.find(a_type);
				if (found == natives.end()) { break; }
				a_type = found->second;
			}
			return Kind::kUnknown;
		}
	}

	bool Parse(const unsigned char* a_data, std::size_t a_size, Pex& a_out, std::string& a_error)
	{
		Reader in(a_data, a_size);
		if (in.u32() != 0xFA57C0DE)
		{
			a_error = "not a compiled script (wrong magic number)";
			return false;
		}
		in.u8();   // major
		in.u8();   // minor
		const auto game = in.u16();
		if (game != 1)
		{
			a_error = "not a Skyrim script";
			return false;
		}
		in.skip(8);  // compile time
		in.text();   // source file
		in.text();   // user
		in.text();   // machine

		Parsing parsing;
		parsing.out = &a_out;
		const auto stringCount = in.u16();
		parsing.strings.reserve(stringCount);
		for (int i = 0; i < stringCount && in.ok(); ++i) { parsing.strings.push_back(in.text()); }

		if (in.u8() != 0)
		{
			in.skip(8);  // modification time
			const auto debugFunctions = in.u16();
			for (int i = 0; i < debugFunctions && in.ok(); ++i)
			{
				in.u16();  // object
				in.u16();  // state
				in.u16();  // function
				in.u8();   // type
				const auto lines = in.u16();
				in.skip(static_cast<std::size_t>(lines) * 2);
			}
		}

		const auto userFlags = in.u16();
		for (int i = 0; i < userFlags && in.ok(); ++i)
		{
			in.u16();
			in.u8();
		}

		const auto objects = in.u16();
		for (int o = 0; o < objects && in.ok(); ++o)
		{
			const auto name = in.u16();
			in.u32();  // size
			const auto parent = in.u16();
			in.u16();  // doc string
			in.u32();  // flags
			in.u16();  // auto state

			if (o == 0)
			{
				a_out.name = Lower(Str(parsing, name));
				a_out.parent = Lower(Str(parsing, parent));
			}

			const auto variables = in.u16();
			for (int v = 0; v < variables && in.ok(); ++v)
			{
				const auto varName = in.u16();
				const auto varType = in.u16();
				in.u32();
				ReadValue(in);
				parsing.objectVars[Lower(Str(parsing, varName))] = Lower(Str(parsing, varType));
			}

			const auto properties = in.u16();
			for (int p = 0; p < properties && in.ok(); ++p)
			{
				const auto propName = in.u16();
				const auto propType = in.u16();
				in.u16();  // doc string
				in.u32();  // user flags
				const auto flags = in.u8();
				const auto lowered = Lower(Str(parsing, propName));
				parsing.properties.insert(lowered);
				parsing.objectVars.try_emplace(lowered, Lower(Str(parsing, propType)));
				if (flags & 4)
				{
					in.u16();  // auto variable
					if (o == 0) { a_out.autoProperties.insert(lowered); }
				}
				else
				{
					if (flags & 1)
					{
						if (!ReadFunction(in, parsing, lowered + ".get")) { break; }
					}
					if (flags & 2)
					{
						if (!ReadFunction(in, parsing, lowered + ".set")) { break; }
					}
				}
			}

			const auto states = in.u16();
			for (int s = 0; s < states && in.ok(); ++s)
			{
				in.u16();  // state name
				const auto functions = in.u16();
				for (int f = 0; f < functions && in.ok(); ++f)
				{
					const auto functionName = in.u16();
					if (!ReadFunction(in, parsing, Str(parsing, functionName))) { break; }
				}
			}
		}

		if (!in.ok() || !a_out.undecoded.empty())
		{
			a_error = a_out.undecoded.empty() ? "the file ended early" : a_out.undecoded.front();
			return false;
		}
		return true;
	}

	Result Classify(const std::string& a_name, const Lookup& a_lookup)
	{
		Result result;
		const auto never = [&](std::string a_reason) -> Result& {
			result.role = Role::kNeverTouch;
			result.reason = std::move(a_reason);
			return result;
		};

		const auto& natives = NativeTypes();
		const auto name = Lower(a_name);
		if (natives.contains(name)) { return never("a game type, not a script of its own"); }

		// The script and its parents, up to the game's type it starts from.
		std::vector<const Pex*> chain;
		std::string root;
		std::string walk = name;
		for (int guard = 0; guard < 16 && !walk.empty(); ++guard)
		{
			if (natives.contains(walk))
			{
				root = walk;
				break;
			}
			const auto* pex = a_lookup(walk);
			if (!pex) { return never(walk == name ? "the script file could not be read" : "its parent " + walk + " could not be read"); }
			chain.push_back(pex);
			if (!result.parents.empty()) { result.parents += " > "; }
			result.parents += walk;
			walk = pex->parent;
		}
		if (!result.parents.empty() && !root.empty()) { result.parents += " > " + root; }
		if (root.empty()) { return never("it is not attached to anything: a library of functions other scripts call"); }
		if (!kAllowedRoots.contains(root)) { return never("it extends " + root + ", and those drive progress"); }

		std::set<std::string> own;
		std::set<std::string> ownNames;
		for (const auto* pex : chain)
		{
			ownNames.insert(pex->name);
			own.insert(pex->functions.begin(), pex->functions.end());
		}

		const auto isOwnType = [&](const std::string& a_type) { return ownNames.contains(a_type) || a_type == root; };

		for (const auto* pex : chain)
		{
			for (const auto& function : pex->functions) { result.accounted.insert(function); }
			for (const auto& call : pex->calls) { result.accounted.insert(call.method); }
		}
		result.accounted.insert("get");
		result.accounted.insert("set");

		// A stored property of another script can be read with no code running. Anything else about another script's property is out.
		const auto isStoredProperty = [&](const std::string& a_typeDotName) {
			const auto dot = a_typeDotName.find('.');
			if (dot == std::string::npos || dot == 0) { return false; }
			const auto property = a_typeDotName.substr(dot + 1);
			auto look = a_typeDotName.substr(0, dot);
			for (int guard = 0; guard < 16 && !look.empty(); ++guard)
			{
				const auto* other = a_lookup(look);
				if (!other) { return false; }
				if (other->autoProperties.contains(property)) { return true; }
				look = other->parent;
			}
			return false;
		};

		struct Verdict
		{
			Kind kind = Kind::kRead;
			std::string blocker;   // not empty: this call rules it out
			std::string note;      // the call that made it a wait
			bool internal = false; // its own function: follow it
		};

		const auto judge = [&](const Pex& a_pex, const Call& a_call) -> Verdict {
			Verdict v;
			const auto where = " (in " + a_pex.name + "." + a_call.function + ")";
			if (a_call.type.empty())
			{
				v.blocker = "calls " + a_call.method + " on an object whose type is not known" + where;
				return v;
			}
			if (a_call.method == "onbeginstate" || a_call.method == "onendstate") { return v; }

			// Its own function, or a parent's, is judged where it is written.
			if (isOwnType(a_call.type) && own.contains(a_call.method))
			{
				v.internal = true;
				return v;
			}

			auto type = a_call.type;
			if (!natives.contains(type))
			{
				if (!isOwnType(type))
				{
					// Another script's type: fine only if the call is really a native one its parents have.
					std::string look = type;
					bool ownCode = false;
					bool known = false;
					for (int guard = 0; guard < 16 && !look.empty(); ++guard)
					{
						if (natives.contains(look))
						{
							known = true;
							break;
						}
						const auto* other = a_lookup(look);
						if (!other) { break; }
						if (other->functions.contains(a_call.method))
						{
							ownCode = true;
							break;
						}
						look = other->parent;
					}
					if (ownCode)
					{
						v.blocker = "calls another script: " + type + "." + a_call.method + where;
						return v;
					}
					if (!known)
					{
						v.blocker = "calls " + type + "." + a_call.method + where;
						return v;
					}
					type = look;
				}
				else
				{
					type = root;
				}
			}

			v.kind = JudgeNative(type, a_call.method);
			if (v.kind == Kind::kUnknown)
			{
				v.blocker = "calls " + type + "." + a_call.method + where;
			}
			else if (v.kind == Kind::kPaired)
			{
				v.note = "calls " + type + "." + a_call.method + where;
			}
			return v;
		};

		// What a set of the script's functions adds up to.
		struct Sum
		{
			Kind worst = Kind::kRead;
			std::string waitReason;
			std::vector<std::string> blockers;
			std::set<std::string> seen;
		};
		const auto add = [&](Sum& a_sum, const Pex& a_pex, const Call& a_call) {
			const auto v = judge(a_pex, a_call);
			if (!v.blocker.empty())
			{
				if (a_sum.seen.insert(a_call.type + "." + a_call.method).second) { a_sum.blockers.push_back(v.blocker); }
				return v;
			}
			if (v.kind == Kind::kPaired && a_sum.worst != Kind::kPaired)
			{
				a_sum.worst = Kind::kPaired;
				a_sum.waitReason = v.note;
			}
			else if (v.kind == Kind::kOneShot && a_sum.worst == Kind::kRead)
			{
				a_sum.worst = Kind::kOneShot;
			}
			return v;
		};
		const auto addProperties = [&](Sum& a_sum, const Pex& a_pex, const std::string& a_function) {
			for (const auto& use : a_pex.foreignPropertyWrites)
			{
				if (!a_function.empty() && use.function != a_function) { continue; }
				a_sum.blockers.push_back("it sets a property of another object: " + use.what);
			}
			for (const auto& use : a_pex.foreignPropertyReads)
			{
				if (!a_function.empty() && use.function != a_function) { continue; }
				if (!isStoredProperty(use.what)) { a_sum.blockers.push_back("it reads a computed property of another script: " + use.what); }
			}
		};

		const bool writesState = std::ranges::any_of(chain, [](const Pex* a_pex) { return a_pex->writesOwnState; });
		const bool changesState = std::ranges::any_of(chain, [](const Pex* a_pex) { return a_pex->changesState; });
		std::set<std::string> events;
		for (const auto* pex : chain) { events.insert(pex->events.begin(), pex->events.end()); }
		const bool startAndEnd = (events.contains("oneffectstart") && events.contains("oneffectfinish")) || (events.contains("onload") && events.contains("onunload")) || (events.contains("oncellattach") && events.contains("oncelldetach"));

		const auto conclude = [&](const Sum& a_sum, bool a_noCode, Role& a_role, std::string& a_reason) {
			if (!a_sum.blockers.empty())
			{
				a_role = Role::kNeverTouch;
				a_reason = a_sum.blockers.front();
				if (a_sum.blockers.size() > 1) { a_reason += " (and " + std::to_string(a_sum.blockers.size() - 1) + " more)"; }
			}
			else if (a_sum.worst == Kind::kPaired)
			{
				a_role = Role::kMayWait;
				a_reason = a_sum.waitReason;
			}
			else if (changesState)
			{
				a_role = Role::kMayWait;
				a_reason = "it changes its own state";
			}
			else if (writesState)
			{
				a_role = Role::kMayWait;
				a_reason = "it keeps values of its own between events";
			}
			else if (startAndEnd)
			{
				a_role = Role::kMayWait;
				a_reason = "it answers a start and an end event, and losing one strands the other";
			}
			else
			{
				a_role = Role::kMayDrop;
				a_reason = a_sum.worst == Kind::kOneShot ? "only reads and one-shot looks or sounds" : (a_noCode ? "it has no code, so no event of its own to hold" : "only reads");
			}
		};

		// Each event, by what it can reach through the script's own functions. Two states' versions of a function count as one.
		for (const auto& event : events)
		{
			Sum sum;
			std::set<std::string> visited{ event };
			std::vector<std::string> todo{ event };
			for (const auto& function : own)
			{
				if (function.find('.') != std::string::npos && visited.insert(function).second) { todo.push_back(function); }
			}
			while (!todo.empty())
			{
				const auto function = std::move(todo.back());
				todo.pop_back();
				for (const auto* pex : chain)
				{
					addProperties(sum, *pex, function);
					for (const auto& call : pex->calls)
					{
						if (Lower(call.function) != function) { continue; }
						const auto v = add(sum, *pex, call);
						if (v.internal && visited.insert(call.method).second) { todo.push_back(call.method); }
					}
				}
			}
			EventRole role;
			role.event = event;
			conclude(sum, false, role.role, role.reason);
			if (role.role != Role::kNeverTouch && kNeverEvents.contains(event))
			{
				role.role = Role::kNeverTouch;
				role.reason = "something is waiting on this event, or it sets the object up";
			}
			result.events.push_back(std::move(role));
		}

		// The script as a whole: everything in it, called from an event or not.
		Sum whole;
		for (const auto* pex : chain)
		{
			if (pex->hasGlobalFunctions) { whole.blockers.push_back("it holds global functions other scripts may call"); }
			addProperties(whole, *pex, {});
			for (const auto& call : pex->calls) { add(whole, *pex, call); }
		}
		result.blockers = whole.blockers;
		conclude(whole, own.empty(), result.role, result.reason);
		return result;
	}
}
