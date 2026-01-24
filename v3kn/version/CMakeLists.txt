add_library(
	version
	STATIC
	include/version/version.h
	version.cpp
)

configure_file(src/version.cpp.in version.cpp)

target_include_directories(version PUBLIC include)
