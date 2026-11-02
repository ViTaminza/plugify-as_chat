# as_chat — Plugify C++ port of the Pisex Metamod module "[AS] Chat": relays team-chat messages
# starting with '@' from admins with "@admin/chat" to the admins, instead of broadcasting them.
#
#   plugify-as_chat  the plugin shared library (entry "bin/as_chat" -> bin/libas_chat.so)
#
# Pure s2sdk: no s2guard and no pisexcompat (docs/PLUGIN_STANDARD.md §7). The guards of
# handoff/weaponskins/PITFALLS.md live in src/plugin.cpp itself - the B1/B2 map gate, the C2
# client checks and the A7 unload rules. Like every admin_system module the .so compiles the
# binding globals the plugify module fills at load time:
#   * s2sdk (__s2sdk_*),
#   * the generated admin_system consumer bindings (__admin_system_*) plus the hand-written
#     consumer wrapper include/admin_api.hpp.

set(AS_CHAT_VERSION "1.1.0")

# The generated admin_system binding globals (__admin_system_*) have to be compiled into every
# consumer plugin; the admin_system plugin directory already builds them as an object library
# (it is added before plugins/as_*), this is the fallback for a build of this directory alone.
if(NOT TARGET admin_system_bindings_obj)
    add_library(admin_system_bindings_obj OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include/admin_system/admin_system.cpp")
    target_include_directories(admin_system_bindings_obj SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(admin_system_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

add_library(plugify-as_chat SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:admin_system_bindings_obj>
)
target_include_directories(plugify-as_chat PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: the generated s2sdk/admin_system/plg headers and admin_api.hpp are not ours to fix;
# their warnings must not fail -Werror on our sources.
target_include_directories(plugify-as_chat SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-as_chat PRIVATE
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-as_chat PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-as_chat PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="as_chat"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-as_chat PROPERTIES OUTPUT_NAME as_chat)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-as_chat PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/as_chat.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/as_chat.pplugin"
)

# --- tests -------------------------------------------------------------------------
# as_chat_tests: engine-free. The plugin TU is compiled into the test (its handlers live in an
# anonymous namespace) and the __s2sdk_* / __admin_system_* binding globals are recording fakes:
# the colour tags and the printf formatting of the phrase, the map gate, the client guards, the
# say/say_team relay and the unload rules. Written into build/tests so ../test.sh runs it with
# the rest (*_tests convention).
#
# The top-level CMakeLists declares the PLUGIFY_PORT_TESTS option *after* the plugin directories,
# so on a first configure it is not defined here yet: default it to the same ON. An explicit
# -DPLUGIFY_PORT_TESTS=OFF is already in the cache and still wins.
if(NOT DEFINED PLUGIFY_PORT_TESTS)
    set(PLUGIFY_PORT_TESTS ON)
endif()
if(PLUGIFY_PORT_TESTS)
    add_executable(as_chat_tests
            tests/as_chat_tests.cpp
            $<TARGET_OBJECTS:s2sdk_bindings>
            $<TARGET_OBJECTS:admin_system_bindings_obj>
    )
    target_include_directories(as_chat_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(as_chat_tests SYSTEM PRIVATE
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_link_libraries(as_chat_tests PRIVATE plugify_sdk -static-libstdc++ -static-libgcc)
    target_compile_options(as_chat_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(as_chat_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            PLUGIFY_MODULE="as_chat"
            PLUGIFY_IS_DEBUG=0
            PLUGIFY_IS_RELEASE=1
    )
    set_target_properties(as_chat_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
