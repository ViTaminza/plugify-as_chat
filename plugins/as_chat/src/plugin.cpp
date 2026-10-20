// as_chat — Plugify C++ port of the Pisex Metamod:Source module "[AS] Chat" (as_chat.cpp).
//
// Behaviour kept from the original:
//   * a *team* chat message that starts with '@' from a player holding "@admin/chat" is not
//     broadcast as a team message; instead one non-team chat line per eligible admin is sent
//     (connected, not a bot, in game, authenticated, IsAdmin() and has "@admin/chat"), formatted
//     with the "AdminChat" phrase of the admin system;
//   * every other message passes through untouched (the listener returns true).
//
// Porting note on the message text: the original read Metamod's raw command text and dropped its
// first character and its last one - the engine's "say" line carried a leading space and a
// trailing newline around the message. pisexcompat hands the parsed chat text itself, so the same
// behaviour is expressed directly: the message must start with the '@' trigger and the broadcast
// body is the text after it.
//
// No Metamod, no SchemaEntity, no SourceHook: the whole module is one chat listener plus the
// admin system API. Nothing is registered against the engine in OnPluginStart beyond the chat
// listener (safe before a map), and OnPluginStart never returns Failed - that would stop every
// Plugify plugin on the server. Without the admin_system bindings every AdminApi call degrades to
// "no permission", i.e. the relay stays silent instead of crashing.

#include <chrono>
#include <exception>
#include <string>
#include <string_view>

#include <plg/plugin.hpp>
#include <plugin_export.h>

#include <pisex/pisex.hpp>

#include <admin_api.hpp>

namespace {

constexpr int kMaxSlots = 64;

admin_system::AdminApi* g_pAdminApi = nullptr;

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

// The original's AddChatListenerPre callback.
bool OnChatPre(int iSlot, const char* szContent, bool bTeam) {
	if (!bTeam || !szContent || szContent[0] != '@') {
		return true; // not a team admin-chat message: let the engine broadcast it
	}
	if (!g_pAdminApi->HasPermission(iSlot, "@admin/chat")) {
		return true; // the original ignored the '@' message of a player without the permission
	}

	// The original dropped the command text's leading character (after trimming the engine's
	// trailing newline): everything after the '@' trigger is the admin chat body.
	const std::string body(std::string_view(szContent).substr(1));
	const char* szName = pisex::g_pPlayers->GetPlayerName(iSlot);

	for (int i = 0; i < kMaxSlots; ++i) {
		if (pisex::g_pPlayers->IsFakeClient(i)) continue;
		if (!pisex::g_pPlayers->IsConnected(i)) continue;
		if (!pisex::g_pPlayers->IsInGame(i)) continue;
		if (!pisex::g_pPlayers->IsAuthenticated(i)) continue;
		if (g_pAdminApi->IsAdmin(i) && g_pAdminApi->HasPermission(i, "@admin/chat")) {
			pisex::g_pUtils->PrintToChat(i, g_pAdminApi->GetTranslation("AdminChat"), szName, body.c_str());
		}
	}
	return false; // consumed: the original team message must not be sent
}

class AsChatPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			// The admin system family keeps its files in <plugify configs>/admin_system (fixed
			// layout); this also wires the s2sdk listeners of pisexcompat (map start/end, say).
			pisex::Init("admin_system");
			g_pAdminApi = &admin_system::AdminApi::Get();

			pisex::g_pUtils->AddChatListenerPre(&OnChatPre);

			LogMessage("as_chat: ready (admins chat: '@' in team chat)", plg::Severity::Info);
		} catch (const std::exception& ex) {
			LogError(std::string("as_chat: OnPluginStart failed: ") + ex.what());
		} catch (...) {
			LogError("as_chat: OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds) override {
		// Pumps the shared per-frame work of pisexcompat (map readiness, menu requests, ...).
		try {
			pisex::Poll();
		} catch (...) {
			LogError("as_chat: Poll() threw");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			// Drops the chat listener and the s2sdk listeners pisexcompat registered.
			pisex::Shutdown();
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
