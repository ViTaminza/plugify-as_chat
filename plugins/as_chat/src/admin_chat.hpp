// Chat text helpers of as_chat: the cs2-menus colour tags of the "AdminChat" phrase, the
// printf-style formatting the original ran the phrase through, and the '@' trigger of the
// admin-chat message.
//
// s2sdk's PrintToChat sends a message to the client as it is and its PrintToChatColored uses a
// different tag table, so the {TAG} words of the production admin_system.phrases.txt
// (" {GREEN}[Админ-чат]{DEFAULT} %s: %s") are replaced here with exactly the control bytes the
// Metamod cs2-menus servers sent (pisexcompat carried the same table).
//
// Header-only and engine-free: as_chat_tests checks every tag byte-for-byte, the trigger and the
// formatting without a game, a map or the admin_system plugin.

#pragma once

#include <array>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace as_chat {
namespace detail {

// cs2-menus' utils.h colors_text[]/colors_hex[], same order and bytes as pisexcompat. The last
// entry is the chat line break: the phrase files spell it "\n" (backslash + n) and a chat line
// wants U+2029, the engine's line separator.
struct ColorTag {
	std::string_view tag;
	std::string_view bytes;
};

inline constexpr std::array<ColorTag, 17> kColorTags = {{{"{DEFAULT}", "\x01"},
														 {"{WHITE}", "\x01"},
														 {"{RED}", "\x02"},
														 {"{LIGHTPURPLE}", "\x03"},
														 {"{GREEN}", "\x04"},
														 {"{LIME}", "\x05"},
														 {"{LIGHTGREEN}", "\x06"},
														 {"{DARKRED}", "\x07"},
														 {"{GRAY}", "\x08"},
														 {"{LIGHTOLIVE}", "\x09"},
														 {"{OLIVE}", "\x10"},
														 {"{LIGHTBLUE}", "\x0B"},
														 {"{BLUE}", "\x0C"},
														 {"{PURPLE}", "\x0E"},
														 {"{LIGHTRED}", "\x0F"},
														 {"{GRAYBLUE}", "\x0A"},
														 {"\\n", "\xe2\x80\xa9"}}};

// One pass per tag, replacement left to right; an unknown {TAG} stays literal, exactly like the
// cs2-menus chat formatter of the original.
inline std::string ReplaceTags(std::string_view text) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), color.bytes);
			pos += color.bytes.size();
		}
	}
	return out;
}

} // namespace detail

// The phrase text with the colour tags turned into their chat control bytes.
inline std::string Colorize(std::string_view text) {
	return detail::ReplaceTags(text);
}

// printf into a std::string (the pisex::FormatString the original went through). The format is
// the phrase from the admin_system core, so it is not a literal and cannot be checked by the
// compiler; a missing translation is handed in as the key itself, which has no placeholders and
// therefore prints unchanged (the extra arguments are ignored), exactly like the original.
inline std::string Format(const char* format, ...) __attribute__((format(printf, 1, 2)));
inline std::string Format(const char* format, ...) {
	if (format == nullptr) {
		return {};
	}
	va_list args;
	va_start(args, format);
	va_list copy;
	va_copy(copy, args);
	const int size = std::vsnprintf(nullptr, 0, format, copy);
	va_end(copy);
	if (size <= 0) {
		va_end(args);
		return {};
	}
	std::vector<char> buffer(static_cast<std::size_t>(size) + 1, '\0');
	std::vsnprintf(buffer.data(), buffer.size(), format, args);
	va_end(args);
	return std::string(buffer.data(), static_cast<std::size_t>(size));
}

// The original's chat callback tested the first character of the parsed chat text: the '@'
// trigger of the admin chat. An empty line is not an admin-chat message.
inline bool IsAdminChatTrigger(std::string_view content) {
	return !content.empty() && content.front() == '@';
}

// Everything after the '@' trigger is the admin-chat body (the original's szMessage + 1). Empty
// for a line that is not an admin-chat message, and for a bare "@".
inline std::string_view AdminChatBody(std::string_view content) {
	return IsAdminChatTrigger(content) ? content.substr(1) : std::string_view{};
}

} // namespace as_chat
