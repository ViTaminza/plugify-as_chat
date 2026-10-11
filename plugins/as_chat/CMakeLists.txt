# as_chat — Plugify C++ port of the Pisex Metamod module "[AS] Chat": relays team-chat messages
# starting with '@' from admins with "@admin/chat" to the admins, instead of broadcasting them.
#
#   plugify-as_chat  the plugin shared library (entry "bin/as_chat" -> bin/libas_chat.so)
#
# Like every admin_system module it links:
#   * pisexcompat (which brings the s2sdk binding globals through its own s2sdk_bindings link),
#   * the generated admin_system consumer bindings (__admin_system_*) and the hand-written
#     consumer wrapper include/admin_api.hpp.

set(AS_CHAT_VERSION "1.0.0")

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
        $<TARGET_OBJECTS:admin_system_bindings_obj>
)
target_include_directories(plugify-as_chat PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: the generated admin_system/plg headers and admin_api.hpp are not ours to fix; their
# warnings must not fail -Werror on our sources.
target_include_directories(plugify-as_chat SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
# pisexcompat PUBLIC-links s2sdk_bindings, so the __s2sdk_* globals are in the plugin as well.
target_link_libraries(plugify-as_chat PRIVATE
        pisexcompat
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
