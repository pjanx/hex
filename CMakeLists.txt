project (hex C)
cmake_minimum_required (VERSION 2.8.5)

# Moar warnings
if ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUC)
	set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99")
	set (CMAKE_C_FLAGS_DEBUG
		"${CMAKE_C_FLAGS_DEBUG} -Wall -Wextra -Wno-unused-function")
endif ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUC)

# Version
set (project_VERSION_MAJOR "0")
set (project_VERSION_MINOR "1")
set (project_VERSION_PATCH "0")

set (project_VERSION "${project_VERSION_MAJOR}")
set (project_VERSION "${project_VERSION}.${project_VERSION_MINOR}")
set (project_VERSION "${project_VERSION}.${project_VERSION_PATCH}")

# For custom modules
set (CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/liberty/cmake)

# Dependencies
find_package (Ncursesw REQUIRED)
find_package (PkgConfig REQUIRED)
find_package (Unistring REQUIRED)

include (AddThreads)

find_package (Termo QUIET NO_MODULE)
option (USE_SYSTEM_TERMO
	"Don't compile our own termo library, use the system one" ${Termo_FOUND})

if (USE_SYSTEM_TERMO)
	if (NOT Termo_FOUND)
		message (FATAL_ERROR "System termo library not found")
	endif (NOT Termo_FOUND)
else ()
	add_subdirectory (termo EXCLUDE_FROM_ALL)
	# We don't have many good choices when we don't want to install it and want
	# to support older versions of CMake; this is a relatively clean approach
	# (other possibilities: setting a variable in the parent scope, using a
	# cache variable, writing a special config file with build paths in it and
	# including it here, or setting a custom property on the targets).
	get_directory_property (Termo_INCLUDE_DIRS
		DIRECTORY termo INCLUDE_DIRECTORIES)
	set (Termo_LIBRARIES termo-static)
endif ()

set (project_libraries ${UNISTRING_LIBRARIES}
	${NCURSESW_LIBRARIES} termo-static)

pkg_search_module (lua lua53 lua5.3 lua-5.3 lua>=5.3)
option (WITH_LUA "Enable support for Lua plugins" ${lua_FOUND})

if (WITH_LUA)
	if (NOT lua_FOUND)
		message (FATAL_ERROR "Lua library not found")
	endif ()

	list (APPEND project_libraries ${lua_LIBRARIES})
	include_directories (${lua_INCLUDE_DIRS})
	link_directories (${lua_LIBRARY_DIRS})

	include (CheckTypeSize)
	set (CMAKE_REQUIRED_LIBRARIES ${lua_LIBRARIES})
	set (CMAKE_REQUIRED_INCLUDES ${lua_INCLUDE_DIRS})
	set (CMAKE_EXTRA_INCLUDE_FILES "lua.h")
	CHECK_TYPE_SIZE (lua_Integer LUA_INTEGER)
	if (NOT HAVE_LUA_INTEGER OR LUA_INTEGER LESS 8)
		message (FATAL_ERROR "Lua must have at least 64-bit integers")
	endif ()
endif ()

include_directories (${UNISTRING_INCLUDE_DIRS}
	${NCURSESW_INCLUDE_DIRS} ${Termo_INCLUDE_DIRS})

# Configuration
include (CheckFunctionExists)
set (CMAKE_REQUIRED_LIBRARIES ${NCURSESW_LIBRARIES})
CHECK_FUNCTION_EXISTS ("resizeterm" HAVE_RESIZETERM)

# Generate a configuration file
set (HAVE_LUA "${WITH_LUA}")

configure_file (${PROJECT_SOURCE_DIR}/config.h.in
	${PROJECT_BINARY_DIR}/config.h)
include_directories (${PROJECT_SOURCE_DIR} ${PROJECT_BINARY_DIR})

# Build the main executable and link it
add_executable (${PROJECT_NAME} ${PROJECT_NAME}.c)
target_link_libraries (${PROJECT_NAME} ${project_libraries})
add_threads (${PROJECT_NAME})

# Installation
include (GNUInstallDirs)
install (TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
install (FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})

# Generate documentation from program help
find_program (HELP2MAN_EXECUTABLE help2man)
if (NOT HELP2MAN_EXECUTABLE)
	message (FATAL_ERROR "help2man not found")
endif ()

foreach (page ${PROJECT_NAME})
	set (page_output "${PROJECT_BINARY_DIR}/${page}.1")
	list (APPEND project_MAN_PAGES "${page_output}")
	add_custom_command (OUTPUT ${page_output}
		COMMAND ${HELP2MAN_EXECUTABLE} -N
			"${PROJECT_BINARY_DIR}/${page}" -o ${page_output}
		DEPENDS ${page}
		COMMENT "Generating man page for ${page}" VERBATIM)
endforeach ()

add_custom_target (docs ALL DEPENDS ${project_MAN_PAGES})

foreach (page ${project_MAN_PAGES})
	string (REGEX MATCH "\\.([0-9])$" manpage_suffix "${page}")
	install (FILES "${page}"
		DESTINATION "${CMAKE_INSTALL_MANDIR}/man${CMAKE_MATCH_1}")
endforeach ()

# CPack
set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "Hex viewer")
set (CPACK_PACKAGE_VENDOR "Premysl Janouch")
set (CPACK_PACKAGE_CONTACT "Přemysl Janouch <p.janouch@gmail.com>")
set (CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set (CPACK_PACKAGE_VERSION_MAJOR ${project_VERSION_MAJOR})
set (CPACK_PACKAGE_VERSION_MINOR ${project_VERSION_MINOR})
set (CPACK_PACKAGE_VERSION_PATCH ${project_VERSION_PATCH})
set (CPACK_GENERATOR "TGZ;ZIP")
set (CPACK_PACKAGE_FILE_NAME
	"${PROJECT_NAME}-${project_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set (CPACK_PACKAGE_INSTALL_DIRECTORY "${PROJECT_NAME}-${project_VERSION}")
set (CPACK_SOURCE_GENERATOR "TGZ;ZIP")
set (CPACK_SOURCE_IGNORE_FILES "/\\\\.git;/build;/CMakeLists.txt.user")
set (CPACK_SOURCE_PACKAGE_FILE_NAME "${PROJECT_NAME}-${project_VERSION}")

set (CPACK_SET_DESTDIR TRUE)
include (CPack)
