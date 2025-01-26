cmake_minimum_required (VERSION 3.5)
project (mg)

set (MG_SRC autoexec.c basic.c bell.c buffer.c cinfo.c dir.c display.c
	echo.c extend.c file.c fileio.c funmap.c help.c kbd.c keymap.c
	line.c macro.c main.c match.c modes.c paragraph.c
	re_search.c region.c search.c spawn.c tty.c ttyio.c ttykbd.c
	undo.c util.c version.c window.c word.c yank.c
	cmode.c cscope.c dired.c grep.c tags.c interpreter.c
)

add_executable (mg ${MG_SRC})

find_package (PkgConfig REQUIRED)
pkg_check_modules (NCURSES REQUIRED ncurses)
string (REPLACE ";" " -I" NCURSES_FLAGS "${NCURSES_INCLUDE_DIRS}")
set (NCURSES_FLAGS "-I${NCURSES_FLAGS}")
target_link_libraries (mg ${NCURSES_LIBRARIES} util)

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  pkg_check_modules (BSD REQUIRED libbsd-overlay)
  link_directories (${BSD_LIBRARY_DIRS})
  add_definitions(-DHAVE_PTY_H)
  string (REPLACE ";" " " LIBBSD_FLAGS "${BSD_CFLAGS}")
  target_link_libraries (mg ${BSD_LIBRARIES})
  set (CMAKE_C_FLAGS "-Wall -DREGEX -D_GNU_SOURCE ${LIBBSD_FLAGS} ${NCURSES_FLAGS} -L${NCURSES_LIBRARY_DIRS}")
else()
  set (CMAKE_C_FLAGS "-Wall -DREGEX ${LIBBSD_FLAGS} ${NCURSES_FLAGS} -L${NCURSES_LIBRARY_DIRS}")
endif()
