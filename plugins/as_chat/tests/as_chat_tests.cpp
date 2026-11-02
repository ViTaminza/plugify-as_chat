// as_chat tests — no engine: the plugin TU is compiled in (its handlers live in an anonymous
// namespace) and every __s2sdk_* / __admin_system_* binding slot is a recording fake.
//
// Covered:
//   * src/admin_chat.hpp — every cs2-menus colour tag byte-for-byte, the "@" trigger and the
//     body, and the printf formatting of the "AdminChat" phrase (arguments, '%' in a name, a
//     missing translation that falls back to the key, no 512-byte truncation);
//   * PITFALLS B1/B2 — the map gate: not one s2sdk client call before the first map, between
//     OnMapEnd and the next OnMapStart, or when the plugin was loaded into the map-change window
//     (an OnMapEnd seen before the first update must not latch readiness); the first
//     OnPluginUpdate of a plugin loaded into a running map does latch it;
//   * PITFALLS C2 — the slot range, the connected/in-game/SourceTV/fake/authorized checks in
//     that order, the controller gate of ClientName() and s2sdk's "<unknown>" -> "";
//   * the relay: only team chat, only a line that starts with '@' from a player with
//     "@admin/chat"; the recipients are the non-bot, connected, in-game, authenticated admins
//     with "@admin/chat"; the "AdminChat" phrase is printf-formatted with the speaker name and
//     the body and sent colourised; the sender receives the relay as well, like the original;
//   * PITFALLS C11 — "say"/"say_team" get a listener only, never a console command;
//   * PITFALLS A7 — both say listeners and the map listeners are removed in OnPluginEnd, the
//     admin API is dropped, a stale callback is harmless and `plg reload` re-registers once;
//   * a start without the s2sdk bindings or without the admin core stays soft (PITFALLS A1).
//
// Run by ./test.sh (build/tests/as_chat_tests).

#include "../src/plugin.cpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

std::vector<std::string> g_logLines;

void FakeLog(std::string_view message, plg::Severity /*severity*/,
			 const plg::source_location& /*location*/) {
	g_logLines.emplace_back(message);
}

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

bool Logged(const std::string& needle) {
	for (const std::string& line : g_logLines) {
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

plg::vector<plg::string> Args(std::initializer_list<const char*> words) {
	plg::vector<plg::string> args;
	for (const char* word : words) {
		args.emplace_back(word);
	}
	return args;
}

// The production "AdminChat" phrase of the admin_system core (admin_system.phrases.txt, "ru").
constexpr const char* kAdminChatPhraseText = " {GREEN}[Админ-чат]{DEFAULT} %s: %s";

// What the original's pisex::PrintToChat sent: {GREEN} -> 0x04, {DEFAULT} -> 0x01.
std::string ExpectedAdminChat(const std::string& szName, const std::string& szBody) {
	return std::string(" \x04[Админ-чат]\x01 ") + szName + ": " + szBody;
}

// --- the fake map lifecycle ----------------------------------------------------------------

s2sdk::OnMapStartCallback g_onMapStart = nullptr;
s2sdk::OnMapEndCallback g_onMapEnd = nullptr;
int g_mapStartRegisters = 0;
int g_mapStartUnregisters = 0;
int g_mapEndRegisters = 0;
int g_mapEndUnregisters = 0;

void FakeMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_onMapStart = callback;
	++g_mapStartRegisters;
}

void FakeMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_onMapStart == callback) {
		g_onMapStart = nullptr;
	}
	++g_mapStartUnregisters;
}

void FakeMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_onMapEnd = callback;
	++g_mapEndRegisters;
}

void FakeMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_onMapEnd == callback) {
		g_onMapEnd = nullptr;
	}
	++g_mapEndUnregisters;
}

void MapStart() {
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	}
}

void MapEnd() {
	if (g_onMapEnd != nullptr) {
		g_onMapEnd();
	}
}

// --- the fake client layer -----------------------------------------------------------------

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool fake = false;
	bool authorized = false;
	bool controller = false; // PlayerSlotToEntPointer() != nullptr
	std::string name;
};

FakeClient LiveClient(const std::string& name) {
	FakeClient client;
	client.connected = true;
	client.inGame = true;
	client.authorized = true;
	client.controller = true;
	client.name = name;
	return client;
}

struct FakePrint {
	int32_t slot = 0;
	std::string text;
};

std::vector<FakeClient> g_clients(64);
int g_clientCalls = 0; // every fake below that is a client query/output counts into this
int g_connectedCalls = 0;
int g_inGameCalls = 0;
int g_sourceTvCalls = 0;
int g_fakeCalls = 0;
int g_authorizedCalls = 0;
int g_nameCalls = 0;
int g_controllerCalls = 0;
std::vector<FakePrint> g_chatPrints;

int g_controllerSentinel = 0; // a stable non-null address for "the controller exists"

const FakeClient& Slot(int32_t slot) {
	static const FakeClient empty;
	return slot >= 0 && slot < static_cast<int32_t>(g_clients.size())
			   ? g_clients[static_cast<std::size_t>(slot)]
			   : empty;
}

