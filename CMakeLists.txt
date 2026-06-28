cmake_minimum_required(VERSION 3.10)

# Default to Debug for development — always use -g -DDEBUG
#set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)

project(async_c_http C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
#add_compile_definitions(DEBUG)

set(COMPILE_WARNING_CONTROL -Wall -Wextra -Werror)
# Find required packages
find_package(CURL REQUIRED)

# Build libmill as a static library
set(LIBMILL_SOURCES
    libmill/chan.c
    libmill/cr.c
    libmill/debug.c
    libmill/file.c
    libmill/ip.c
    libmill/list.c
    libmill/mfork.c
    libmill/poller.c
    libmill/slist.c
    libmill/stack.c
    libmill/timer.c
    libmill/udp.c
    libmill/unix.c
    libmill/ssl.c
    libmill/tcp.c
    libmill/dns/dns.c
)
set(CJSON_SOURCES cjson/cJSON.c cjson_arena.c)

# SDS dynamic string library
add_library(sds STATIC sds/sds.c)
target_include_directories(sds PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/sds
)

# Add libmill library
add_library(mill STATIC ${LIBMILL_SOURCES})
add_library(cjson STATIC ${CJSON_SOURCES})
target_include_directories(mill PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/libmill
)
target_include_directories(cjson PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/cjson
    ${CMAKE_CURRENT_SOURCE_DIR}/sds
)
target_compile_options(mill PRIVATE
    -Wall -Wextra -g -O0
    # Disable stack protection flags that conflict with libmill's stack switching
    #-fno-stack-protector -fno-stack-clash-protection -fcf-protection=none
    -fno-stack-clash-protection
)

target_compile_options(cjson PRIVATE
    -Wall -Wextra -g -O0
)

# Lua interpreter library
file(GLOB LUA_SOURCES lua/*.c)
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/lua
)
target_compile_options(lua PRIVATE
    -Wall -Wextra -g -O0 -Wno-unused-parameter -Wno-sign-compare -Wno-old-style-definition
)
# Lua needs math library
target_link_libraries(lua PUBLIC m)

# Check for epoll or kqueue
include(CheckSymbolExists)
# check_symbol_exists(epoll_create "sys/epoll.h" HAVE_EPOLL)
# check_symbol_exists(kqueue "sys/event.h" HAVE_KQUEUE)

# if(HAVE_EPOLL)
#     target_compile_definitions(mill PRIVATE MILL_EPOLL)
# elseif(HAVE_KQUEUE)
#     target_compile_definitions(mill PRIVATE MILL_KQUEUE)
# else()
target_compile_definitions(mill PRIVATE MILL_POLL)
# endif()
# Check for OpenSSL
find_package(OpenSSL)
if(OPENSSL_FOUND)
    target_compile_definitions(mill PRIVATE MILL_OPENSSL)
    target_link_libraries(mill PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

# SSE Parser library
add_library(openai_sse_parser STATIC
    openai_sse_parser.c
)
target_link_libraries(openai_sse_parser
    cjson
    sds
)
target_compile_options(openai_sse_parser PRIVATE ${COMPILE_WARNING_CONTROL})
target_include_directories(openai_sse_parser PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/cjson
    ${CMAKE_CURRENT_SOURCE_DIR}/sds
)

# Stream Client library (libmill + pthread + curl)
add_library(openai_stream_client_lib STATIC
    openai_stream_client.c
    stream_log.c
    stream_curl.c
)
target_compile_options(openai_stream_client_lib PRIVATE ${COMPILE_WARNING_CONTROL})

add_library(tool_call_parser STATIC
    tool_call_parser.c
)
target_compile_options(tool_call_parser PRIVATE ${COMPILE_WARNING_CONTROL})
target_link_libraries(tool_call_parser
    cjson
    sds
)
target_include_directories(tool_call_parser PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/cjson
    ${CMAKE_CURRENT_SOURCE_DIR}/sds
)

add_library(llm_parser STATIC llm_parser.c)
target_compile_options(llm_parser PRIVATE ${COMPILE_WARNING_CONTROL})
target_link_libraries(llm_parser
    cjson
    openai_sse_parser
    tool_call_parser
    sds
)
target_include_directories(llm_parser PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/sds
)

# LLM Runtime (high-level orchestration)
add_library(llm_runtime STATIC llm_runtime.c agent_loop.c)
target_compile_options(llm_runtime PRIVATE ${COMPILE_WARNING_CONTROL})
target_link_libraries(llm_runtime
    openai_stream_client_lib
    llm_parser
    openai_sse_parser
    cjson
    mill
    CURL::libcurl
)
target_include_directories(llm_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libmill
    ${CMAKE_CURRENT_SOURCE_DIR}/cjson
)
target_link_libraries(openai_stream_client_lib
    openai_sse_parser
    cjson
    mill
    CURL::libcurl
)
target_include_directories(openai_stream_client_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libmill
    ${CMAKE_CURRENT_SOURCE_DIR}/cjson
)

# Stream client example with async API
add_executable(openai_stream_client_async openai_stream_client_async_test.c utils.c)
# Disable stack protection flags that conflict with libmill's stack switching
target_compile_options(openai_stream_client_async PRIVATE
    #-fno-stack-protector -fno-stack-clash-protection -fcf-protection=none
    -fno-stack-clash-protection
)
target_link_libraries(openai_stream_client_async
    llm_runtime
    openai_stream_client_lib
    openai_sse_parser
    llm_parser
    cjson
    mill
    CURL::libcurl
)

# isocline library (amalgamation build)
add_library(isocline STATIC isocline/src/isocline.c)
target_include_directories(isocline PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/isocline/include
)
target_compile_options(isocline PRIVATE
    -Wall -Wextra -Wno-unused-function -Wno-unused-parameter
)

# SQLite3 (amalgamation)
add_library(sqlite3 STATIC sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/sqlite
)
target_compile_options(sqlite3 PRIVATE
    -Wall -Wextra -Wno-unused-function -Wno-unused-parameter
    -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION
)

# History database (SQLite-backed session/message storage)
add_library(history_db STATIC history_db.c)
target_compile_options(history_db PRIVATE ${COMPILE_WARNING_CONTROL})
target_link_libraries(history_db
    sqlite3
    cjson
    sds
)
target_include_directories(history_db PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/cjson
    ${CMAKE_CURRENT_SOURCE_DIR}/sqlite
    ${CMAKE_CURRENT_SOURCE_DIR}/sds
)

# Models configuration (parses ~/.agent/models.json)
add_library(models_config STATIC models_config.c)
target_compile_options(models_config PRIVATE ${COMPILE_WARNING_CONTROL})
target_link_libraries(models_config
    cjson
)
target_include_directories(models_config PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/cjson
)

# Tool functions (example tool implementations)
add_library(diff STATIC diff.c)
target_compile_options(diff PRIVATE ${COMPILE_WARNING_CONTROL})
target_include_directories(diff PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(tool_functions STATIC tool_functions.c scroll_viewport.c fuzzy_replace.c cop_lua.c)
target_compile_options(tool_functions PRIVATE ${COMPILE_WARNING_CONTROL})
target_link_libraries(tool_functions
    diff
    isocline
    llm_runtime
    cjson
    utils_utf8
    lua
    sqlite3
)
target_include_directories(tool_functions PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/cjson
    ${CMAKE_CURRENT_SOURCE_DIR}/isocline/include
    ${CMAKE_CURRENT_SOURCE_DIR}/sqlite
)

# cop — main agent executable
add_executable(cop cop.c cop_ui.c utils.c)

# cop_tcp — TCP test executable
add_executable(cop_tcp cop_tcp.c)
target_compile_options(cop_tcp PRIVATE
    -fno-stack-clash-protection
)
target_link_libraries(cop_tcp
    mill
)
target_compile_options(cop PRIVATE
    ${COMPILE_WARNING_CONTROL}
    -fno-stack-clash-protection
)
target_link_libraries(cop
    models_config
    history_db
    tool_functions
    isocline
    stream_md_renderer
    llm_runtime
    openai_stream_client_lib
    openai_sse_parser
    llm_parser
    sqlite3
    cjson
    mill
    CURL::libcurl
)

# LLM Runtime test program (legacy)
add_executable(llm_runtime_test llm_runtime_test.c utils.c)
target_compile_options(llm_runtime_test PRIVATE
    -fno-stack-clash-protection
)
target_link_libraries(llm_runtime_test
    models_config
    history_db
    tool_functions
    isocline
    llm_runtime
    openai_stream_client_lib
    openai_sse_parser
    llm_parser
    sqlite3
    cjson
    mill
    CURL::libcurl
)

# Lua interpreter test
add_executable(test_lua test_lua.c cop_lua.c)
target_compile_options(test_lua PRIVATE -std=c11 -D_GNU_SOURCE)
target_link_libraries(test_lua lua sqlite3)
target_include_directories(test_lua PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/lua
    ${CMAKE_CURRENT_SOURCE_DIR}/sqlite
)

# UTF-8 Sanitizer library and tests
add_library(utils_utf8 STATIC utils_utf8.c)
target_compile_options(utils_utf8 PRIVATE ${COMPILE_WARNING_CONTROL})
target_include_directories(utils_utf8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# md4c Markdown parser (amalgamation)
add_library(md4c STATIC
    md4c/md4c.c
    md4c/entity.c
)
target_include_directories(md4c PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/md4c
)
target_compile_options(md4c PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-declaration-after-statement
)

# Stream Markdown Renderer (incremental MD → ANSI output)
add_library(stream_md_renderer STATIC stream_md_renderer.c)
target_compile_options(stream_md_renderer PRIVATE ${COMPILE_WARNING_CONTROL})
target_link_libraries(stream_md_renderer
    md4c
    sds
    utils_utf8
)
target_include_directories(stream_md_renderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/md4c
    ${CMAKE_CURRENT_SOURCE_DIR}/sds
)

add_executable(test_sanitize_utf8 test_sanitize_utf8.c utils_utf8.c)
target_include_directories(test_sanitize_utf8 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_sanitize_utf8 PRIVATE -Wall -Wextra)
target_link_libraries(test_sanitize_utf8 sds)

# Print configuration
message(STATUS "")
message(STATUS "=== Build Configuration ===")
message(STATUS "C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "CURL: ${CURL_VERSION_STRING}")
# if(HAVE_EPOLL)
#     message(STATUS "Poller: epoll")
# elseif(HAVE_KQUEUE)
#     message(STATUS "Poller: kqueue")
# else()
#     message(STATUS "Poller: poll")
# endif()
message(STATUS "===========================")
message(STATUS "")
