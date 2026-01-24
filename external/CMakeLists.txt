# Set folder for external projects (for project tree in IDE)
set(CMAKE_FOLDER externals)

if(WIN32)
	add_library(winsock INTERFACE)
	find_library(WSOCK32 wsock32)
	find_library(WS2_32 ws2_32)
	find_library(IPHLPAPI iphlpapi)
	target_link_libraries(winsock INTERFACE WSOCK32 WS2_32 IPHLPAPI)
endif()

execute_process(
	COMMAND brew --prefix openssl
	RESULT_VARIABLE BREW_RESULT
	OUTPUT_VARIABLE BREW_OPENSSL
	OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(BREW_RESULT EQUAL 0)
	set(OPENSSL_ROOT_DIR "${BREW_OPENSSL}")
	set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()

find_package(OpenSSL QUIET)

if(NOT OPENSSL_FOUND)
	if(MSVC)
		message("OpenSSL not found, using prebuilt version")

		if(NOT EXISTS "${CMAKE_BINARY_DIR}/external/openssl.zip")
			message(STATUS "Downloading openssl...")
			file(DOWNLOAD https://firedaemon.com/download-firedaemon-openssl-3-5-zip
				"${CMAKE_BINARY_DIR}/external/openssl.zip" SHOW_PROGRESS)
		endif()

		if(NOT EXISTS "${CMAKE_BINARY_DIR}/external/openssl")
			file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/external/openssl")
			execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf "${CMAKE_BINARY_DIR}/external/openssl.zip"
				WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/external/openssl")
		endif()

		set(OPENSSL_ROOT_DIR "${CMAKE_BINARY_DIR}/external/openssl/x64" CACHE STRING "Path to OpenSSL root")
	endif()
	find_package(OpenSSL REQUIRED)
endif()

message("OpenSSL include dir: ${OPENSSL_INCLUDE_DIR}")
message("OpenSSL libraries: ${OPENSSL_LIBRARIES}")

add_library(crypto INTERFACE)
target_link_libraries(crypto INTERFACE OpenSSL::Crypto)

add_library(ssl INTERFACE)
target_link_libraries(ssl INTERFACE OpenSSL::SSL)

add_library(httplib INTERFACE)
target_include_directories(httplib INTERFACE cpp-httplib)

add_subdirectory(json)

add_subdirectory(pugixml)
