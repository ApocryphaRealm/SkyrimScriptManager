// The loop watch and the loop guard. Own code, GPL-3.0-or-later (2026-11-04).
//
// What the game does, read from decrypted dumps of 1.5.97 (id 53149, 52665) and 1.7.104 (id 53960, 53518):
//   - SkyrimVM's sink for TESActiveEffectApplyRemoveEvent is the only sender of OnEffectStart and OnEffectFinish. It builds the
//     handle, then calls IVirtualMachine::SendEvent through the vtable (+0x120, slot 0x24) with an argument object on its own
//     stack, which it destroys when the call returns. The script objects of the effect are bound and unbound by that sink beside
//     the call, not inside it, so a dropped event leaves the VM's books as they were.
//   - An effect's handle is 0x0002'UUUU'FFFFFFFF: U the effect's unique id, F the target actor's form id. So the actor can be had
//     from the handle by arithmetic, with no lookup in the hot path. CheckHandleFormat holds this against the game's own
//     GetHandleForObject at run time; a mismatch turns the guard off and stops grouping effects by actor.
//
// The pair rule, which is what makes dropping safe. Per effect handle, a script sees: every Start it was sent has its Finish, and
// a Finish goes only to a Start it saw. While an actor is armed:
//   - a Start on a handle not seen before goes through (a new effect is not a repeat);
//   - a Start on a handle whose last pair closed is dropped and marked held;
//   - a Finish on a held handle is dropped and closes it, the pair never happened;
//   - a Finish to a Start that was delivered always goes through;
//   - any other event on a held handle delivers the held Start first, then itself: it is never lost or seen before the Start.
// At release, at a save, or if the guard turns itself off, a held handle whose effect still exists gets its Start: the first pair
// and the last Start are what the script sees of a storm. An effect that ended in the storm lost a Start and its Finish together.
//
// Any fault turns the guard off for the session and delivers what it holds; the hook then passes everything through.
#include "PCH.h"

#include "LoopGuard.h"
#include "Roles.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <Windows.h>
#undef GetObject

namespace LoopGuard
{
	namespace
	{
		using VM = RE::BSScript::Internal::VirtualMachine;
		using Handle = RE::VMHandle;

		Settings g_settings;
		SendFn g_original = nullptr;

		inline constexpr Handle kEffectTag = 0x0002000000000000ULL;
		inline constexpr Handle kFormMask = 0x00000000FFFFFFFFULL;
		inline constexpr Handle kOwnerMask = 0xFFFF0000FFFFFFFFULL;

		inline constexpr const char* kAttachEvent = "(cell attach or detach)";
		inline constexpr const char* kLoadEvent = "(3D load or unload)";

		bool IsEffectHandle(Handle a_handle) { return (a_handle >> 48) == 2; }
		std::uint32_t FormOf(Handle a_handle) { return static_cast<std::uint32_t>(a_handle & kFormMask); }

		// ---- names: BSFixedString is interned, so a pointer compare is the whole test ----

		RE::BSFixedString g_nameStart;
		RE::BSFixedString g_nameFinish;
		std::atomic<const char*> g_startPtr{ nullptr };
		std::atomic<const char*> g_finishPtr{ nullptr };
		std::vector<RE::BSFixedString> g_oneShotNames;

		// ---- the table: one row per (object, event), found without a lock ----

		struct Slot
		{
			std::atomic<std::uint64_t> key{ 0 };
			std::atomic<std::uint32_t> count{ 0 };
			std::atomic<Handle> owner{ 0 };
			std::atomic<const char*> name{ nullptr };
			char text[48]{};
			// the rest belongs to the main thread
			float lastRate = 0.0F;
			float peakRate = 0.0F;
			std::uint64_t total = 0;
			std::uint32_t seconds = 0;
			std::uint16_t above = 0;
			std::uint16_t calm = 0;
			std::uint8_t kind = 0;
			bool tripped = false;
			bool armed = false;
		};
		inline constexpr std::size_t kSlots = 4096;
		Slot g_slots[kSlots];

		enum Kind : std::uint8_t
		{
			kKindUnset,
			kKindOneShot,
			kKindAttach,
			kKindOther
		};

		std::uint64_t Mix(Handle a_handle, const char* a_name)
		{
			auto x = a_handle * 0x9E3779B97F4A7C15ULL;
			x ^= reinterpret_cast<std::uintptr_t>(a_name) * 0xC2B2AE3D27D4EB4FULL;
			x ^= x >> 29;
			return x ? x : 1;
		}

		std::atomic<std::uint32_t> g_overflow{ 0 };
		std::atomic<std::uint32_t> g_overflowPeriod{ 0 };

		void Count(Handle a_owner, const char* a_name)
		{
			const auto key = Mix(a_owner, a_name);
			const auto start = static_cast<std::size_t>(key % kSlots);
			for (std::size_t i = 0; i < 16; ++i)
			{
				auto& slot = g_slots[(start + i) % kSlots];
				auto seen = slot.key.load(std::memory_order_acquire);
				if (seen == 0)
				{
					std::uint64_t expected = 0;
					if (slot.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel))
					{
						strncpy_s(slot.text, a_name, _TRUNCATE);
						slot.owner.store(a_owner, std::memory_order_relaxed);
						slot.name.store(a_name, std::memory_order_release);
						slot.count.fetch_add(1, std::memory_order_relaxed);
						return;
					}
					seen = expected;
				}
				if (seen == key)
				{
					slot.count.fetch_add(1, std::memory_order_relaxed);
					return;
				}
			}
			g_overflow.fetch_add(1, std::memory_order_relaxed);
			g_overflowPeriod.fetch_add(1, std::memory_order_relaxed);
		}

		Slot* Find(Handle a_owner, const char* a_name)
		{
			const auto key = Mix(a_owner, a_name);
			const auto start = static_cast<std::size_t>(key % kSlots);
			for (std::size_t i = 0; i < 16; ++i)
			{
				auto& slot = g_slots[(start + i) % kSlots];
				if (slot.key.load(std::memory_order_acquire) == key) { return &slot; }
			}
			return nullptr;
		}

		// ---- the pair table and the armed actors: reached only while an actor is armed ----

		enum PairState : std::uint8_t
		{
			kFree,
			kOpen,    // its Start was delivered, its Finish has not come
			kClosed,  // its last pair is complete
			kHeld     // its Start was dropped and its Finish has not come
		};
		struct Pair
		{
			Handle handle = 0;
			PairState state = kFree;
		};
		inline constexpr std::size_t kPairs = 512;
		Pair g_pairs[kPairs];

