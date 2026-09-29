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
)

add_executable(davecc6502 6502_interpreter/main.c)
target_link_libraries(davecc6502 PRIVATE 6502_lib)
set_target_properties(davecc6502 PROPERTIES OUTPUT_NAME 6502)

add_executable(cumana 6502_interpreter/cumana.c)
target_include_directories(cumana PRIVATE ${BBC_INCLUDES})

add_executable(cassette 6502_interpreter/cassette.c)
target_include_directories(cassette PRIVATE ${BBC_INCLUDES})

add_executable(bbc_hardware_test
  6502_interpreter/bbc_hardware.c
  6502_interpreter/bbc_hardware_test.c
)
target_include_directories(bbc_hardware_test PRIVATE ${BBC_INCLUDES})
target_link_libraries(bbc_hardware_test PRIVATE Threads::Threads)

add_executable(6502_nmos_test 6502_interpreter/6502_nmos_test.c)
target_link_libraries(6502_nmos_test PRIVATE 6502_lib)

if(APPLE)
  enable_language(OBJC)
  add_executable(bbc
    6502_interpreter/bbc_entry.c
    6502_interpreter/bbc_host.c
  )
  set_source_files_properties(6502_interpreter/bbc_host.c PROPERTIES LANGUAGE OBJC)
  target_compile_options(bbc PRIVATE -fobjc-arc)
  target_link_libraries(bbc PRIVATE
    6502_lib
    "-framework Cocoa"
    "-framework AudioToolbox"
  )
endif()

enable_testing()
add_test(NAME bbc_hardware_test COMMAND bbc_hardware_test)
add_test(NAME 6502_nmos_test COMMAND 6502_nmos_test)
if(APPLE)
  add_test(NAME bbc_selftest COMMAND bbc -selftest)
endif()
