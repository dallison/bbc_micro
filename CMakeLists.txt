cmake_minimum_required(VERSION 3.16)
project(bbc_micro C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_compile_definitions(_GNU_SOURCE)
endif()

set(BBC_INCLUDES
  ${CMAKE_SOURCE_DIR}/6502_interpreter
  ${CMAKE_SOURCE_DIR}/c_compiler
  ${CMAKE_SOURCE_DIR}/c_compiler/util
  ${CMAKE_SOURCE_DIR}/c_compiler/ELF
  ${CMAKE_SOURCE_DIR}/c_compiler/Loader
  ${CMAKE_SOURCE_DIR}/c_compiler/Loader/Architecture
  ${CMAKE_SOURCE_DIR}/c_compiler/6502
)

if(WIN32)
  # compat/win32 supplies the POSIX headers MinGW lacks. Windows 7 is the
  # oldest system with WSAPoll and the other calls used here.
  add_compile_definitions(_WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX
                          _POSIX_THREAD_SAFE_FUNCTIONS=200112L)
  list(APPEND BBC_INCLUDES ${CMAKE_SOURCE_DIR}/compat/win32)
  set(BBC_SOCKET_LIBS ws2_32)
  # open() defaults to text mode on Windows, which rewrites CR LF in ELF
  # images and guest files. MinGW's binmode.o makes binary the default.
  execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=binmode.o
                  OUTPUT_VARIABLE BBC_BINMODE OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(EXISTS "${BBC_BINMODE}")
    add_link_options("${BBC_BINMODE}")
  endif()
  # Static runtime libraries, so the .exe files run without MinGW DLLs.
  add_link_options(-static)
else()
  set(BBC_SOCKET_LIBS)
endif()

find_package(Threads REQUIRED)

add_library(utils STATIC
  c_compiler/util/binary_tree.c
  c_compiler/util/bitset.c
  c_compiler/util/buffer.c
  c_compiler/util/config.c
  c_compiler/util/dstring.c
  c_compiler/util/hashtable.c
  c_compiler/util/list.c
  c_compiler/util/map.c
  c_compiler/util/set.c
  c_compiler/util/vector.c
)
target_include_directories(utils PUBLIC ${BBC_INCLUDES})

add_library(elf STATIC
  c_compiler/ELF/elf_format.c
  c_compiler/ELF/elf_reader.c
  c_compiler/ELF/elf_writer.c
)
target_include_directories(elf PUBLIC ${BBC_INCLUDES})
target_link_libraries(elf PUBLIC utils)

add_library(filesystem_host STATIC c_compiler/Loader/filesystem_host.c)
target_include_directories(filesystem_host PUBLIC ${BBC_INCLUDES})

add_library(chrono_host STATIC c_compiler/Loader/chrono_host.c)
target_include_directories(chrono_host PUBLIC ${BBC_INCLUDES})
target_link_libraries(chrono_host PUBLIC filesystem_host)

add_library(random_host STATIC c_compiler/Loader/random_host.c)
target_include_directories(random_host PUBLIC ${BBC_INCLUDES})

add_library(loader STATIC
  c_compiler/Loader/Architecture/loader_arch_6502.c
  c_compiler/Loader/loader.c
  c_compiler/Loader/loader_dynamic.c
  c_compiler/Loader/loader_lifecycle.c
)
target_include_directories(loader PUBLIC ${BBC_INCLUDES})
target_link_libraries(loader PUBLIC elf utils)

add_library(6502_lib STATIC
  6502_interpreter/6502_debugger.c
  6502_interpreter/6502_devices.c
  6502_interpreter/6502_disassembler.c
  6502_interpreter/6502_interpreter.c
  6502_interpreter/6502_nmos.c
  6502_interpreter/bbc_hardware.c
)
target_include_directories(6502_lib PUBLIC ${BBC_INCLUDES})
target_link_libraries(6502_lib PUBLIC
  elf
  chrono_host
  filesystem_host
  random_host
  loader
  utils
  Threads::Threads
  ${BBC_SOCKET_LIBS}
)

add_executable(davecc6502 6502_interpreter/main.c)
target_link_libraries(davecc6502 PRIVATE 6502_lib)
set_target_properties(davecc6502 PROPERTIES OUTPUT_NAME 6502)

add_executable(cumana 6502_interpreter/cumana.c)
target_include_directories(cumana PRIVATE ${BBC_INCLUDES})
target_link_libraries(cumana PRIVATE ${BBC_SOCKET_LIBS})

add_executable(cassette 6502_interpreter/cassette.c)
target_include_directories(cassette PRIVATE ${BBC_INCLUDES})
target_link_libraries(cassette PRIVATE ${BBC_SOCKET_LIBS})

add_executable(fileserver 6502_interpreter/fileserver.c)
target_include_directories(fileserver PRIVATE ${BBC_INCLUDES})
target_link_libraries(fileserver PRIVATE ${BBC_SOCKET_LIBS})

add_executable(bbc_hardware_test
  6502_interpreter/bbc_hardware.c
  6502_interpreter/bbc_hardware_test.c
)
target_include_directories(bbc_hardware_test PRIVATE ${BBC_INCLUDES})
target_link_libraries(bbc_hardware_test PRIVATE Threads::Threads ${BBC_SOCKET_LIBS})

add_executable(6502_nmos_test 6502_interpreter/6502_nmos_test.c)
target_link_libraries(6502_nmos_test PRIVATE 6502_lib)

if(APPLE)
  enable_language(OBJC)
  add_executable(bbc
    6502_interpreter/bbc_entry.c
    6502_interpreter/bbc_host.c
    6502_interpreter/bbc_session.c
  )
  set_source_files_properties(6502_interpreter/bbc_host.c PROPERTIES LANGUAGE OBJC)
  target_compile_options(bbc PRIVATE -fobjc-arc)
  target_link_libraries(bbc PRIVATE
    6502_lib
    "-framework Cocoa"
    "-framework Carbon"
    "-framework AudioToolbox"
  )
elseif(WIN32)
  # GDI, waveOut, and the timer are in every Windows install.
  add_executable(bbc WIN32
    6502_interpreter/bbc_windows.c
    6502_interpreter/bbc_session.c
  )
  target_link_libraries(bbc PRIVATE 6502_lib gdi32 user32 winmm)
elseif(UNIX)
  # libX11 is already installed on a desktop Ubuntu or Debian system. The
  # compiler also needs the headers from libx11-dev. ALSA is optional.
  find_package(X11)
  find_path(ALSA_INCLUDE_DIR alsa/asoundlib.h)
  find_library(ALSA_LIBRARY asound)
  if(X11_FOUND)
    add_executable(bbc
      6502_interpreter/bbc_linux.c
      6502_interpreter/bbc_session.c
    )
    target_include_directories(bbc PRIVATE ${BBC_INCLUDES} ${X11_INCLUDE_DIR})
    target_link_libraries(bbc PRIVATE 6502_lib ${X11_LIBRARIES} Threads::Threads)
    if(ALSA_INCLUDE_DIR AND ALSA_LIBRARY)
      target_compile_definitions(bbc PRIVATE BBC_HAVE_ALSA=1)
      target_include_directories(bbc PRIVATE ${ALSA_INCLUDE_DIR})
      target_link_libraries(bbc PRIVATE ${ALSA_LIBRARY})
    endif()
  else()
    message(STATUS "libX11 not found; the bbc window is not being built. Install libx11-dev.")
  endif()
endif()

enable_testing()
add_test(NAME bbc_hardware_test COMMAND bbc_hardware_test)
add_test(NAME 6502_nmos_test COMMAND 6502_nmos_test)
if(TARGET bbc)
  add_test(NAME bbc_selftest COMMAND bbc -selftest)
endif()
