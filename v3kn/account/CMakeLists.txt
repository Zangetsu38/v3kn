add_library(
	account
	STATIC
	include/account/account.h
	src/account.cpp
)

target_include_directories(account PUBLIC include)
target_link_libraries(account PRIVATE httplib)
target_link_libraries(account PUBLIC friend utils)
