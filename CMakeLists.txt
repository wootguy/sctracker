cmake_minimum_required(VERSION 3.0)
project(SvenTracker)

set(SOURCE_FILES 
    src/main.cpp
    src/util.h src/util.cpp
)

include_directories(include)
include_directories(src)
add_executable(${PROJECT_NAME} ${SOURCE_FILES})

if(MSVC)
    # compile using the static runtime
	add_compile_definitions(CURL_STATICLIB)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
	set(CMAKE_CXX_FLAGS "/w /EHsc")
	target_link_libraries(${PROJECT_NAME} WS2_32 IPHLPAPI)
else()
    set(CMAKE_CXX_FLAGS "-Wall")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
    set(CMAKE_CXX_FLAGS_RELEASE "-Os -w -Wfatal-errors")
	target_link_libraries(${PROJECT_NAME} -lcurl)
endif()

