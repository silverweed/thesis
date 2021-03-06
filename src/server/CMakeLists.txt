cmake_minimum_required(VERSION 2.8 FATAL_ERROR)
project(server)
set(PROJECT_NAME server)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")

set(MAIN_FILE server_main.cpp)

include_directories(.)
include_directories(..)
include_directories(../third_party)
include_directories(../common)

file(GLOB SRC ${MAIN_FILE}
	./*.cpp
)

add_executable(${PROJECT_NAME} ${SRC})
set_target_properties(${PROJECT_NAME} PROPERTIES
	CXX_STANDARD 14
	CXX_STANDARD_REQUIRED YES
	CXX_EXTENSIONS NO
	LINKER_LANGUAGE CXX
)

##### FIND PACKAGES

find_package(Vulkan REQUIRED)
include_directories(${Vulkan_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} ${Vulkan_LIBRARIES})

set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)
include_directories(${Threads_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} ${CMAKE_THREAD_LIBS_INIT})

find_package(GLM REQUIRED)
include_directories(${GLM_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} ${GLM_LIBRARIES})
add_definitions(-DGLM_FORCE_CXX14)

find_package(Assimp REQUIRED)
include_directories(${ASSIMP_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} ${ASSIMP_LIBRARIES})



#####################

target_link_libraries(${PROJECT_NAME} common)

do_build()
