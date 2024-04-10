cmake_minimum_required (VERSION 3.0...3.27)
project (hex VERSION 0.1.0 LANGUAGES C)

# Moar warnings
if ("${CMAKE_C_COMPILER_ID}" MATCHES "GNU" OR CMAKE_COMPILER_IS_GNUCC)
	set (wdisabled "-Wno-unused-function")
	set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99 -Wall -Wextra ${wdisabled}")
endif ()

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
	endif ()
else ()
	# We don't want the library to install, but EXCLUDE_FROM_ALL ignores tests
	add_subdirectory (termo EXCLUDE_FROM_ALL)
	file (WRITE ${PROJECT_BINARY_DIR}/CTestCustom.cmake
		"execute_process (COMMAND ${CMAKE_COMMAND} --build termo)")

	# We don't have many good choices; this is a relatively clean approach
	# (other possibilities: setting a variable in the parent scope, using
	# a cache variable, writing a special config file with build paths in it
	# and including it here, or setting a custom property on the targets)
	get_directory_property (Termo_INCLUDE_DIRS
		DIRECTORY termo INCLUDE_DIRECTORIES)
	set (Termo_LIBRARIES termo-static)
endif ()

set (project_libraries ${Unistring_LIBRARIES}
	${Ncursesw_LIBRARIES} ${Termo_LIBRARIES})

pkg_search_module (lua lua53 lua5.3 lua-5.3 lua54 lua5.4 lua-5.4 lua>=5.3)
option (WITH_LUA "Enable support for Lua plugins" ${lua_FOUND})
if (WITH_LUA)
	if (NOT lua_FOUND)
		message (FATAL_ERROR "Lua library not found")
	endif ()

	list (APPEND project_libraries ${lua_LIBRARIES})
	include_directories (${lua_INCLUDE_DIRS})
	link_directories (${lua_LIBRARY_DIRS})
endif ()

pkg_check_modules (x11 x11 xrender xft fontconfig libpng)
option (WITH_X11 "Build with X11 support" ${x11_FOUND})
if (WITH_X11)
	if (NOT x11_FOUND)
		message (FATAL_ERROR "Some X11 libraries were not found")
	endif ()

	list (APPEND project_libraries ${x11_LIBRARIES})
	include_directories (${x11_INCLUDE_DIRS})
	link_directories (${x11_LIBRARY_DIRS})
endif ()

include_directories (${Unistring_INCLUDE_DIRS}
	${Ncursesw_INCLUDE_DIRS} ${Termo_INCLUDE_DIRS})

# Configuration
if ("${CMAKE_SYSTEM_NAME}" MATCHES "BSD")
	# Need this for SIGWINCH in FreeBSD and OpenBSD respectively;
	# our POSIX version macros make it undefined
	add_definitions (-D__BSD_VISIBLE=1 -D_BSD_SOURCE=1)
elseif (APPLE)
	add_definitions (-D_DARWIN_C_SOURCE)
endif ()

include (CheckFunctionExists)
set (CMAKE_REQUIRED_LIBRARIES ${Ncursesw_LIBRARIES})
CHECK_FUNCTION_EXISTS ("resizeterm" HAVE_RESIZETERM)

# Generate a configuration file
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
if (WITH_LUA)
	install (DIRECTORY plugins
		DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME})
endif ()
if (WITH_X11)
	include (IconUtils)

	set (icon_base ${PROJECT_BINARY_DIR}/icons)
	set (icon_png_list)
	foreach (icon_size 16 32 48)
		icon_to_png (${PROJECT_NAME} ${PROJECT_SOURCE_DIR}/${PROJECT_NAME}.svg
			${icon_size} ${icon_base} icon_png)
		list (APPEND icon_png_list ${icon_png})
	endforeach ()

	add_custom_target (icons ALL DEPENDS ${icon_png_list})

	install (FILES ${PROJECT_NAME}.svg
		DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
	install (DIRECTORY ${icon_base}
		DESTINATION ${CMAKE_INSTALL_DATADIR})
	install (FILES ${PROJECT_NAME}.desktop
		DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif ()

# Generate documentation from text markup
find_program (ASCIIDOCTOR_EXECUTABLE asciidoctor)
find_program (A2X_EXECUTABLE a2x)
if (NOT ASCIIDOCTOR_EXECUTABLE AND NOT A2X_EXECUTABLE)
	message (WARNING "Neither asciidoctor nor a2x were found, "
		"falling back to a substandard manual page generator")
endif ()

foreach (page ${PROJECT_NAME})
	set (page_output "${PROJECT_BINARY_DIR}/${page}.1")
	list (APPEND project_MAN_PAGES "${page_output}")
	if (ASCIIDOCTOR_EXECUTABLE)
		add_custom_command (OUTPUT ${page_output}
			COMMAND ${ASCIIDOCTOR_EXECUTABLE} -b manpage
				-a release-version=${PROJECT_VERSION}
				-o "${page_output}"
				"${PROJECT_SOURCE_DIR}/${page}.adoc"
			DEPENDS ${page}.adoc
			COMMENT "Generating man page for ${page}" VERBATIM)
	elseif (A2X_EXECUTABLE)
		add_custom_command (OUTPUT ${page_output}
			COMMAND ${A2X_EXECUTABLE} --doctype manpage --format manpage
				-a release-version=${PROJECT_VERSION}
				-D "${PROJECT_BINARY_DIR}"
				"${PROJECT_SOURCE_DIR}/${page}.adoc"
			DEPENDS ${page}.adoc
			COMMENT "Generating man page for ${page}" VERBATIM)
	else ()
		set (ASCIIMAN ${PROJECT_SOURCE_DIR}/liberty/tools/asciiman.awk)
		add_custom_command (OUTPUT ${page_output}
			COMMAND env LC_ALL=C asciidoc-release-version=${PROJECT_VERSION}
				awk -f ${ASCIIMAN} "${PROJECT_SOURCE_DIR}/${page}.adoc"
				> ${page_output}
			DEPENDS ${page}.adoc ${ASCIIMAN}
			COMMENT "Generating man page for ${page}" VERBATIM)
	endif ()
endforeach ()

add_custom_target (docs ALL DEPENDS ${project_MAN_PAGES})

foreach (page ${project_MAN_PAGES})
	string (REGEX MATCH "\\.([0-9])$" manpage_suffix "${page}")
	install (FILES "${page}"
		DESTINATION "${CMAKE_INSTALL_MANDIR}/man${CMAKE_MATCH_1}")
endforeach ()

# CPack
set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "Interpreting hex viewer")
set (CPACK_PACKAGE_VENDOR "Premysl Eric Janouch")
set (CPACK_PACKAGE_CONTACT "Přemysl Eric Janouch <p@janouch.name>")
set (CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set (CPACK_GENERATOR "TGZ;ZIP")
set (CPACK_PACKAGE_FILE_NAME
	"${PROJECT_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set (CPACK_PACKAGE_INSTALL_DIRECTORY "${PROJECT_NAME}-${PROJECT_VERSION}")
set (CPACK_SOURCE_GENERATOR "TGZ;ZIP")
set (CPACK_SOURCE_IGNORE_FILES "/\\\\.git;/build;/CMakeLists.txt.user")
set (CPACK_SOURCE_PACKAGE_FILE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}")

set (CPACK_SET_DESTDIR TRUE)
include (CPack)