void ResetClientCounters() {
	g_clientCalls = 0;
	g_connectedCalls = 0;
	g_inGameCalls = 0;
	g_sourceTvCalls = 0;
	g_fakeCalls = 0;
	g_authorizedCalls = 0;
	g_nameCalls = 0;
	g_controllerCalls = 0;
}

bool FakeIsClientConnected(int32_t slot) {
	++g_clientCalls;
	++g_connectedCalls;
	return Slot(slot).connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++g_clientCalls;
	++g_inGameCalls;
	return Slot(slot).inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_clientCalls;
	++g_sourceTvCalls;
	return Slot(slot).sourceTV;
}

bool FakeIsFakeClient(int32_t slot) {
	++g_clientCalls;
	++g_fakeCalls;
	return Slot(slot).fake;
}

bool FakeIsClientAuthorized(int32_t slot) {
	++g_clientCalls;
	++g_authorizedCalls;
	return Slot(slot).authorized;
}

plg::string FakeGetClientName(int32_t slot) {
	++g_clientCalls;
	++g_nameCalls;
	// s2sdk answers "<unknown>" when it cannot resolve the client name.
	return plg::string(Slot(slot).name.empty() ? "<unknown>" : Slot(slot).name);
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	++g_clientCalls;
	++g_controllerCalls;
	return Slot(slot).controller ? static_cast<void*>(&g_controllerSentinel) : nullptr;
}

void FakePrintToChat(int32_t slot, const plg::string& text) {
	++g_clientCalls;
	g_chatPrints.push_back(FakePrint{slot, ToStd(text)});
}

// --- the fake command manager (listeners only; as_chat registers no console command) --------

struct FakeListener {
	std::string name;
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};

struct FakeCommand {
	std::string name;
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};

std::vector<FakeListener> g_addedListeners;
std::vector<FakeListener> g_removedListeners;
std::vector<FakeCommand> g_consoleCommands;
std::vector<std::string> g_removedCommands;

bool FakeAddConsoleCommand(const plg::string& name, const plg::string& /*description*/,
						   s2sdk::ConVarFlag /*flags*/, s2sdk::ConCommandCallback callback,
						   s2sdk::HookMode mode) {
	g_consoleCommands.push_back(FakeCommand{std::string(name.data(), name.size()), callback, mode});
	return true;
}

bool FakeRemoveCommand(const plg::string& name, s2sdk::ConCommandCallback /*callback*/) {
	g_removedCommands.emplace_back(name.data(), name.size());
	return true;
}

bool FakeAddCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							s2sdk::HookMode mode) {
	g_addedListeners.push_back(FakeListener{std::string(name.data(), name.size()), callback, mode});
	return true;
}

bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							   s2sdk::HookMode mode) {
	g_removedListeners.push_back(FakeListener{std::string(name.data(), name.size()), callback, mode});
	return true;
}

const FakeListener* FindListener(const std::string& name) {
	for (const FakeListener& listener : g_addedListeners) {
		if (listener.name == name) {
			return &listener;
		}
	}
	return nullptr;
}

int CountListeners(const std::string& name) {
	int count = 0;
	for (const FakeListener& listener : g_addedListeners) {
		count += listener.name == name ? 1 : 0;
	}
	return count;
}

bool RemovedListener(const std::string& name, s2sdk::ConCommandCallback callback,
					 s2sdk::HookMode mode) {
	for (const FakeListener& listener : g_removedListeners) {
		if (listener.name == name && listener.callback == callback && listener.mode == mode) {
			return true;
		}
	}
	return false;
}

s2sdk::ResultType CallCommand(s2sdk::ConCommandCallback callback, int32_t caller,
							  s2sdk::ConCommandContext context,
							  std::initializer_list<const char*> words) {
	const plg::vector<plg::string> args = Args(words);
	return callback(caller, context, args);
}

