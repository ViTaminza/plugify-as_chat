// as_chat — Plugify C++ port of the Pisex Metamod:Source module "[AS] Chat" (as_chat.cpp).
//
// Behaviour kept from the original:
//   * a *team* chat message that starts with '@' from a player holding "@admin/chat" is not
//     broadcast as a team message; instead one non-team chat line per eligible admin is sent
//     (connected, not a bot, in game, authenticated, IsAdmin() and has "@admin/chat"), formatted
//     with the "AdminChat" phrase of the admin system;
//   * every other message passes through untouched (the listener returns true / Continue).
//
// Porting note on the message text: the original read Metamod's raw command text and dropped its
// first character and its last one - the engine's "say"/"say_team" line carried a leading space
// and a trailing newline around the message - and then tested the resulting first character for
// the '@' trigger. s2sdk hands the listener the engine's tokenised arguments instead, so the same
// behaviour is expressed directly: the chat text is the space-joined argv[1..] (a CS2 client
// sends the whole message as one quoted argument, a console say as one token per word), the
// message must start with the '@' trigger, and the broadcast body is the text after it.
//
// Pure s2sdk, no s2guard and no pisexcompat (docs/PLUGIN_STANDARD.md §7): the guards the other
// modules get from libs/s2guard live in this file.
//   * PITFALLS B1/B2 - the map gate g_mapReady. s2sdk client functions dereference a null client
//     list before the first map and an emptied one between OnMapEnd and the next OnMapStart. The
//     map listeners are registered in OnPluginStart; readiness is latched by OnMapStart and by
//     the first OnPluginUpdate of a plugin loaded into a running map (unless an OnMapEnd was seen
//     since OnPluginStart), and dropped by OnMapEnd. No client call is made while it is shut.
//   * PITFALLS C2 - IsHumanAuthenticatedInGame() is the only door to a client: slot 0..63,
//     IsClientConnected && IsClientInGame && !IsClientSourceTV && !IsFakeClient &&
//     IsClientAuthorized. ClientName() additionally requires a live controller
//     (PlayerSlotToEntPointer != nullptr) and turns s2sdk's "<unknown>" placeholder into "".
//   * PITFALLS A7 - OnPluginEnd detaches both say listeners with RemoveCommandListener and
//     unregisters the map listeners, so `plg reload` leaves nothing behind that points into the
//     unloaded library. "say" and "say_team" belong to the engine (PITFALLS C11): only a
//     listener is ever attached, AddConsoleCommand is never called for them, and nothing is
//     removed with RemoveCommand.
//
// What pisexcompat did for this module is now explicit:
//   * AddChatListenerPre(&OnChatPre) -> s2sdk::AddCommandListener("say", &OnSayCommand, Pre) and
//     the same for "say_team"; OnSayCommand rebuilds the chat text and maps OnChatPre's allow bit
//     to Continue/Handled, like pisexcompat's ChatTrampoline did;
//   * pisex::Init()/Shutdown() -> the map gate and the listener bookkeeping above (the module has
//     no config, no phrase file and no per-frame work of its own, so OnPluginUpdate only runs the
//     map-ready latch);
//   * pisex::PrintToChat(slot, phrase, name, body) -> Format(phrase, name, body), Colorize() and
//     s2sdk::PrintToChat() below. The phrase is passed as the *format*, never as an argument of a
//     wider format, and the name/body are printf arguments, so a player name with '%' is inert.
//
// Nothing touches a client in OnPluginStart (reregistering the listeners is safe before a map);
// OnPluginStart never returns Failed - that would stop every Plugify plugin on the server.
// Without the admin_system bindings HasPermission()/IsAdmin() answer false, i.e. the relay stays
// silent instead of crashing.

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include <admin_api.hpp>

#include "admin_chat.hpp"

