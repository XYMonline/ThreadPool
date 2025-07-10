cmake_minimum_required (VERSION 3.12)

# 如果支持，请为 MSVC 编译器启用热重载。
if (POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

set (PROJECT_NAME "ThreadPoolTest")

project (${PROJECT_NAME})

add_executable(${PROJECT_NAME} 
    thread_pool_tests.cpp
)

target_compile_options(${PROJECT_NAME}
	PRIVATE 
	$<$<CXX_COMPILER_ID:MSVC>:/bigobj> 
	$<$<CXX_COMPILER_ID:GNU>:-Wa,-mbig-obj>) 

if(MSVC)
    target_link_libraries(${PROJECT_NAME}
        PRIVATE 
        gtest 
        gtest_main
)
else()
    target_link_libraries(${PROJECT_NAME}
        PRIVATE 
        gtest 
        gtest_main
        pthread  # 在 Linux 上需要
)
endif()

include(GoogleTest)
gtest_discover_tests(${PROJECT_NAME})

if (CMAKE_VERSION VERSION_GREATER 3.12)
    set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
endif()