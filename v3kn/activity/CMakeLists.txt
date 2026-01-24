add_library(
	activity
	STATIC
	include/activity/activity.h
	src/activity.cpp
)

target_include_directories(activity PUBLIC include)
target_link_libraries(activity PRIVATE httplib)
target_link_libraries(activity PUBLIC account utils)
