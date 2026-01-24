add_library(
	utils
	STATIC
	include/utils/concurrency.h
	include/utils/utils.h
	src/concurrency.cpp
	src/utils.cpp
)

target_include_directories(utils PUBLIC include)
target_link_libraries(utils PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(utils PRIVATE httplib ssl)