		std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
		struct SpinGuard
		{
			SpinGuard()
			{
				while (g_lock.test_and_set(std::memory_order_acquire)) { YieldProcessor(); }
			}
			~SpinGuard() { g_lock.clear(std::memory_order_release); }
		};

		struct Armed
		{
			std::atomic<std::uint32_t> form{ 0 };
			std::atomic<std::uint32_t> dropped{ 0 };
			std::atomic<std::uint32_t> delivered{ 0 };
		};
		inline constexpr std::size_t kMaxArmed = 32;
		Armed g_armed[kMaxArmed];
		std::atomic<int> g_armedCount{ 0 };
		std::atomic<int> g_heldCount{ 0 };

		std::atomic<bool> g_guardLive{ false };
		std::atomic<bool> g_guardOff{ false };
		std::string g_guardOffReason;
		std::atomic<std::uint64_t> g_droppedTotal{ 0 };
		std::atomic<std::uint64_t> g_droppedPeriod{ 0 };
		std::atomic<std::uint64_t> g_heldDelivered{ 0 };
		std::atomic<std::uint32_t> g_pairFull{ 0 };

		enum class Format
		{
			kNotChecked,
			kGood,
			kWrong
		};
		std::atomic<Format> g_format{ Format::kNotChecked };
		std::atomic<std::uint32_t> g_formatChecked{ 0 };

		Pair* FindPair(Handle a_handle, bool a_insert)
		{
			const auto start = static_cast<std::size_t>((a_handle ^ (a_handle >> 32)) * 2654435761u) % kPairs;
			Pair* firstFree = nullptr;
			for (std::size_t i = 0; i < 32; ++i)
			{
				auto& pair = g_pairs[(start + i) % kPairs];
				if (pair.state == kFree)
				{
					if (!firstFree) { firstFree = &pair; }
				}
				else if (pair.handle == a_handle)
				{
					return &pair;
				}
			}
			if (a_insert && firstFree)
			{
				firstFree->handle = a_handle;
				return firstFree;
			}
			return nullptr;
		}

		Armed* FindArmed(std::uint32_t a_form)
		{
			for (auto& armed : g_armed)
			{
				if (armed.form.load(std::memory_order_relaxed) == a_form) { return &armed; }
			}
			return nullptr;
		}

		// ---- delivering a held start ----

		void DeliverInner(VM* a_vm, Handle a_handle, bool& a_sent)
		{
			auto* policy = a_vm->handlePolicy;
			if (!policy || !g_original) { return; }
			auto* effect = static_cast<RE::ActiveEffect*>(policy->GetObjectForHandle(RE::ActiveEffect::VMTYPEID, a_handle));
			if (!effect) { return; }   // the effect has ended: the pair never happened
			RE::Actor* target = effect->GetTargetActor();
			if (!target) { return; }
			const auto casterPtr = effect->GetCasterActor();
			RE::Actor* caster = casterPtr.get();
			// The game passes an object on its own stack and destroys it when the call returns, so the VM does not keep it.
			auto* args = RE::MakeFunctionArguments(std::move(target), std::move(caster));
			g_original(a_vm, a_handle, g_nameStart, args);
			delete args;
			a_sent = true;
		}

