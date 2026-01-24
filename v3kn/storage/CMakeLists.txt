add_library(
	storage
	STATIC
	include/storage/storage.h
	src/storage.cpp
)

target_include_directories(storage PUBLIC include)
target_link_libraries(storage PRIVATE httplib pugixml utils)
