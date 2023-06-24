# meson.buid file for mg

# This file has been written by Han Boetes <hboetes@gmail.com> and is
# released in Public Domain.

project(
  'mg', 'c',
  version: run_command('git', 'describe', '--tags', check: true).stdout().strip(),
  default_options: ['warning_level=1'],
)

bsdlib_dep  = dependency('libbsd-overlay')
ncurses_dep = dependency('ncurses')

add_global_arguments('-DREGEX', language : 'c')
add_global_arguments('-D_GNU_SOURCE', language : 'c')
add_global_arguments('-DHAVE_PTY_H', language : 'c')
add_global_arguments('-Wno-strict-aliasing', language : 'c')
add_global_arguments('-Wno-deprecated-declarations', language : 'c')

executable(
  'mg', 'cinfo.c', 'echo.c', 'line.c', 'ttyio.c', 'log.c',
  'autoexec.c', 'bell.c', 'cscope.c', 'dir.c', 'dired.c', 'file.c',
  'fileio.c', 'grep.c', 'help.c', 'kbd.c', 'macro.c', 're_search.c',
  'region.c', 'search.c', 'spawn.c', 'tags.c', 'tty.c', 'ttykbd.c',
  'undo.c', 'version.c', 'window.c', 'word.c', 'yank.c', 'main.c',
  'basic.c', 'buffer.c', 'cmode.c', 'display.c', 'extend.c',
  'funmap.c', 'interpreter.c', 'keymap.c', 'match.c', 'modes.c',
  'paragraph.c', 'util.c',
  install: true,
  dependencies: [bsdlib_dep, ncurses_dep],
)

install_man('mg.1')
