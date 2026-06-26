cmake_minimum_required(VERSION 3.20)
project(cdp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(Boost CONFIG REQUIRED COMPONENTS system thread)

add_library(cdp STATIC
    src/cdp/browser.cpp
    src/cdp/page.cpp
    src/cdp/cookie.cpp
    src/cdp/result.cpp
    src/detail/channel.cpp
    src/detail/connection.cpp
 )

target_include_directories(cdp 
    PUBLIC include
    PRIVATE src
)

target_link_libraries(cdp PUBLIC
    nlohmann_json::nlohmann_json
    Boost::system
    Boost::thread
)

add_executable(cdp_test src/cdp/main.cpp)
target_link_libraries(cdp_test PRIVATE cdp)