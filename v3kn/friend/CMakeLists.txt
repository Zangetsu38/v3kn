add_library(
	friend
	STATIC
	include/friend/friend.h
	src/friend.cpp
)

target_include_directories(friend PUBLIC include)
target_link_libraries(friend PRIVATE httplib)
target_link_libraries(friend PUBLIC utils)
