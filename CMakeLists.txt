cmake_minimum_required(VERSION 3.22)

project(v3kn)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
	string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
	string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
	string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
	string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
	string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
	string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
endif()

if(WIN32)
	add_compile_definitions("_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS" "_CRT_SECURE_NO_WARNINGS" "NOMINMAX")

	# Disable inclusion of timestamps in PCH files on Windows
	if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		string(APPEND CMAKE_C_FLAGS " -Xclang -fno-pch-timestamp")
		string(APPEND CMAKE_CXX_FLAGS " -Xclang -fno-pch-timestamp")
	endif()
endif()

add_subdirectory(external)
add_subdirectory(v3kn)