namespace {

// CS2 has 64 player slots; the original iterated 0..63 without asking for a maximum.
constexpr int32_t kMaxSlots = 64;

// The permission and the phrase of the original (as_chat.cpp).
constexpr const char* kAdminChatPermission = "@admin/chat";
constexpr const char* kAdminChatPhrase = "AdminChat";

// The admin_system consumer API (IAdminApi replacement). Bound in OnPluginStart and dropped in
// OnPluginEnd, so a callback the engine still holds for a moment cannot reach it (PITFALLS A7).
admin_system::AdminApi* g_pAdminApi = nullptr;

// --- PITFALLS B1/B2: the map gate ------------------------------------------------

bool g_mapReady = false;           // a map is up: s2sdk client calls are safe
bool g_mapEndedSinceStart = false; // OnMapEnd was seen since OnPluginStart
bool g_firstUpdateDone = false;    // the mid-map latch has already been decided
bool g_mapListeners = false;       // OnMapStart/OnMapEnd are registered with s2sdk

// --- the engine's chat commands (PITFALLS C11) -----------------------------------

struct SayListener {
	const char* name;
	bool attached = false;
};

// One callback serves both names; it reads args[0] to tell the team line from the public one.
const char* const kSayCommand = "say";
const char* const kSayTeamCommand = "say_team";
SayListener g_sayListeners[] = {{kSayCommand}, {kSayTeamCommand}};

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

// --- PITFALLS B1/B2: the map lifecycle callbacks ---------------------------------
// They are plain function pointers called on the game thread and must not throw; OnMapEnd only
// clears state and never queries a client.

void OnMapStartListener() {
	// From here on the clients and entities of the new map are in place.
	g_mapReady = true;
}

void OnMapEndListener() {
	// From here until the next map start every s2sdk client query can crash (PITFALLS B2).
	g_mapReady = false;
	g_mapEndedSinceStart = true;
}

// --- PITFALLS C2: the client guards ----------------------------------------------

// A map is up and the slot is in range: from here on a client query is safe.
bool CanQueryClient(int32_t iSlot) {
	return g_mapReady && iSlot >= 0 && iSlot < kMaxSlots;
}

// A live, human-addressable slot with a controller entity: connected, in game, not the SourceTV
// proxy and PlayerSlotToEntPointer != nullptr. Only such a slot may be handed to GetClientName.
bool HasController(int32_t iSlot) {
	if (!CanQueryClient(iSlot)) {
		return false;
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientInGame == nullptr ||
		__s2sdk_IsClientSourceTV == nullptr || __s2sdk_PlayerSlotToEntPointer == nullptr) {
		return false;
	}
	return s2sdk::IsClientConnected(iSlot) && s2sdk::IsClientInGame(iSlot) &&
		   !s2sdk::IsClientSourceTV(iSlot) && s2sdk::PlayerSlotToEntPointer(iSlot) != nullptr;
}

// The recipient test of the original loop, in the same order but map-gated: connected, in game,
// not the SourceTV proxy, not a bot, authenticated. Each call is checked against its binding and
// short-circuits, so a slot that is not in game never reaches IsFakeClient/IsClientAuthorized.
bool IsHumanAuthenticatedInGame(int32_t iSlot) {
	if (!CanQueryClient(iSlot)) {
		return false;
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientInGame == nullptr ||
		__s2sdk_IsClientSourceTV == nullptr || __s2sdk_IsFakeClient == nullptr ||
		__s2sdk_IsClientAuthorized == nullptr) {
		return false;
	}
	return s2sdk::IsClientConnected(iSlot) && s2sdk::IsClientInGame(iSlot) &&
		   !s2sdk::IsClientSourceTV(iSlot) && !s2sdk::IsFakeClient(iSlot) &&
		   s2sdk::IsClientAuthorized(iSlot);
}

// The player name the original printed. s2sdk answers "<unknown>" when it cannot resolve the
// client; the original printed an empty name in that case.
std::string ClientName(int32_t iSlot) {
	if (!HasController(iSlot) || __s2sdk_GetClientName == nullptr) {
		return {};
	}
	const plg::string szName = s2sdk::GetClientName(iSlot);
	std::string out(szName.data(), szName.size());
	if (out == "<unknown>") {
		out.clear();
	}
	return out;
}

// --- the output -------------------------------------------------------------------

void PrintToChat(int32_t iSlot, std::string_view text) {
	if (!HasController(iSlot) || __s2sdk_PrintToChat == nullptr) {
		return;
	}
	s2sdk::PrintToChat(iSlot, plg::string(text));
}

// pisex::PrintToChat of the original: vsnprintf(phrase, name, body) -> cs2-menus colour tags ->
// s2sdk::PrintToChat. The phrase comes from the admin_system core, so it is a runtime format;
// the name and the body are arguments (%s of the phrase), never part of the format.
void SendAdminChat(int32_t iSlot, const char* szPhrase, const std::string& szName,
				   const std::string& szBody) {
	if (szPhrase == nullptr) {
		return;
	}
	const std::string szLine = as_chat::Format(szPhrase, szName.c_str(), szBody.c_str());
	const std::string szColored = as_chat::Colorize(szLine);
	PrintToChat(iSlot, szColored);
}

// --- the chat listener --------------------------------------------------------------

// Everything the caller wrote after the command name, space-joined (the shape pisexcompat's
// ChatTrampoline built for AddChatListenerPre). The engine tokenises the line, so this only
// normalises the whitespace *between* the tokens.
std::string JoinArguments(const plg::vector<plg::string>& args, std::size_t iFirst) {
	std::string szJoined;
	for (std::size_t i = iFirst; i < args.size(); ++i) {
		if (i != iFirst) {
			szJoined.push_back(' ');
		}
		szJoined.append(args[i].data(), args[i].size());
	}
	return szJoined;
}

// The original's AddChatListenerPre callback. false = consumed: the team message must not be
// broadcast (the relay already ran). true = let the engine handle the chat line as usual.
bool OnChatPre(int32_t iSlot, std::string_view szContent, bool bTeam) {
	if (!bTeam) {
		return true; // only team chat carries the admin chat
	}
	// PITFALLS C2: the speaker must be a live authenticated human client. The server console's
	// own "say_team" (slot -1) and a stale slot pass through untouched.
	if (!IsHumanAuthenticatedInGame(iSlot)) {
		return true;
	}
	if (!as_chat::IsAdminChatTrigger(szContent)) {
		return true; // not an admin-chat message: ordinary team chat
	}
	if (g_pAdminApi == nullptr) {
		return true; // unloaded, or admin_system is not loaded at all
	}
	if (!g_pAdminApi->HasPermission(iSlot, kAdminChatPermission)) {
		return true; // the original ignored the '@' message of a player without the permission
	}

	// The original dropped the leading '@': everything after it is the admin chat body.
	const std::string szBody(as_chat::AdminChatBody(szContent));
	const std::string szName = ClientName(iSlot);
	const char* szPhrase = g_pAdminApi->GetTranslation(kAdminChatPhrase);

	for (int32_t i = 0; i < kMaxSlots; ++i) {
		if (!IsHumanAuthenticatedInGame(i)) {
			continue;
		}
		// The recipient rule of the original: IsAdmin() && HasPermission("@admin/chat").
		if (!g_pAdminApi->IsAdmin(i) || !g_pAdminApi->HasPermission(i, kAdminChatPermission)) {
			continue;
		}
		SendAdminChat(i, szPhrase, szName, szBody);
	}
	return false; // consumed: the original team message must not be sent
}

// The say/say_team pre listener (PITFALLS C11: a listener on the engine's commands, not a console
// command of this plugin). A throwing handler never swallows the player's chat line.
s2sdk::ResultType OnSayCommand(int32_t iSlot, s2sdk::ConCommandContext /*eContext*/,
							   const plg::vector<plg::string>& args) {
	try {
		const bool bTeam = !args.empty() && args[0] == kSayTeamCommand;
		const std::string szContent = JoinArguments(args, 1);
		if (!OnChatPre(iSlot, szContent, bTeam)) {
			return s2sdk::ResultType::Handled;
		}
	} catch (const std::exception& ex) {
		LogError(std::string("as_chat: the chat listener failed: ") + ex.what());
	} catch (...) {
		LogError("as_chat: the chat listener failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

void AttachSayListeners() {
	if (__s2sdk_AddCommandListener == nullptr) {
		LogError("as_chat: s2sdk AddCommandListener is unavailable - the admin chat stays disabled");
		return;
	}
	for (SayListener& say : g_sayListeners) {
		if (say.attached) {
			continue;
		}
		say.attached =
			s2sdk::AddCommandListener(plg::string(say.name), &OnSayCommand, s2sdk::HookMode::Pre);
		if (!say.attached) {
			LogError(std::string("as_chat: could not listen on the engine's '") + say.name + "'");
		}
	}
}

class AsChatPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			g_pAdminApi = &admin_system::AdminApi::Get();
			g_mapReady = false;
			g_mapEndedSinceStart = false;
			g_firstUpdateDone = false;

			// PITFALLS B1: registering the map listeners touches no client, so it is safe here.
			// Without the listeners nothing could ever close the gate again, so a missing binding
			// keeps it shut (every client call stays disabled) instead of opening it blindly.
			if (!g_mapListeners && __s2sdk_OnMapStart_Register && __s2sdk_OnMapEnd_Register) {
				s2sdk::OnMapStart_Register(&OnMapStartListener);
				s2sdk::OnMapEnd_Register(&OnMapEndListener);
				g_mapListeners = true;
			} else if (!__s2sdk_OnMapStart_Register || !__s2sdk_OnMapEnd_Register) {
				LogError("as_chat: the s2sdk map listeners are unavailable - client calls stay disabled");
			}

			// PITFALLS C11: "say" and "say_team" are the engine's commands, so the chat hook is a
			// listener only. AddConsoleCommand would create a shadow command when the engine's
			// one is not registered yet, and RemoveCommand would unregister somebody else's.
			AttachSayListeners();

			LogInfo("as_chat: ready (admins chat: '@' in team chat)");
		} catch (const std::exception& ex) {
			LogError(std::string("as_chat: OnPluginStart failed: ") + ex.what());
		} catch (...) {
			LogError("as_chat: OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed (PITFALLS A1)
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds) override {
		// A plugin loaded into a running map never saw OnMapStart: its first update is the "a map
		// is up" signal (PITFALLS B1). An OnMapEnd seen since OnPluginStart means the server is
		// between maps, so the latch waits for the next OnMapStart instead. There is no other
		// per-frame work: the relay is driven entirely by the chat listener.
		if (!g_firstUpdateDone) {
			g_firstUpdateDone = true;
			if (g_mapListeners && !g_mapEndedSinceStart) {
				g_mapReady = true;
			}
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			// PITFALLS A7: both engine listeners go away before the map callbacks and the admin
			// API, so a stale say callback can no longer reach a client or the admin system.
			for (SayListener& say : g_sayListeners) {
				if (!say.attached) {
					continue;
				}
				if (__s2sdk_RemoveCommandListener != nullptr) {
					s2sdk::RemoveCommandListener(plg::string(say.name), &OnSayCommand,
												 s2sdk::HookMode::Pre);
				}
				say.attached = false;
			}

			if (g_mapListeners) {
				if (__s2sdk_OnMapStart_Unregister) {
					s2sdk::OnMapStart_Unregister(&OnMapStartListener);
				}
				if (__s2sdk_OnMapEnd_Unregister) {
					s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
				}
				g_mapListeners = false;
			}

			g_mapReady = false;
			g_mapEndedSinceStart = true;
			g_firstUpdateDone = false;
			g_pAdminApi = nullptr;
		} catch (const std::exception& ex) {
			LogError(std::string("as_chat: OnPluginEnd failed: ") + ex.what());
		} catch (...) {
			LogError("as_chat: OnPluginEnd failed with an unknown exception");
		}
		return {};
	}
};

AsChatPlugin g_asChatPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_asChatPlugin)
