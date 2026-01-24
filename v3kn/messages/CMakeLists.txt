add_library(
	messages
	STATIC
	include/messages/messages.h
	src/messages.cpp
)

target_include_directories(messages PUBLIC include)
target_link_libraries(messages PRIVATE httplib)
target_link_libraries(messages PUBLIC utils)
