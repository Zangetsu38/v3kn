execute_process(
	COMMAND git rev-parse --short HEAD
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	OUTPUT_VARIABLE GIT_HASH
	OUTPUT_STRIP_TRAILING_WHITESPACE)

if(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
	set(LINUX TRUE)
endif()

add_subdirectory(account)
add_subdirectory(friend)
add_subdirectory(messages)
add_subdirectory(storage)
add_subdirectory(utils)
add_subdirectory(version)

add_executable(v3kn main.cpp)

target_link_libraries(v3kn PRIVATE account friend httplib messages nlohmann_json::nlohmann_json ssl storage utils version)

set_target_properties(v3kn PROPERTIES OUTPUT_NAME v3kn
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(WIN32)
	set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT v3kn)
	set_target_properties(v3kn PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$(Configuration)")
	if(EXISTS "${OPENSSL_ROOT_DIR}")
		add_custom_command(
		TARGET v3kn
		POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENSSL_ROOT_DIR}/bin/libssl-3-x64.dll" "$<TARGET_FILE_DIR:v3kn>"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENSSL_ROOT_DIR}/bin/libcrypto-3-x64.dll" "$<TARGET_FILE_DIR:v3kn>")
	endif()
elseif(LINUX)
	set_target_properties(v3kn PROPERTIES OUTPUT_NAME "v3kn.bin")
	add_custom_command(
	TARGET v3kn
	POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/script/update-linux.sh" "$<TARGET_FILE_DIR:v3kn>/update-v3kn.sh")
endif()

add_custom_command(
	TARGET v3kn
	POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/favicon.ico" "$<TARGET_FILE_DIR:v3kn>/favicon.ico")
