# as_chat (Plugify)

Admin chat for the admin_system plugin, ported from the Pisex Metamod module '[AS] Chat'. A team-chat message that starts with '@' from a player holding '@admin/chat' is not broadcast as a team message; it is delivered as one non-team chat line to every connected, authenticated non-bot admin that has '@admin/chat', formatted with the 'AdminChat' phrase of the admin system (admin_system/translations/admin_system.phrases.txt). Every other chat message passes through untouched. No exported methods.

Plugify port (fork) of **Chat ([AS] Chat)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-admin_system_modules
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.1.0
- Plugify dependencies: s2sdk (>=2.18.2), admin_system (>=1.0.0)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `as_chat-1.1.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/as_chat`.

`plugins/admin_system` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

