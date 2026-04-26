cmake_minimum_required(VERSION 3.10)
project(async_c_http C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug)

# Find required packages
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

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
)
set(CJSON_SOURCES cjson/cJSON.c)
# Add libmill library
add_library(mill STATIC ${LIBMILL_SOURCES})
add_library(cjson STATIC ${CJSON_SOURCES})
target_include_directories(mill PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/libmill
)
target_include_directories(cjson PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/cjson
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
)
target_include_directories(openai_sse_parser PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/cjson
)

# Stream Client library (libmill + pthread + curl)
add_library(openai_stream_client_lib STATIC
    openai_stream_client.c
)
add_library(llm_parser STATIC llm_parser.c)
target_link_libraries(llm_parser 
    cjson
    openai_sse_parser
)
target_link_libraries(openai_stream_client_lib
    openai_sse_parser
    cjson
    mill
    CURL::libcurl
    Threads::Threads
)
target_include_directories(openai_stream_client_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libmill
    ${CMAKE_CURRENT_SOURCE_DIR}/cjson
)

# Stream client example with async API
add_executable(openai_stream_client_async openai_stream_client_async_test.c)
# Disable stack protection flags that conflict with libmill's stack switching
target_compile_options(openai_stream_client_async PRIVATE
    #-fno-stack-protector -fno-stack-clash-protection -fcf-protection=none
    -fno-stack-clash-protection 
)
target_link_libraries(openai_stream_client_async
    openai_stream_client_lib
    openai_sse_parser
    llm_parser
    cjson
    mill
    CURL::libcurl
    Threads::Threads
)

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
