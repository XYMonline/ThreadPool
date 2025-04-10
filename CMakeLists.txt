cmake_minimum_required (VERSION 3.12)

# 如果支持，请为 MSVC 编译器启用热重载。
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

set (PROJECT_NAME "ThreadPool")

project (${PROJECT_NAME})

option(BUILD_TEST "Enable test compilation" ON)
option(BUILD_EXAMPLE "Enable example compilation" ON)
option(ENABLE_DEBUG "Enable debug logging" OFF)

add_library(${PROJECT_NAME} INTERFACE)
target_include_directories(${PROJECT_NAME} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

if (BUILD_EXAMPLE)
	add_subdirectory(example)
endif()

if (BUILD_TEST) 
  message(STATUS "Test compilation enabled")
  target_compile_definitions(${PROJECT_NAME} INTERFACE TEST_INTERFACE)

  include(FetchContent)
  FetchContent_Declare(
	googletest
	GIT_REPOSITORY https://github.com/google/googletest.git
	GIT_TAG v1.14.0
  )

  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_subdirectory(test)
endif()

if (ENABLE_DEBUG)
  message(STATUS "Debug option enabled. Defining DEBUG macro.")
  target_compile_definitions(${PROJECT_NAME} INTERFACE THREADPOOL_DEBUG)
 endif()

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
endif()


