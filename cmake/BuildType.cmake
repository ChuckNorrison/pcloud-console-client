# This file is part of the pCloud Console Client.
#
# (c) 2021 Serghei Iakovlev <egrep@protonmail.ch>
#
# For the full copyright and license information, please view
# the LICENSE file that was distributed with this source code.

# CMAKE_BUILD_TYPE is not used by multi-configuration generators.
# Thus, we need to check this first.
get_property(isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if(NOT isMultiConfig) # Makefiles, Ninja, ...
  set(_allowed_build_types Asan Ubsan Debug Release RelWithDebInfo MinSizeRel)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "${_allowed_build_types}")

  # - The default value for CMAKE_BUILD_TYPE is an empty string
  # - User can set CMAKE_BUILD_TYPE to any value at the cmake command line
  #
  # Therefore, we check both cases, and make sure that we are dealing with
  # a known build type.
  if(NOT CMAKE_BUILD_TYPE)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
  elseif(NOT CMAKE_BUILD_TYPE IN_LIST _allowed_build_types)
    message(FATAL_ERROR "Invalid build type: ${CMAKE_BUILD_TYPE}")
  endif()

  unset(_allowed_build_types)
elseif(NOT MULTICONFIG_DONE) # Xcode, Visual Studio, ...
  set(MULTICONFIG_DONE TRUE)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    list(APPEND CMAKE_CONFIGURATION_TYPES Asan Ubsan)

    # This is needed because user can set CMAKE_CONFIGURATION_TYPES
    # at the cmake command line
    list(REMOVE_DUPLICATES CMAKE_CONFIGURATION_TYPES)

    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING
        "Semicolon separated list of supported configuration types." FORCE)
  endif()
endif()

# Notes for Windows with Visual Studio:
#
# - Address Sanitizer (Asan) currently is under experimental stage
# - Undefined Behavior Sanitizer (Ubsan) is not supported at this time
#
# Therefore, we have disabled support for these build types now, but
# this support may be enabled in the future.
#
# Notes about '-fno-omit-frame-pointer' flag:
#
# Frame pointer omission does make debugging significantly harder. Local
# vars are harder to locate and stack traces are much harder to reconstruct
# without a frame pointer to help out. Also, accessing parameters can get more
# expensive since they are far away from the top of the stack and may require
# more expensive addressing modes.
#
# The '-fno-omit-frame-pointer' option direct the compiler to generate code
# that maintains and uses stack frame pointer for all functions so that a
# debugger can still produce a stack backtrace even with optimizations flags
# (eg '-O1', '-O2', etc).
#
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  # Setup Asan flags
  set(CMAKE_C_FLAGS_ASAN
      "${CMAKE_C_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer" CACHE STRING
      "Flags used by the C compiler for Asan build type or configuration." FORCE)

  set(CMAKE_CXX_FLAGS_ASAN
      "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer" CACHE STRING
      "Flags used by the C++ compiler for Asan build type or configuration." FORCE)

  set(CMAKE_EXE_LINKER_FLAGS_ASAN
      "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=address" CACHE STRING
      "Linker flags to be used to create executables for Asan build type." FORCE)

  set(CMAKE_SHARED_LINKER_FLAGS_ASAN
      "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=address" CACHE STRING
      "Linker flags to be used to create shared libraries for Asan build type." FORCE)

  set(CMAKE_STATIC_LINKER_FLAGS_ASAN
      "${CMAKE_STATIC_LINKER_FLAGS_DEBUG} -fsanitize=address" CACHE STRING
      "Linker flags to be used to create static libraries for Asan build type." FORCE)

  # Setup Ubsan flags
  set(CMAKE_C_FLAGS_UBSAN
      "${CMAKE_C_FLAGS_DEBUG} -fsanitize=undefined -fno-omit-frame-pointer" CACHE STRING
      "Flags used by the C compiler for Ubsan build type or configuration." FORCE)

  set(CMAKE_CXX_FLAGS_UBSAN
      "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=undefined -fno-omit-frame-pointer" CACHE STRING
      "Flags used by the C++ compiler for Ubsan build type or configuration." FORCE)

  set(CMAKE_EXE_LINKER_FLAGS_UBSAN
      "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=undefined" CACHE STRING
      "Linker flags to be used to create executables for Ubsan build type." FORCE)

  set(CMAKE_SHARED_LINKER_FLAGS_UBSAN
      "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=undefined" CACHE STRING
      "Linker flags to be used to create shared libraries for Ubsan build type." FORCE)

  set(CMAKE_STATIC_LINKER_FLAGS_UBSAN
      "${CMAKE_STATIC_LINKER_FLAGS_DEBUG} -fsanitize=undefined" CACHE STRING
      "Linker flags to be used to create static libraries for Ubsan build type." FORCE)
endif()