s2sdk::ResultType DispatchListener(const std::string& name, int32_t caller,
								   s2sdk::ConCommandContext context,
								   std::initializer_list<const char*> words) {
	const FakeListener* listener = FindListener(name);
	Check(listener != nullptr, "listener \"" + name + "\" is installed");
	if (listener == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	return CallCommand(listener->callback, caller, context, words);
}

// --- the fake admin_system bindings ---------------------------------------------------------

std::vector<std::pair<int32_t, std::string>> g_permissionChecks;
std::set<int32_t> g_permittedSlots;
std::set<int32_t> g_adminSlots;
bool g_translationUnknown = false;

float FakeGetPluginVersion() {
	return 1.0f;
}

plg::string FakeGetTranslation(const plg::string& key) {
	const std::string szKey = ToStd(key);
	if (!g_translationUnknown && szKey == kAdminChatPhrase) {
		return plg::string(kAdminChatPhraseText);
	}
	// The core returns the key itself for an unknown phrase, like the original.
	return plg::string(szKey);
}

bool FakeHasPermission(int32_t slot, const plg::string& permission) {
	g_permissionChecks.emplace_back(slot, ToStd(permission));
	return g_permittedSlots.count(slot) != 0;
}

bool FakeIsAdmin(int32_t slot) {
	return g_adminSlots.count(slot) != 0;
}

// --- the recording helpers -------------------------------------------------------------------

bool PermissionChecked(int32_t slot) {
	for (const auto& [checkedSlot, permission] : g_permissionChecks) {
		(void)permission;
		if (checkedSlot == slot) {
			return true;
		}
	}
	return false;
}

bool AllPermissionChecksAreAdminChat() {
	for (const auto& [slot, permission] : g_permissionChecks) {
		(void)slot;
		if (permission != kAdminChatPermission) {
			return false;
		}
	}
	return !g_permissionChecks.empty();
}

std::set<int32_t> PrintedSlots() {
	std::set<int32_t> slots;
	for (const FakePrint& print : g_chatPrints) {
		slots.insert(print.slot);
	}
	return slots;
}

bool AllChatPrintsAre(const std::string& expected) {
	for (const FakePrint& print : g_chatPrints) {
		if (print.text != expected) {
			return false;
		}
	}
	return !g_chatPrints.empty();
}

void InstallFakes() {
	plg::Log = &FakeLog;

	__s2sdk_OnMapStart_Register = &FakeMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeMapEndUnregister;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_IsClientAuthorized = &FakeIsClientAuthorized;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PrintToChat = &FakePrintToChat;

	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_AddCommandListener = &FakeAddCommandListener;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;

	__admin_system_GetPluginVersion = &FakeGetPluginVersion;
	__admin_system_GetTranslation = &FakeGetTranslation;
	__admin_system_HasPermission = &FakeHasPermission;
	__admin_system_IsAdmin = &FakeIsAdmin;
}

// Forgets every fake registration and all recorded state. The plugin must have been stopped first
// (PrepareTest/FinishTest do that), otherwise it still believes its listeners are registered.
void ResetWorld() {
	InstallFakes(); // a test may have removed a binding to check the fail-soft path
	g_clients.assign(64, FakeClient{});
	g_permittedSlots.clear();
	g_adminSlots.clear();
	g_permissionChecks.clear();
	g_translationUnknown = false;
	admin_system::AdminApi::Get().ReloadTranslations(); // the phrase cache of admin_api.hpp
	ResetClientCounters();
	g_chatPrints.clear();
	g_logLines.clear();

	g_addedListeners.clear();
	g_removedListeners.clear();
	g_consoleCommands.clear();
	g_removedCommands.clear();

	g_onMapStart = nullptr;
	g_onMapEnd = nullptr;
	g_mapStartRegisters = 0;
	g_mapStartUnregisters = 0;
	g_mapEndRegisters = 0;
	g_mapEndUnregisters = 0;
}

// Stops whatever a previous test left behind (OnPluginEnd is a no-op when nothing is registered)
// and starts the next test from an empty fake world.
void PrepareTest() {
	g_asChatPlugin.OnPluginEnd();
	ResetWorld();
}

void FinishTest() {
	g_asChatPlugin.OnPluginEnd();
	ResetWorld();
}

void StartWithMap() {
	Check(g_asChatPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok,
		  "OnPluginStart returns Ok (never Failed)");
	MapStart();
	Check(g_mapReady, "OnMapStart opens the map gate");
}

// --- A. the chat text helpers ---------------------------------------------------------------

void TestChatTextHelpers() {
	PrepareTest();

	// Every tag of the cs2-menus table becomes its control bytes.
	for (const as_chat::detail::ColorTag& color : as_chat::detail::kColorTags) {
		Check(as_chat::Colorize(color.tag) == std::string(color.bytes),
			  "colour tag " + std::string(color.tag) + " becomes its control bytes");
	}
	Check(as_chat::Colorize(kAdminChatPhraseText) == " \x04[Админ-чат]\x01 %s: %s",
		  "the production AdminChat phrase is colourised like cs2-menus/pisexcompat did");
	Check(as_chat::Colorize("no tags here") == "no tags here", "a tagless text is unchanged");
	Check(as_chat::Colorize("{PINK}x{DEFAULT}") == "{PINK}x\x01",
		  "an unknown {TAG} stays literal, like the cs2-menus formatter");
	Check(as_chat::Colorize("{RED}{RED}") == "\x02\x02", "every occurrence of a tag is replaced");
	Check(as_chat::Colorize("a\\nb") == "a\xe2\x80\xa9"
										"b",
		  "the phrase-file \"\\n\" becomes the chat line separator U+2029");
	Check(as_chat::Colorize("").empty(), "an empty text stays empty");

	// The printf formatting the original's pisex::PrintToChat did.
	Check(as_chat::Format("%s: %s", "Alice", "hi") == "Alice: hi", "Format substitutes both %s");
	Check(as_chat::Format(" %s", "") == " ", "Format accepts an empty argument");
	Check(as_chat::Format("100%% %s", "x") == "100% x", "Format expands %%");
	Check(as_chat::Format("%s", "100%s") == "100%s",
		  "a '%' inside an argument is not a format specifier");
	Check(as_chat::Format("AdminChat") == "AdminChat",
		  "a missing translation (the key, no placeholders) prints unchanged");
	// The phrase is a runtime string, so the extra arguments must not be a compile-time error.
	const char* szMissingPhrase = "AdminChat";
	Check(as_chat::Format(szMissingPhrase, "Alice", "hi") == "AdminChat",
		  "extra arguments are ignored, exactly like vsnprintf");
	const char* szNullFormat = nullptr;
	Check(as_chat::Format(szNullFormat, "unused").empty(), "a null format prints nothing");
	const char* szEmptyFormat = "";
	Check(as_chat::Format(szEmptyFormat, "unused").empty(), "an empty format prints nothing");

	const std::string szLongText(2000, 'x');
	Check(as_chat::Format("%s", szLongText.c_str()) == szLongText,
		  "the formatting is not capped at the original's 512-byte buffer");

	// The '@' trigger and the body.
	Check(as_chat::IsAdminChatTrigger("@"), "a bare '@' is the admin-chat trigger");
	Check(as_chat::IsAdminChatTrigger("@hello"), "'@hello' is an admin-chat message");
	Check(!as_chat::IsAdminChatTrigger(""), "an empty chat line is not an admin-chat message");
	Check(!as_chat::IsAdminChatTrigger("hello @x"), "an '@' in the middle is not the trigger");
	Check(!as_chat::IsAdminChatTrigger(" @x"),
		  "a leading space is not the trigger (the parsed chat text has none)");
	Check(as_chat::AdminChatBody("@hello world") == "hello world",
		  "the body is everything after the trigger");
	Check(as_chat::AdminChatBody("@").empty(), "a bare '@' has an empty body");
	Check(as_chat::AdminChatBody("hello").empty(), "a non-trigger line has no body");

	FinishTest();
}

// --- B. the map gate (PITFALLS B1/B2) -------------------------------------------------------

void TestMapGate() {
	PrepareTest();
	Check(g_asChatPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok,
		  "OnPluginStart returns Ok (never Failed)");
	Check(g_mapStartRegisters == 1 && g_mapEndRegisters == 1,
		  "OnPluginStart registers both map listeners");
	Check(!g_mapReady, "the gate is shut on plugin start");
	Check(g_consoleCommands.empty(), "no console command is registered at start");

	// The chat listeners are listeners of the engine's commands, not console commands (C11).
	Check(CountListeners("say") == 1 && CountListeners("say_team") == 1,
		  "OnPluginStart attaches the say and say_team listeners");
	const FakeListener* say = FindListener("say");
	const FakeListener* sayTeam = FindListener("say_team");
	Check(say != nullptr && say->mode == s2sdk::HookMode::Pre,
		  "the say listener is a Pre listener");
	Check(sayTeam != nullptr && sayTeam->mode == s2sdk::HookMode::Pre,
		  "the say_team listener is a Pre listener");
	Check(say != nullptr && say->callback == &OnSayCommand &&
			  sayTeam != nullptr && sayTeam->callback == &OnSayCommand,
		  "both chat listeners run OnSayCommand");
	Check(g_consoleCommands.empty(), "say/say_team are never registered with AddConsoleCommand");

	g_clients[3] = LiveClient("Alice");
	g_permittedSlots = {3};
	g_adminSlots = {3};

	// --- before the first map: not one client call may reach s2sdk ---------------------------
	ResetClientCounters();
	Check(DispatchListener("say_team", 3, s2sdk::ConCommandContext::Console,
						   {"say_team", "@hello"}) == s2sdk::ResultType::Continue,
		  "before the gate opens the chat line continues to the engine");
	Check(g_clientCalls == 0, "not one s2sdk client call is made before the gate opens");
	Check(g_permissionChecks.empty(), "no admin permission check before the gate opens");
	Check(g_chatPrints.empty(), "nothing is relayed before the gate opens");

	// A plugin loaded into a running map: its first update latches readiness.
	Check(!g_mapReady, "the map start listener was not called yet");
	g_asChatPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	Check(g_mapReady, "the first OnPluginUpdate latches the gate");
	ResetClientCounters();
	Check(DispatchListener("say_team", 3, s2sdk::ConCommandContext::Console,
						   {"say_team", "@hello"}) == s2sdk::ResultType::Handled,
		  "a permitted '@' team line is handled after the latch");
	Check(g_chatPrints.size() == 1 && g_chatPrints[0].slot == 3,
		  "the admin receives the relay after the latch");
	Check(g_chatPrints[0].text == ExpectedAdminChat("Alice", "hello"),
		  "the relay is the colourised AdminChat phrase");
	Check(g_clientCalls > 0, "the relay queried the clients after the latch");

	// --- after OnMapEnd: not one client call may reach s2sdk ---------------------------------
	MapEnd();
	Check(!g_mapReady, "OnMapEnd drops readiness");
	g_asChatPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	Check(!g_mapReady, "an update after OnMapEnd does not re-open the gate");
	ResetClientCounters();
	g_chatPrints.clear();
	Check(DispatchListener("say_team", 3, s2sdk::ConCommandContext::Console,
						   {"say_team", "@hello"}) == s2sdk::ResultType::Continue,
		  "the chat line continues in the map-change window");
	Check(g_clientCalls == 0, "not one s2sdk client call is made after OnMapEnd");
	Check(g_chatPrints.empty(), "nothing is relayed after OnMapEnd");

	// --- the next OnMapStart opens the gate again without waiting for an update ---------------
	MapStart();
	Check(g_mapReady, "OnMapStart latches the gate");
	Check(DispatchListener("say_team", 3, s2sdk::ConCommandContext::Console,
						   {"say_team", "@hello"}) == s2sdk::ResultType::Handled,
		  "the relay works again on the next map");

	FinishTest();
}

// A plugin loaded during the map change (OnMapEnd before its first update) must not latch on the
// update: the first update only counts when no OnMapEnd was seen since OnPluginStart.
void TestMapChangeWindowStart() {
	PrepareTest();
	g_asChatPlugin.OnPluginStart();
	MapEnd();
	g_asChatPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	Check(!g_mapReady, "the first update after an OnMapEnd does not latch the gate");

	g_clients[2] = LiveClient("Bob");
	g_permittedSlots = {2};
	g_adminSlots = {2};

	ResetClientCounters();
	Check(DispatchListener("say_team", 2, s2sdk::ConCommandContext::Console,
						   {"say_team", "@hello"}) == s2sdk::ResultType::Continue,
		  "no relay in the map-change window");
	Check(g_clientCalls == 0, "no client call in the map-change window");

	MapStart();
	Check(g_mapReady, "the next OnMapStart opens the gate");
	Check(DispatchListener("say_team", 2, s2sdk::ConCommandContext::Console,
						   {"say_team", "@hello"}) == s2sdk::ResultType::Handled,
		  "the relay works after the map start");

	FinishTest();
}

// --- C. the client guards (PITFALLS C2) -----------------------------------------------------

void TestClientGuards() {
	PrepareTest();
	StartWithMap();

	// --- out-of-range slots never reach s2sdk ------------------------------------------------
	ResetClientCounters();
	Check(!CanQueryClient(-1) && !CanQueryClient(64), "slots -1 and 64 cannot be queried");
	Check(!IsHumanAuthenticatedInGame(-1) && !IsHumanAuthenticatedInGame(64),
		  "out-of-range slots are not eligible recipients");
	Check(!HasController(-1) && !HasController(64), "out-of-range slots have no controller");
	Check(ClientName(-1).empty() && ClientName(64).empty(), "out-of-range slots have no name");
	PrintToChat(64, "x");
	Check(g_clientCalls == 0, "slots -1 and 64 never reach a client binding");
	Check(g_chatPrints.empty(), "slots -1 and 64 get no chat output");
	ResetClientCounters();

	g_clients[7].connected = true; // connected, not in game
	g_clients[8] = LiveClient("SourceTV");
	g_clients[8].sourceTV = true;
	g_clients[9] = LiveClient("BOT Rick");
	g_clients[9].fake = true;
	g_clients[10] = LiveClient("Eve");
	g_clients[10].authorized = false;
	g_clients[10].controller = false; // authenticated but no controller entity at all
	g_clients[11] = LiveClient(""); // s2sdk answers "<unknown>" for an empty name
	g_clients[12] = LiveClient("Carol");

	// --- each gate short-circuits before the next (more expensive) query ----------------------
	Check(!IsHumanAuthenticatedInGame(7), "a connected slot that is not in game is not eligible");
	Check(g_fakeCalls == 0 && g_authorizedCalls == 0,
		  "not-in-game short-circuits before IsFakeClient/IsClientAuthorized");
	ResetClientCounters();
	Check(!IsHumanAuthenticatedInGame(8), "the SourceTV proxy is not eligible");
	Check(g_fakeCalls == 0, "SourceTV short-circuits before IsFakeClient");
	Check(!IsHumanAuthenticatedInGame(9), "a bot is not a human client");
	Check(g_authorizedCalls == 0, "a bot short-circuits before IsClientAuthorized");
	Check(!IsHumanAuthenticatedInGame(10), "an unauthenticated client is not eligible");
	Check(IsHumanAuthenticatedInGame(11), "an authenticated in-game human is eligible");
	Check(IsHumanAuthenticatedInGame(12), "an authenticated in-game human is eligible");

	// --- ClientName needs a live controller and turns "<unknown>" into "" --------------------
	ResetClientCounters();
	Check(!HasController(10), "a slot without a controller entity has no controller");
	Check(ClientName(10).empty(), "no name without a controller");
	Check(g_nameCalls == 0, "GetClientName is not called without a controller");
	Check(ClientName(11).empty(), "s2sdk's \"<unknown>\" placeholder becomes an empty name");
	Check(ClientName(12) == "Carol", "a real client name is read");
	Check(g_controllerCalls > 0, "the controller pointer is checked before GetClientName");

	// --- output -------------------------------------------------------------------------------
	ResetClientCounters();
	g_chatPrints.clear();
	PrintToChat(10, "x"); // no controller
	Check(g_chatPrints.empty(), "a slot without a controller gets no chat line");
	PrintToChat(12, "y");
	Check(g_chatPrints.size() == 1 && g_chatPrints[0].slot == 12 && g_chatPrints[0].text == "y",
		  "a chat line reaches a live client unchanged");

	MapEnd();
	ResetClientCounters();
	g_chatPrints.clear();
	PrintToChat(12, "z");
	Check(g_clientCalls == 0 && g_chatPrints.empty(),
		  "PrintToChat makes no client call while the gate is shut");

	FinishTest();
}

// --- D. the say/say_team relay --------------------------------------------------------------

void TestSayRelay() {
	PrepareTest();
	StartWithMap();

	g_clients[3] = LiveClient("Alice");   // the permitted admin speaker
	g_clients[4] = LiveClient("Bob");     // admin + permission
	g_clients[5] = LiveClient("Carol");   // admin, no permission
	g_clients[6] = LiveClient("Dave");    // permission, not an admin
	g_clients[7] = LiveClient("BOT Rick");
	g_clients[7].fake = true;             // bot, admin + permission
	g_clients[8] = LiveClient("SourceTV");
	g_clients[8].sourceTV = true;         // proxy, admin + permission
	g_clients[9] = LiveClient("Eve");
	g_clients[9].authorized = false;      // not authenticated, admin + permission
	g_clients[10] = LiveClient("Frank");
	g_clients[10].inGame = false;         // connected, not in game
	g_clients[11] = LiveClient("");       // admin + permission, name unresolved
	g_adminSlots = {3, 4, 5, 7, 8, 9, 10, 11};
	g_permittedSlots = {3, 4, 6, 7, 8, 9, 10, 11};

	// --- a public "say" line is never the admin chat ------------------------------------------
	ResetClientCounters();
	g_permissionChecks.clear();
	Check(DispatchListener("say", 3, s2sdk::ConCommandContext::Console, {"say", "@hello"}) ==
			  s2sdk::ResultType::Continue,
		  "a public say line continues to the engine");
	Check(g_permissionChecks.empty() && g_chatPrints.empty(),
		  "a public say line is not relayed and not permission-checked");

	// --- a team line that does not start with '@' is ordinary team chat -----------------------
	ResetClientCounters();
	Check(DispatchListener("say_team", 3, s2sdk::ConCommandContext::Console,
						   {"say_team", "hello"}) == s2sdk::ResultType::Continue,
		  "an ordinary team line continues to the engine");
	Check(g_permissionChecks.empty() && g_chatPrints.empty(),
		  "an ordinary team line is not relayed and not permission-checked");

	// --- the admin chat of a permitted admin --------------------------------------------------
	ResetClientCounters();
	g_permissionChecks.clear();
	Check(DispatchListener("say_team", 3, s2sdk::ConCommandContext::Console,
						   {"say_team", "@hello"}) == s2sdk::ResultType::Handled,
		  "the admin-chat team line is consumed");
	Check(g_chatPrints.size() == 3, "one chat line per eligible admin");
	Check((PrintedSlots() == std::set<int32_t>{3, 4, 11}),
		  "only the admins with @admin/chat receive the relay (speaker included)");
	Check(AllChatPrintsAre(ExpectedAdminChat("Alice", "hello")),
		  "the relay is ' AdminChat phrase: Alice: hello', colourised");
	Check(AllPermissionChecksAreAdminChat(),
		  "every permission check asks for @admin/chat");
	Check(!g_permissionChecks.empty() && g_permissionChecks.front().first == 3,
		  "the speaker is checked for @admin/chat first");
	Check(PermissionChecked(5), "the admin without the permission was checked");
	Check(!PermissionChecked(6), "a non-admin is rejected by IsAdmin before HasPermission");
	Check(!PermissionChecked(7) && !PermissionChecked(8) && !PermissionChecked(9) &&
			  !PermissionChecked(10),
		  "bots, the SourceTV proxy and unusable slots are rejected before HasPermission");

	// --- the message body is everything after the '@' -----------------------------------------
	ResetClientCounters();
	g_chatPrints.clear();
	Check(DispatchListener("say_team", 3, s2sdk::ConCommandContext::Console,
						   {"say_team", "@"}) == s2sdk::ResultType::Handled,
		  "a bare '@' admin chat is consumed");
	Check(AllChatPrintsAre(ExpectedAdminChat("Alice", "")), "a bare '@' has an empty body");

	// A CS2 client sends the whole message as one argument, a console say sends one token per
	// word: both have to produce the same body.
	ResetClientCounters();
	g_chatPrints.clear();
	Check(DispatchListener("say_team", 3, s2sdk::ConCommandContext::Console,
						   {"say_team", "@kick", "bob", "cheating"}) ==
			  s2sdk::ResultType::Handled,
		  "a tokenised team line is handled");
	Check(AllChatPrintsAre(ExpectedAdminChat("Alice", "kick bob cheating")),
		  "the tokenised message is space-joined into the body");
	ResetClientCounters();
	g_chatPrints.clear();
	Check(DispatchListener("say_team", 3, s2sdk::ConCommandContext::Console,
						   {"say_team", "@kick bob cheating"}) == s2sdk::ResultType::Handled,
		  "a single quoted team line is handled");
	Check(AllChatPrintsAre(ExpectedAdminChat("Alice", "kick bob cheating")),
		  "the single quoted message produces the same body");
	ResetClientCounters();
	g_chatPrints.clear();
	Check(DispatchListener("say_team", 3, s2sdk::ConCommandContext::Console,
						   {"say_team", "@a", "", "b"}) == s2sdk::ResultType::Handled,
		  "an empty token is kept");
	Check(AllChatPrintsAre(ExpectedAdminChat("Alice", "a  b")),
		  "the arguments are joined with single spaces, including around an empty token");

	// --- a team line without the permission is ordinary team chat -----------------------------
	ResetClientCounters();
	g_permissionChecks.clear();
	g_chatPrints.clear();
	Check(DispatchListener("say_team", 5, s2sdk::ConCommandContext::Console,
						   {"say_team", "@hello"}) == s2sdk::ResultType::Continue,
		  "an unpermitted speaker's '@' line continues to the engine");
	Check(g_permissionChecks.size() == 1 && g_permissionChecks[0].first == 5 &&
			  g_permissionChecks[0].second == kAdminChatPermission,
		  "the unpermitted speaker is checked for @admin/chat");
	Check(g_chatPrints.empty(), "an unpermitted speaker's line is not relayed");

	// --- the speaker only needs the permission, not IsAdmin (the original's rule) -------------
	ResetClientCounters();
	g_chatPrints.clear();
	Check(DispatchListener("say_team", 6, s2sdk::ConCommandContext::Console,
						   {"say_team", "@hi"}) == s2sdk::ResultType::Handled,
		  "a permitted non-admin can send the admin chat");
	Check(AllChatPrintsAre(ExpectedAdminChat("Dave", "hi")),
		  "the permitted non-admin's line is relayed with their name");

	// --- the speaker must be a live authenticated human ---------------------------------------
	g_permissionChecks.clear();
	g_chatPrints.clear();
	Check(DispatchListener("say_team", 7, s2sdk::ConCommandContext::Console,
						   {"say_team", "@bot"}) == s2sdk::ResultType::Continue,
		  "a bot cannot use the admin chat");
	Check(DispatchListener("say_team", 8, s2sdk::ConCommandContext::Console,
						   {"say_team", "@tv"}) == s2sdk::ResultType::Continue,
		  "the SourceTV proxy cannot use the admin chat");
	Check(DispatchListener("say_team", 9, s2sdk::ConCommandContext::Console,
						   {"say_team", "@eve"}) == s2sdk::ResultType::Continue,
		  "an unauthenticated client cannot use the admin chat");
	Check(DispatchListener("say_team", 10, s2sdk::ConCommandContext::Console,
						   {"say_team", "@frank"}) == s2sdk::ResultType::Continue,
		  "a client that is not in game cannot use the admin chat");
	Check(DispatchListener("say_team", -1, s2sdk::ConCommandContext::Console,
						   {"say_team", "@console"}) == s2sdk::ResultType::Continue,
		  "the server console's say_team line continues to the engine");
	Check(g_permissionChecks.empty() && g_chatPrints.empty(),
		  "no admin-chat work for a speaker that is not a live human");

	// --- an empty chat line -------------------------------------------------------------------
	g_chatPrints.clear();
	Check(DispatchListener("say_team", 3, s2sdk::ConCommandContext::Console, {"say_team"}) ==
			  s2sdk::ResultType::Continue,
		  "an empty say_team line continues");
	Check(DispatchListener("say_team", 3, s2sdk::ConCommandContext::Console, {"say_team", ""}) ==
			  s2sdk::ResultType::Continue,
		  "an empty message continues");
	Check(DispatchListener("say", 3, s2sdk::ConCommandContext::Console, {"say"}) ==
			  s2sdk::ResultType::Continue,
		  "an empty say line continues");
	Check(g_chatPrints.empty(), "an empty chat line relays nothing");

	// --- a speaker whose name cannot be resolved ---------------------------------------------
	ResetClientCounters();
	g_chatPrints.clear();
	Check(DispatchListener("say_team", 11, s2sdk::ConCommandContext::Console,
						   {"say_team", "@hi"}) == s2sdk::ResultType::Handled,
		  "the admin with the unresolved name still has the admin chat");
	Check(AllChatPrintsAre(ExpectedAdminChat("", "hi")),
		  "s2sdk's \"<unknown>\" name is printed as an empty name");

	// --- a missing translation falls back to the key, like the original ----------------------
	g_translationUnknown = true;
	admin_system::AdminApi::Get().ReloadTranslations();
	ResetClientCounters();
	g_chatPrints.clear();
	Check(DispatchListener("say_team", 3, s2sdk::ConCommandContext::Console,
						   {"say_team", "@hello"}) == s2sdk::ResultType::Handled,
		  "the admin chat still works without the phrase");
	Check(AllChatPrintsAre("AdminChat"),
		  "a missing AdminChat phrase prints the key unchanged, like the original");

	FinishTest();
}

// --- E. the admin core and the s2sdk bindings are optional (PITFALLS A1) --------------------

void TestWithoutAdminCore() {
	PrepareTest();
	StartWithMap();
	g_clients[3] = LiveClient("Alice");
	g_permittedSlots = {3};
	g_adminSlots = {3};

	// admin_system is not loaded: the version slot (admin_api.hpp's availability bit) stays null.
	__admin_system_GetPluginVersion = nullptr;
	ResetClientCounters();
	g_permissionChecks.clear();
	Check(DispatchListener("say_team", 3, s2sdk::ConCommandContext::Console,
						   {"say_team", "@hello"}) == s2sdk::ResultType::Continue,
		  "without the admin core the admin chat passes the line through");
	Check(g_permissionChecks.empty(), "no admin API call without the core");
	Check(g_chatPrints.empty(), "nothing is relayed without the admin core");

	__admin_system_GetPluginVersion = &FakeGetPluginVersion; // restore for the next test
	FinishTest();
}

void TestStartWithoutBindings() {
	PrepareTest();
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	__s2sdk_AddCommandListener = nullptr;

	Check(g_asChatPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok,
		  "a start without the s2sdk bindings stays Ok");
	Check(g_addedListeners.empty(), "no say listener is attached without AddCommandListener");
	Check(!g_mapListeners, "the map listeners are not marked registered");
	g_asChatPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	Check(!g_mapReady, "the first update does not open a gate that has no map listeners");
	Check(Logged("map listeners are unavailable"), "the missing map bindings are logged");
	Check(Logged("AddCommandListener is unavailable"), "the missing command binding is logged");

	FinishTest();
}

// --- F. a clean unload (PITFALLS A7) ---------------------------------------------------------

void TestUnload() {
	PrepareTest();
	StartWithMap();
	g_clients[3] = LiveClient("Alice");
	g_permittedSlots = {3};
	g_adminSlots = {3};

	const FakeListener* say = FindListener("say");
	const FakeListener* sayTeam = FindListener("say_team");
	Check(say != nullptr && sayTeam != nullptr, "both chat listeners exist before the unload");
	if (say == nullptr || sayTeam == nullptr) {
		FinishTest();
		return;
	}
	const s2sdk::ConCommandCallback sayCallback = say->callback;
	const s2sdk::ConCommandCallback sayTeamCallback = sayTeam->callback;
	Check(sayCallback == sayTeamCallback, "one handler serves both chat commands");

	g_asChatPlugin.OnPluginEnd();
	Check(!g_mapReady, "OnPluginEnd drops readiness");
	Check(g_pAdminApi == nullptr, "OnPluginEnd drops the admin API");
	Check(g_mapStartUnregisters == 1 && g_mapEndUnregisters == 1,
		  "OnPluginEnd unregisters both map listeners");
	Check(g_onMapStart == nullptr && g_onMapEnd == nullptr,
		  "the map callbacks are gone from s2sdk");
	Check(RemovedListener("say", sayCallback, s2sdk::HookMode::Pre),
		  "the say listener is detached with RemoveCommandListener");
	Check(RemovedListener("say_team", sayTeamCallback, s2sdk::HookMode::Pre),
		  "the say_team listener is detached with RemoveCommandListener");
	Check(g_removedCommands.empty(), "RemoveCommand is never called (say/say_team are the engine's)");
	Check(g_consoleCommands.empty(), "this plugin never registered a console command");

	// A stale callback (the engine may still hold the pointer for a moment) is harmless.
	ResetClientCounters();
	g_chatPrints.clear();
	Check(CallCommand(sayCallback, 3, s2sdk::ConCommandContext::Console,
					  {"say_team", "@hello"}) == s2sdk::ResultType::Continue,
		  "a callback after OnPluginEnd passes the chat line through");
	Check(g_clientCalls == 0, "a callback after OnPluginEnd makes no client call");
	Check(g_chatPrints.empty(), "a callback after OnPluginEnd sends no chat output");

	// plg reload: a fresh start re-registers everything exactly once.
	g_asChatPlugin.OnPluginStart();
	Check(g_mapStartRegisters == 2 && g_mapEndRegisters == 2,
		  "a reload registers the map listeners again");
	Check(CountListeners("say") == 2 && CountListeners("say_team") == 2,
		  "a reload attaches both chat listeners again");
	Check(!g_mapReady, "a reload starts with the gate shut");
	MapStart();
	Check(g_mapReady, "the reloaded plugin opens the gate on OnMapStart");

	// The reloaded registrations are removed by the next OnPluginEnd, and a second end is a no-op.
	g_asChatPlugin.OnPluginEnd();
	Check(g_mapStartUnregisters == 2 && g_mapEndUnregisters == 2,
		  "the reloaded map listeners are unregistered");
	Check(g_removedListeners.size() == 4, "the reloaded chat listeners are detached");
	const std::size_t removedAfterReload = g_removedListeners.size();
	const int startUnregistersAfterReload = g_mapStartUnregisters;
	g_asChatPlugin.OnPluginEnd();
	Check(g_removedListeners.size() == removedAfterReload &&
			  g_mapStartUnregisters == startUnregistersAfterReload,
		  "a second OnPluginEnd detaches nothing again");

	FinishTest();
}

} // namespace

int main() {
	std::printf("as_chat tests (fake s2sdk/admin_system bindings, no engine)\n");

	InstallFakes();

	TestChatTextHelpers();
	TestMapGate();
	TestMapChangeWindowStart();
	TestClientGuards();
	TestSayRelay();
	TestWithoutAdminCore();
	TestStartWithoutBindings();
	TestUnload();

	std::printf("as_chat_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