		bool Deliver(VM* a_vm, Handle a_handle, bool& a_sent)
		{
			__try
			{
				DeliverInner(a_vm, a_handle, a_sent);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		void PushLine(std::string a_line);

		void TurnGuardOff(VM* a_vm, std::string a_reason);

		void DeliverHeld(VM* a_vm, const std::vector<Handle>& a_handles)
		{
			for (const auto handle : a_handles)
			{
				bool sent = false;
				if (!Deliver(a_vm, handle, sent))
				{
					TurnGuardOff(a_vm, "delivering a held start raised an exception");
					return;
				}
				if (sent)
				{
					g_heldDelivered.fetch_add(1, std::memory_order_relaxed);
					if (auto* armed = FindArmed(FormOf(handle))) { armed->delivered.fetch_add(1, std::memory_order_relaxed); }
				}
			}
		}

		// Takes out every held handle, for one actor or (0) for all of them.
		std::vector<Handle> TakeHeld(std::uint32_t a_form)
		{
			std::vector<Handle> held;
			SpinGuard guard;
			for (auto& pair : g_pairs)
			{
				if (pair.state == kHeld && (a_form == 0 || FormOf(pair.handle) == a_form))
				{
					held.push_back(pair.handle);
					pair.state = kOpen;
					g_heldCount.fetch_sub(1, std::memory_order_relaxed);
				}
			}
			return held;
		}

		void TurnGuardOff(VM* a_vm, std::string a_reason)
		{
			if (g_guardOff.exchange(true)) { return; }
			g_guardLive.store(false);
			{
				SpinGuard guard;
				g_guardOffReason = a_reason;
			}
			PushLine(std::format("LOOP GUARD OFF for this session: {}. Events pass through untouched; the watch carries on reporting.", a_reason));
			if (a_vm) { DeliverHeld(a_vm, TakeHeld(0)); }
			{
				SpinGuard guard;
				for (auto& pair : g_pairs) { pair = Pair{}; }
				for (auto& armed : g_armed) { armed.form.store(0); }
				g_armedCount.store(0);
				g_heldCount.store(0);
			}
		}

		// ---- the filter, on whatever thread sent the event ----

		bool FilterPair(Handle a_handle, bool a_isStart)
		{
			auto* armed = FindArmed(FormOf(a_handle));
			if (!armed) { return false; }
			SpinGuard guard;
			auto* pair = FindPair(a_handle, true);
			if (!pair)
			{
				g_pairFull.fetch_add(1, std::memory_order_relaxed);
				return false;   // no room to remember it: delivered, so nothing goes out of balance
			}
			bool drop = false;
			if (a_isStart)
			{
				switch (pair->state)
				{
				case kFree:   pair->state = kOpen; break;                                                               // not seen before: a new effect, not a repeat
				case kOpen:   drop = true; break;                                                                       // a second start with no finish between
				case kClosed: pair->state = kHeld; g_heldCount.fetch_add(1, std::memory_order_relaxed); drop = true; break;
				case kHeld:   drop = true; break;
				}
			}
			else
			{
				switch (pair->state)
				{
				case kHeld:   pair->state = kClosed; g_heldCount.fetch_sub(1, std::memory_order_relaxed); drop = true; break;   // the pair never happened
				default:      pair->state = kClosed; break;                                                             // its start went through, so its finish does
				}
			}
			if (drop)
			{
				armed->dropped.fetch_add(1, std::memory_order_relaxed);
				g_droppedTotal.fetch_add(1, std::memory_order_relaxed);
				g_droppedPeriod.fetch_add(1, std::memory_order_relaxed);
			}
			return drop;
		}

		void WakeHeld(VM* a_vm, Handle a_handle)
		{
			bool wake = false;
			{
				SpinGuard guard;
				if (auto* pair = FindPair(a_handle, false); pair && pair->state == kHeld)
				{
					pair->state = kOpen;
					g_heldCount.fetch_sub(1, std::memory_order_relaxed);
					wake = true;
				}
			}
			if (wake) { DeliverHeld(a_vm, { a_handle }); }
		}

		// ---- what the object is, for the log ----

		std::string FileOf(const RE::TESForm* a_form)
		{
			const auto* file = a_form ? a_form->GetFile(0) : nullptr;
			return file ? std::string(file->GetFilename()) : std::string("no plugin");
		}

		void DescribeRefInner(std::uint32_t a_form, std::string& a_out, std::string& a_package)
		{
			auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_form);
			if (!ref)
			{
				a_out = std::format("form {:08X}, not loaded", a_form);
				return;
			}
			const char* name = ref->GetName();
			a_out = std::format("{} ({:08X}, {})", name && name[0] ? name : "unnamed", a_form, FileOf(ref));
			if (const auto* base = ref->GetBaseObject())
			{
				const char* baseName = base->GetName();
				a_out += std::format(", base {} ({:08X}, {})", baseName && baseName[0] ? baseName : "unnamed", base->GetFormID(), FileOf(base));
			}
			a_out += ref->IsPersistent() ? ", persistent" : ", not persistent";
			if (auto* player = RE::PlayerCharacter::GetSingleton(); player && player != ref && ref->GetParentCell() && player->GetParentCell())
			{
				a_out += std::format(", {:.0f} units away", player->GetPosition().GetDistance(ref->GetPosition()));
			}
			if (auto* actor = ref->As<RE::Actor>())
			{
				if (const auto* package = actor->GetCurrentPackage())
				{
					a_package = std::format("package {:08X} from {}", package->GetFormID(), FileOf(package));
				}
			}
		}

		bool DescribeRefGuarded(std::uint32_t a_form, std::string& a_out, std::string& a_package)
		{
			__try
			{
				DescribeRefInner(a_form, a_out, a_package);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		std::string DescribeHandleText(VM* a_vm, Handle a_handle)
		{
			auto* policy = a_vm ? a_vm->handlePolicy : nullptr;
			if (!policy) { return std::format("handle {:016X}", a_handle); }
			RE::BSFixedString text;
			policy->ConvertHandleToString(a_handle, text);
			return text.empty() ? std::format("handle {:016X}", a_handle) : std::string(text.c_str());
		}

		// An owner handle from the table: an effect's actor, a bare form id from the attach and load sinks, or a handle as it came.
		bool OwnerIsRef(Handle a_owner) { return IsEffectHandle(a_owner) || (a_owner >> 32) == 0 || (a_owner >> 32) == static_cast<Handle>(RE::FormType::ActorCharacter) || (a_owner >> 32) == static_cast<Handle>(RE::FormType::Reference); }

		std::string DescribeOwner(VM* a_vm, Handle a_owner, std::string* a_package = nullptr)
		{
			std::string package;
			if (OwnerIsRef(a_owner))
			{
				std::string text;
				if (DescribeRefGuarded(FormOf(a_owner), text, package))
				{
					if (a_package) { *a_package = package; }
					return text;
				}
			}
			return DescribeHandleText(a_vm, a_owner);
		}

		void EffectHandlesInner(RE::Actor* a_actor, std::vector<std::pair<Handle, RE::ActiveEffect*>>& a_out)
		{
			auto* target = a_actor->AsMagicTarget();
			auto* list = target ? target->GetActiveEffectList() : nullptr;
			if (!list) { return; }
			const auto form = static_cast<Handle>(a_actor->GetFormID());
			for (auto* effect : *list)
			{
				if (effect) { a_out.emplace_back(kEffectTag | (static_cast<Handle>(effect->usUniqueID) << 32) | form, effect); }
			}
		}

		bool EffectHandles(RE::Actor* a_actor, std::vector<std::pair<Handle, RE::ActiveEffect*>>& a_out)
		{
			__try
			{
				EffectHandlesInner(a_actor, a_out);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		void ScriptNamesInner(VM* a_vm, Handle a_handle, std::set<std::string>& a_out)
		{
			RE::BSSpinLockGuard guard(a_vm->attachedScriptsLock);
			const auto found = a_vm->attachedScripts.find(a_handle);
			if (found == a_vm->attachedScripts.end()) { return; }
			const auto& scripts = found->second;
			for (std::uint32_t i = 0; i < scripts.size(); ++i)
			{
				const auto* object = scripts[i].get();
				const auto* type = object ? object->GetTypeInfo() : nullptr;
				if (type) { a_out.emplace(type->GetName()); }
			}
		}

		bool ScriptNames(VM* a_vm, Handle a_handle, std::set<std::string>& a_out)
		{
			__try
			{
				ScriptNamesInner(a_vm, a_handle, a_out);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// The scripts on an object, each with its role for this event. An effect's scripts are read from the actor's live effects.
		std::string ScriptsText(VM* a_vm, Handle a_owner, const char* a_event, bool a_layoutOK)
		{
			if (!a_layoutOK) { return "not read, the VM layout check failed"; }
			std::set<std::string> names;
			if (IsEffectHandle(a_owner))
			{
				if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(FormOf(a_owner)))
				{
					std::vector<std::pair<Handle, RE::ActiveEffect*>> effects;
					if (EffectHandles(actor, effects))
					{
						for (const auto& [handle, effect] : effects) { ScriptNames(a_vm, handle, names); }
					}
				}
			}
			else
			{
				ScriptNames(a_vm, a_owner, names);
			}
			if (names.empty()) { return "none bound right now"; }
			std::string text;
			for (const auto& name : names)
			{
				if (!text.empty()) { text += "; "; }
				text += std::format("{} [{}]", name, Roles::Describe(name, a_event));
			}
			return text;
		}

		// Holds the arithmetic against the game's own answer. One wrong effect is enough.
		bool CheckHandleFormatInner(VM* a_vm, RE::Actor* a_actor, std::uint32_t& a_checked, std::string& a_wrong)
		{
			auto* policy = a_vm->handlePolicy;
			if (!policy) { return true; }
			std::vector<std::pair<Handle, RE::ActiveEffect*>> effects;
			if (!EffectHandles(a_actor, effects)) { return true; }
			for (const auto& [handle, effect] : effects)
			{
				const auto real = policy->GetHandleForObject(RE::ActiveEffect::VMTYPEID, effect);
				a_checked++;
				if (real != handle)
				{
					a_wrong = std::format("worked out {:016X}, the game says {:016X}", handle, real);
					return false;
				}
			}
			return true;
		}

		bool CheckHandleFormatGuarded(VM* a_vm, RE::Actor* a_actor, std::uint32_t& a_checked, std::string& a_wrong, bool& a_faulted)
		{
			__try
			{
				return CheckHandleFormatInner(a_vm, a_actor, a_checked, a_wrong);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				a_faulted = true;
				return true;
			}
		}

		void CheckHandleFormat(VM* a_vm, RE::Actor* a_actor)
		{
			if (!a_vm || !a_actor || g_format.load() == Format::kWrong) { return; }
			std::uint32_t checked = 0;
			std::string wrong;
			bool faulted = false;
			const bool good = CheckHandleFormatGuarded(a_vm, a_actor, checked, wrong, faulted);
			g_formatChecked.fetch_add(checked, std::memory_order_relaxed);
			if (faulted)
			{
				logger::warn("Effect handle check on {:08X} raised an exception; it stays unproved", a_actor->GetFormID());
				return;
			}
			if (!good)
			{
				g_format.store(Format::kWrong);
				PushLine(std::format("Effect handle format is not what this build read from the game: {}. Effects are no longer grouped by actor.", wrong));
				TurnGuardOff(a_vm, "the effect handle format did not match");
				return;
			}
			if (checked > 0 && g_format.load() == Format::kNotChecked)
			{
				g_format.store(Format::kGood);
				logger::info("Effect handle format checked against the game on {} effects of {:08X}: it matches", checked, a_actor->GetFormID());
			}
		}

		// ---- the attach and load events: public event sources, so no hook ----

		class AttachSink final : public RE::BSTEventSink<RE::TESCellAttachDetachEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent* a_event, RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
			{
				if (a_event && a_event->reference && a_event->reference->Is(RE::FormType::ActorCharacter))
				{
					Count(a_event->reference->GetFormID(), kAttachEvent);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		AttachSink g_attachSink;

		class LoadSink final : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
			{
				if (a_event && a_event->formID != 0) { Count(a_event->formID, kLoadEvent); }
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		LoadSink g_loadSink;

		// ---- what has been seen, for the state tool and the report ----

		std::mutex g_textLock;
		std::deque<std::string> g_lines;
		std::vector<std::string> g_periodLines;

		void PushLine(std::string a_line)
		{
			if (a_line.starts_with("LOOP GUARD OFF") || a_line.starts_with("Effect handle format")) { logger::error("{}", a_line); }
			else { logger::warn("{}", a_line); }
			spdlog::default_logger()->flush();
			std::scoped_lock l(g_textLock);
			g_lines.push_back(a_line);
			if (g_lines.size() > 16) { g_lines.pop_front(); }
		}

		std::atomic<std::uint32_t> g_detectionsNow{ 0 };
		std::atomic<std::uint32_t> g_detectionsTotal{ 0 };
		std::atomic<std::uint32_t> g_detectionsPeriod{ 0 };
		std::atomic<std::uint32_t> g_stackDetections{ 0 };
		std::atomic<std::uint32_t> g_deepDetections{ 0 };
		std::atomic<std::uint64_t> g_scanTicksWorst{ 0 };
		std::atomic<std::uint32_t> g_scans{ 0 };
		std::atomic<float> g_busiestRate{ 0.0F };

		struct Busy
		{
			Handle owner = 0;
			std::string event;
			float rate = 0.0F;
		};
		std::vector<Busy> g_busy;   // this period's busiest (object, event), tripped or not, so a report shows what normal looks like

		std::unordered_map<Handle, std::uint32_t> g_lastWalk;
		std::unordered_map<Handle, std::uint64_t> g_stackReportedAt;
		std::set<std::string> g_deepSeen;

		std::uint64_t g_lastScan = 0;
		std::uint64_t g_nowTicks = 0;
		double g_ticksPerSecond = 1.0;
		bool g_layoutOK = false;
		VM* g_vm = nullptr;

		// ---- the DevBench flood ----

		struct FloodRequest
		{
			std::string target;
			std::string event;
			float rate = 0.0F;
			float seconds = 0.0F;
		};
		std::mutex g_floodLock;
		bool g_floodPending = false;
		FloodRequest g_floodRequest;
		std::atomic<bool> g_floodStop{ false };
		std::atomic<bool> g_floodActive{ false };
		std::atomic<std::uint64_t> g_floodSent{ 0 };
		std::string g_floodResult = "none yet";

		std::vector<Handle> g_floodHandles;
		RE::BSFixedString g_floodEvent;
		RE::ActorHandle g_floodActor;
		bool g_floodEffects = false;
		float g_floodRate = 0.0F;
		std::uint64_t g_floodEnd = 0;
		std::uint64_t g_floodLastFrame = 0;
		double g_floodOwed = 0.0;

		void SetFloodResult(std::string a_text)
		{
			logger::info("Flood: {}", a_text);
			std::scoped_lock l(g_floodLock);
			g_floodResult = std::move(a_text);
		}

		std::vector<Handle> ScriptedEffects(VM* a_vm, RE::Actor* a_actor)
		{
			std::vector<std::pair<Handle, RE::ActiveEffect*>> effects;
			std::vector<Handle> scripted;
			if (!EffectHandles(a_actor, effects)) { return scripted; }
			for (const auto& [handle, effect] : effects)
			{
				std::set<std::string> names;
				ScriptNames(a_vm, handle, names);
				if (!names.empty() && std::ranges::find(scripted, handle) == scripted.end()) { scripted.push_back(handle); }
			}
			return scripted;
		}

		RE::Actor* NearestWithScriptedEffect(VM* a_vm)
		{
			auto* lists = RE::ProcessLists::GetSingleton();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!lists || !player) { return nullptr; }
			RE::Actor* best = nullptr;
			float bestDistance = 0.0F;
			for (auto& handle : lists->highActorHandles)
			{
				auto actor = handle.get();
				if (!actor || actor.get() == player || actor->IsDead()) { continue; }
				const auto distance = player->GetPosition().GetDistance(actor->GetPosition());
				if ((!best || distance < bestDistance) && !ScriptedEffects(a_vm, actor.get()).empty())
				{
					best = actor.get();
					bestDistance = distance;
				}
			}
			return best;
		}

		void BeginFlood(VM* a_vm, const FloodRequest& a_request)
		{
			RE::Actor* actor = nullptr;
			if (a_request.target.empty()) { actor = NearestWithScriptedEffect(a_vm); }
			else
			{
				const auto form = static_cast<RE::FormID>(std::strtoul(a_request.target.c_str(), nullptr, 16));
				actor = RE::TESForm::LookupByID<RE::Actor>(form);
			}
			if (!actor)
			{
				SetFloodResult(std::format("refused: no actor for target '{}'", a_request.target));
				return;
			}
			g_floodHandles.clear();
			g_floodEffects = a_request.event.empty() || a_request.event == "effect";
			if (g_floodEffects)
			{
				g_floodHandles = ScriptedEffects(a_vm, actor);
				if (g_floodHandles.empty())
				{
					SetFloodResult(std::format("refused: {:08X} has no scripted effect", actor->GetFormID()));
					return;
				}
			}
			else
			{
				auto* policy = a_vm->handlePolicy;
				if (!policy)
				{
					SetFloodResult("refused: the VM has no handle policy");
					return;
				}
				g_floodHandles.push_back(policy->GetHandleForObject(static_cast<RE::VMTypeID>(RE::FormType::ActorCharacter), actor));
				g_floodEvent = RE::BSFixedString(a_request.event.c_str());
			}
			g_floodActor = actor->GetHandle();
			g_floodRate = std::clamp(a_request.rate, 1.0F, 500.0F);
			const auto seconds = std::clamp(a_request.seconds, 1.0F, 300.0F);
			g_floodEnd = g_nowTicks + static_cast<std::uint64_t>(seconds * g_ticksPerSecond);
			g_floodLastFrame = g_nowTicks;
			g_floodOwed = 0.0;
			g_floodSent.store(0);
			g_floodStop.store(false);
			g_floodActive.store(true);
			CheckHandleFormat(a_vm, actor);
			SetFloodResult(std::format("running on {:08X}: {} at {:.0f}/s on {} handles for {:.0f} s", actor->GetFormID(), g_floodEffects ? "OnEffectFinish and OnEffectStart" : a_request.event, g_floodRate, g_floodHandles.size(), seconds));
		}

		void RunFloodFrame(VM* a_vm)
		{
			if (!g_floodActive.load(std::memory_order_relaxed)) { return; }
			if (g_floodStop.load() || g_nowTicks >= g_floodEnd)
			{
				g_floodActive.store(false);
				SetFloodResult(std::format("finished: {} events sent", g_floodSent.load()));
				return;
			}
			auto actor = g_floodActor.get();
			if (!actor)
			{
				g_floodActive.store(false);
				SetFloodResult("stopped: the actor is gone");
				return;
			}
			const auto dt = static_cast<double>(g_nowTicks - g_floodLastFrame) / g_ticksPerSecond;
			g_floodLastFrame = g_nowTicks;
			g_floodOwed = std::min(g_floodOwed + dt * g_floodRate, 64.0);
			while (g_floodOwed >= 1.0)
			{
				g_floodOwed -= 1.0;
				for (const auto handle : g_floodHandles)
				{
					if (g_floodEffects)
					{
						RE::Actor* target = actor.get();
						RE::Actor* caster = actor.get();
						auto* finishArgs = RE::MakeFunctionArguments(std::move(target), std::move(caster));
						a_vm->SendEvent(handle, g_nameFinish, finishArgs);
						delete finishArgs;
						target = actor.get();
						caster = actor.get();
						auto* startArgs = RE::MakeFunctionArguments(std::move(target), std::move(caster));
						a_vm->SendEvent(handle, g_nameStart, startArgs);
						delete startArgs;
						g_floodSent.fetch_add(2, std::memory_order_relaxed);
					}
					else
					{
						auto* args = RE::MakeFunctionArguments();
						a_vm->SendEvent(handle, g_floodEvent, args);
						delete args;
						g_floodSent.fetch_add(1, std::memory_order_relaxed);
					}
				}
			}
		}

		// ---- the second-by-second scan ----

		std::uint8_t KindOf(const char* a_name)
		{
			if (a_name == kAttachEvent || a_name == kLoadEvent) { return kKindAttach; }
			for (const auto& name : g_oneShotNames)
			{
				if (name.data() == a_name) { return kKindOneShot; }
			}
			return kKindOther;
		}

		float RateOf(const char* a_name, Handle a_owner)
		{
			const auto* slot = Find(a_owner, a_name);
			return slot ? slot->lastRate : 0.0F;
		}

		bool IsEffectStartSlot(const Slot& a_slot)
		{
			return a_slot.name.load(std::memory_order_relaxed) == g_startPtr.load(std::memory_order_relaxed) && IsEffectHandle(a_slot.owner.load(std::memory_order_relaxed));
		}

		const char* ActionText(const Slot& a_slot, bool a_overstressed)
		{
			if (!g_settings.guard) { return "report only (bLoopGuard=0)"; }
			if (g_guardOff.load()) { return "report only (the guard turned itself off)"; }
			const auto* name = a_slot.name.load(std::memory_order_relaxed);
			const bool pair = name == g_finishPtr.load(std::memory_order_relaxed) || name == g_startPtr.load(std::memory_order_relaxed);
			if (!pair || !IsEffectHandle(a_slot.owner.load(std::memory_order_relaxed))) { return "report only (not safe to drop)"; }
			if (g_format.load() != Format::kGood) { return "report only (the effect handle format is not proved)"; }
			return a_overstressed ? "dropping repeats" : "report only until the VM is overstressed";
		}

		void ReportCause(VM* a_vm, const Slot& a_slot, const std::string& a_package)
		{
			const auto owner = a_slot.owner.load(std::memory_order_relaxed);
			if (!OwnerIsRef(owner)) { return; }
			const auto form = static_cast<Handle>(FormOf(owner));
			const auto attach = RateOf(kAttachEvent, form);
			const auto load = RateOf(kLoadEvent, form);
			std::string reading;
			if (attach >= 1.0F || load >= 1.0F)
			{
				reading = "It is re-attaching, which restarts its abilities. Look at what moves it: a package, or following a leader while not persistent. A patch that changes that is the fix; this mod edits no actor.";
			}
			else
			{
				reading = "Its attach events do not account for this: look at a script adding and removing the effect, or resetting its 3D.";
			}
			PushLine(std::format("LOOP cause: {:.0f}/s attach or detach, {:.0f}/s 3D load or unload on this object{}{}. {}", attach, load, a_package.empty() ? "" : ", ", a_package, reading));
		}

		void Trip(VM* a_vm, Slot& a_slot, bool a_overstressed)
		{
			a_slot.tripped = true;
			a_slot.calm = 0;
			a_slot.total = 0;
			a_slot.seconds = 0;
			g_detectionsNow.fetch_add(1, std::memory_order_relaxed);
			g_detectionsTotal.fetch_add(1, std::memory_order_relaxed);
			g_detectionsPeriod.fetch_add(1, std::memory_order_relaxed);
			const auto owner = a_slot.owner.load(std::memory_order_relaxed);
			if (IsEffectHandle(owner)) { CheckHandleFormat(a_vm, RE::TESForm::LookupByID<RE::Actor>(FormOf(owner))); }
			std::string package;
			const auto who = DescribeOwner(a_vm, owner, &package);
			const auto stackKey = StackKey(owner);
			const auto stacks = g_lastWalk.find(stackKey);
			PushLine(std::format("LOOP: {} on {} at {:.0f}/s for {} s | stacks on it at the last walk: {} | scripts: {} | action: {}",
				a_slot.text, who, a_slot.lastRate, a_slot.above, stacks != g_lastWalk.end() ? std::to_string(stacks->second) : std::string("none seen"),
				ScriptsText(a_vm, owner, a_slot.text, g_layoutOK), ActionText(a_slot, a_overstressed)));
			if (a_slot.kind != kKindOther) { ReportCause(a_vm, a_slot, package); }
		}

		void Arm(VM* a_vm, Slot& a_slot)
		{
			const auto form = FormOf(a_slot.owner.load(std::memory_order_relaxed));
			if (g_format.load() != Format::kGood)
			{
				return;   // unproved or wrong: it stays report only
			}
			for (auto& armed : g_armed)
			{
				std::uint32_t expected = 0;
				if (armed.form.compare_exchange_strong(expected, form))
				{
					armed.dropped.store(0);
					armed.delivered.store(0);
					g_armedCount.fetch_add(1);
					a_slot.armed = true;
					PushLine(std::format("LOOP guard: dropping repeats of OnEffectStart and its OnEffectFinish on {:08X}, the VM is overstressed", form));
					return;
				}
			}
			logger::warn("Loop guard: {} actors are already armed, {:08X} stays report only", kMaxArmed, form);
		}

		void Release(VM* a_vm, Slot& a_slot)
		{
			const auto owner = a_slot.owner.load(std::memory_order_relaxed);
			const auto form = FormOf(owner);
			std::uint64_t dropped = 0;
			std::uint64_t delivered = 0;
			if (a_slot.armed)
			{
				DeliverHeld(a_vm, TakeHeld(form));
				if (auto* armed = FindArmed(form))
				{
					dropped = armed->dropped.load();
					delivered = armed->delivered.load();
				}
				{
					SpinGuard guard;
					for (auto& pair : g_pairs)
					{
						if (pair.state != kFree && FormOf(pair.handle) == form) { pair = Pair{}; }
					}
				}
				if (auto* armed = FindArmed(form))
				{
					armed->form.store(0);
					g_armedCount.fetch_sub(1);
				}
				a_slot.armed = false;
			}
			g_detectionsNow.fetch_sub(1, std::memory_order_relaxed);
			a_slot.tripped = false;
			PushLine(std::format("LOOP ended: {} on {} | {} s, peak {:.0f}/s, {} events | dropped {}, held starts delivered {}",
				a_slot.text, DescribeOwner(a_vm, owner), a_slot.seconds, a_slot.peakRate, a_slot.total, dropped, delivered));
			a_slot.peakRate = 0.0F;
			a_slot.above = 0;
			a_slot.calm = 0;
		}

		void NoteBusy(const Slot& a_slot, float a_rate)
		{
			const auto owner = a_slot.owner.load(std::memory_order_relaxed);
			for (auto& busy : g_busy)
			{
				if (busy.owner == owner && busy.event == a_slot.text)
				{
					busy.rate = std::max(busy.rate, a_rate);
					return;
				}
			}
			if (g_busy.size() < 3)
			{
				g_busy.push_back({ owner, a_slot.text, a_rate });
				return;
			}
			auto least = std::ranges::min_element(g_busy, {}, &Busy::rate);
			if (a_rate > least->rate) { *least = { owner, a_slot.text, a_rate }; }
		}

		void Scan(VM* a_vm, bool a_overstressed)
		{
			const auto seconds = static_cast<double>(g_nowTicks - g_lastScan) / g_ticksPerSecond;
			if (seconds < 1.0) { return; }
			g_lastScan = g_nowTicks;

			LARGE_INTEGER before;
			QueryPerformanceCounter(&before);
			float busiest = 0.0F;
			for (auto& slot : g_slots)
			{
				if (slot.key.load(std::memory_order_acquire) == 0) { continue; }
				const auto* name = slot.name.load(std::memory_order_acquire);
				if (!name) { continue; }   // another thread is still filling it in
				const auto count = slot.count.exchange(0, std::memory_order_relaxed);
				const auto rate = static_cast<float>(count / seconds);
				slot.lastRate = rate;
				if (slot.kind == kKindUnset) { slot.kind = KindOf(name); }
				const float limit = slot.kind == kKindOneShot ? g_settings.oneShotRate : slot.kind == kKindAttach ? g_settings.attachRate : g_settings.otherRate;
				const int need = slot.kind == kKindOther ? g_settings.otherSustainSeconds : g_settings.sustainSeconds;
				if (count > 0)
				{
					busiest = std::max(busiest, rate);
					NoteBusy(slot, rate);
				}
				if (slot.tripped)
				{
					slot.total += count;
					slot.seconds++;
					slot.peakRate = std::max(slot.peakRate, rate);
					if (rate < limit)
					{
						if (++slot.calm >= g_settings.releaseSeconds) { Release(a_vm, slot); }
					}
					else
					{
						slot.calm = 0;
					}
					if (slot.tripped && !slot.armed && g_settings.guard && a_overstressed && g_guardLive.load(std::memory_order_relaxed) && IsEffectStartSlot(slot)) { Arm(a_vm, slot); }
					continue;
				}
				if (rate >= limit)
				{
					slot.peakRate = std::max(slot.peakRate, rate);
					if (++slot.above >= need)
					{
						Trip(a_vm, slot, a_overstressed);
						if (g_settings.guard && a_overstressed && g_guardLive.load(std::memory_order_relaxed) && IsEffectStartSlot(slot)) { Arm(a_vm, slot); }
					}
				}
				else
				{
					slot.above = 0;
					slot.peakRate = 0.0F;
					if (count == 0)
					{
						// nothing this second: the row goes back to the table
						slot.name.store(nullptr, std::memory_order_relaxed);
						slot.owner.store(0, std::memory_order_relaxed);
						slot.kind = kKindUnset;
						slot.lastRate = 0.0F;
						slot.key.store(0, std::memory_order_release);
					}
				}
			}
			g_busiestRate.store(busiest, std::memory_order_relaxed);
			LARGE_INTEGER after;
			QueryPerformanceCounter(&after);
			const auto spent = static_cast<std::uint64_t>(after.QuadPart - before.QuadPart);
			auto seen = g_scanTicksWorst.load(std::memory_order_relaxed);
			while (spent > seen && !g_scanTicksWorst.compare_exchange_weak(seen, spent, std::memory_order_relaxed)) {}
			g_scans.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void Configure(const Settings& a_settings) { g_settings = a_settings; }
	const Settings& GetSettings() { return g_settings; }
	void SetOriginalSend(SendFn a_original) { g_original = a_original; }

	void OnDataLoaded()
	{
		if (!g_settings.watch)
		{
			logger::info("Loop watch off by bLoopWatch; the guard needs it, so that is off too");
			return;
		}
		g_nameStart = RE::BSFixedString("OnEffectStart");
		g_nameFinish = RE::BSFixedString("OnEffectFinish");
		for (const auto* name : { "OnEffectStart", "OnEffectFinish", "OnLoad", "OnUnload", "OnCellAttach", "OnCellDetach", "OnAttachedToCell", "OnDetachedFromCell", "OnInit" })
		{
			g_oneShotNames.emplace_back(name);
		}
		g_startPtr.store(g_nameStart.data());
		g_finishPtr.store(g_nameFinish.data());
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton())
		{
			holder->AddEventSink<RE::TESCellAttachDetachEvent>(&g_attachSink);
			holder->AddEventSink<RE::TESObjectLoadedEvent>(&g_loadSink);
			logger::info("Loop watch: joined the cell attach and object load events");
		}
		g_guardLive.store(g_settings.guard);
		logger::info("Loop watch on: an object taking a one-shot event {:.0f}/s or an attach {:.0f}/s for {} s, any other event {:.0f}/s for {} s, {} stacks on one object, a stack {} frames deep or {} times in one function. Guard: {}",
			g_settings.oneShotRate, g_settings.attachRate, g_settings.sustainSeconds, g_settings.otherRate, g_settings.otherSustainSeconds, g_settings.stacksOnOneObject, g_settings.deepFrames, g_settings.sameFunctionFrames, g_settings.guard ? "ON (bLoopGuard=1)" : "off");
	}

	void OnGameLoaded()
	{
		if (!g_settings.watch) { return; }
		CheckHandleFormat(RE::BSScript::Internal::VirtualMachine::GetSingleton(), RE::PlayerCharacter::GetSingleton());
	}

	void OnPreLoad()
	{
		g_floodStop.store(true);
		g_floodActive.store(false);
		{
			SpinGuard guard;
			for (auto& pair : g_pairs) { pair = Pair{}; }
			for (auto& armed : g_armed) { armed.form.store(0); }
			g_armedCount.store(0);
			g_heldCount.store(0);
		}
		for (auto& slot : g_slots)
		{
			slot.name.store(nullptr, std::memory_order_relaxed);
			slot.owner.store(0, std::memory_order_relaxed);
			slot.count.store(0, std::memory_order_relaxed);
			slot.tripped = false;
			slot.armed = false;
			slot.above = 0;
			slot.calm = 0;
			slot.kind = kKindUnset;
			slot.peakRate = 0.0F;
			slot.lastRate = 0.0F;
			slot.key.store(0, std::memory_order_release);
		}
		g_detectionsNow.store(0);
		g_lastWalk.clear();
		g_busy.clear();
	}

	void OnSave()
	{
		if (g_heldCount.load() == 0) { return; }
		auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		auto held = TakeHeld(0);
		logger::info("Loop guard: a save is starting, {} held starts go out first", held.size());
		if (vm) { DeliverHeld(vm, held); }
	}

	bool OnSendEvent(VM* a_vm, Handle a_handle, const char* a_name)
	{
		if (!a_name || !g_settings.watch) { return false; }
		const bool isStart = a_name == g_startPtr.load(std::memory_order_relaxed);
		const bool isFinish = !isStart && a_name == g_finishPtr.load(std::memory_order_relaxed);
		const bool effect = IsEffectHandle(a_handle) && g_format.load(std::memory_order_relaxed) != Format::kWrong;

		Count(effect && (isStart || isFinish) ? (a_handle & kOwnerMask) : a_handle, a_name);

		if (!effect || !g_guardLive.load(std::memory_order_relaxed)) { return false; }
		if (g_armedCount.load(std::memory_order_relaxed) == 0 && g_heldCount.load(std::memory_order_relaxed) == 0) { return false; }
		if (isStart || isFinish) { return FilterPair(a_handle, isStart); }
		if (g_heldCount.load(std::memory_order_relaxed) != 0) { WakeHeld(a_vm, a_handle); }
		return false;
	}

	void Tick(VM* a_vm, bool a_overstressed, bool a_layoutOK, std::uint64_t a_now, double a_ticksPerSecond)
	{
		if (!g_settings.watch) { return; }
		g_nowTicks = a_now;
		g_ticksPerSecond = a_ticksPerSecond;
		g_layoutOK = a_layoutOK;
		g_vm = a_vm;
		if (g_lastScan == 0) { g_lastScan = a_now; }

		if (!a_layoutOK && g_guardLive.load(std::memory_order_relaxed)) { TurnGuardOff(a_vm, "the VM layout check failed"); }

		FloodRequest request;
		bool begin = false;
		{
			std::scoped_lock l(g_floodLock);
			if (g_floodPending)
			{
				g_floodPending = false;
				request = g_floodRequest;
				begin = true;
			}
		}
		if (begin) { BeginFlood(a_vm, request); }
		RunFloodFrame(a_vm);

		Scan(a_vm, a_overstressed);
	}

	Handle StackKey(Handle a_handle)
	{
		if (IsEffectHandle(a_handle) || (a_handle >> 32) == static_cast<Handle>(RE::FormType::ActorCharacter)) { return kEffectTag | (a_handle & kFormMask); }
		return a_handle;
	}

	void OnWalk(VM* a_vm, const std::unordered_map<Handle, std::uint32_t>& a_stacksByObject)
	{
		if (!g_settings.watch) { return; }
		g_lastWalk = a_stacksByObject;
		const auto limit = static_cast<std::uint32_t>(g_settings.stacksOnOneObject);
		for (const auto& [owner, count] : a_stacksByObject)
		{
			if (count < limit) { continue; }
			// Once a minute per object: it is a level, not an event.
			auto& last = g_stackReportedAt[owner];
			if (last != 0 && static_cast<double>(g_nowTicks - last) / g_ticksPerSecond < 60.0) { continue; }
			last = g_nowTicks;
			g_stackDetections.fetch_add(1, std::memory_order_relaxed);
			PushLine(std::format("LOOP stacks: {} stacks on {} at one walk | scripts: {} | action: report only", count, DescribeOwner(a_vm, owner), ScriptsText(a_vm, owner, "OnEffectStart", g_layoutOK)));
		}
	}

	void NoteDeepStack(const std::string& a_function, std::uint32_t a_frames, std::uint32_t a_sameFunction)
	{
		if (!g_deepSeen.insert(a_function).second) { return; }
		g_deepDetections.fetch_add(1, std::memory_order_relaxed);
		PushLine(std::format("LOOP recursion: a stack {} frames deep, {} of the frames read in one function, began by {} | action: report only (a stack past 1000 frames is what Recursion Monitor and Papyrus Tweaks end)", a_frames, a_sameFunction, a_function));
	}

	void StartFlood(const std::string& a_target, const std::string& a_event, float a_ratePerSecond, float a_seconds)
	{
		std::scoped_lock l(g_floodLock);
		g_floodRequest = { a_target, a_event, a_ratePerSecond, a_seconds };
		g_floodPending = true;
	}

	void StopFlood() { g_floodStop.store(true); }

	std::vector<std::string> ReportLines()
	{
		std::vector<std::string> lines;
		if (!g_settings.watch)
		{
			lines.emplace_back("Loop watch: off");
			return lines;
		}
		const auto dropped = g_droppedPeriod.exchange(0);
		const auto detections = g_detectionsPeriod.exchange(0);
		const auto overflow = g_overflowPeriod.exchange(0);
		const auto worst = g_scanTicksWorst.exchange(0);
		lines.push_back(std::format("Loop watch: {} going on now, {} began this period, {} dropped, scan worst {:.3f} ms{}",
			g_detectionsNow.load(), detections, dropped, g_ticksPerSecond > 0.0 ? static_cast<double>(worst) * 1000.0 / g_ticksPerSecond : 0.0, overflow ? std::format(", {} not counted, table full", overflow) : ""));
		std::ranges::sort(g_busy, [](const Busy& a, const Busy& b) { return a.rate > b.rate; });
		for (const auto& busy : g_busy)
		{
			lines.push_back(std::format("  busiest: {} on {}: {:.0f}/s", busy.event, DescribeOwner(g_vm, busy.owner), busy.rate));
		}
		g_busy.clear();
		return lines;
	}

	std::string StateJsonFragment()
	{
		std::string reason;
		{
			SpinGuard guard;
			reason = g_guardOffReason;
		}
		std::string floodResult;
		{
			std::scoped_lock l(g_floodLock);
			floodResult = g_floodResult;
		}
		const auto escape = [](const std::string& a_text) {
			std::string out;
			out.reserve(a_text.size() + 8);
			for (const char c : a_text)
			{
				switch (c)
				{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				default:
					if (static_cast<unsigned char>(c) >= 0x20) { out += c; }
					break;
				}
			}
			return out;
		};
		std::string lines;
		{
			std::scoped_lock l(g_textLock);
			for (const auto& line : g_lines)
			{
				if (!lines.empty()) { lines += ','; }
				lines += '"';
				lines += escape(line);
				lines += '"';
			}
		}
		const auto format = g_format.load();
		return std::format(
			R"("loop":{{"watch":{},"guard":{},"guardLive":{},"guardOff":{},"guardOffReason":"{}","handleFormat":"{}","handlesChecked":{},"goingOn":{},"detections":{},"stackDetections":{},"deepDetections":{},"busiestRatePerSecond":{:.1f},"armedActors":{},"held":{},"dropped":{},"heldStartsDelivered":{},"pairTableFull":{},"notCounted":{},"scans":{},"floodActive":{},"floodSent":{},"floodResult":"{}","lines":[{}]}})",
			g_settings.watch, g_settings.guard, g_guardLive.load(), g_guardOff.load(), escape(reason),
			format == Format::kGood ? "good" : format == Format::kWrong ? "wrong" : "not checked", g_formatChecked.load(),
			g_detectionsNow.load(), g_detectionsTotal.load(), g_stackDetections.load(), g_deepDetections.load(), g_busiestRate.load(),
			g_armedCount.load(), g_heldCount.load(), g_droppedTotal.load(), g_heldDelivered.load(), g_pairFull.load(), g_overflow.load(), g_scans.load(),
			g_floodActive.load(), g_floodSent.load(), escape(floodResult), lines);
	}
}
