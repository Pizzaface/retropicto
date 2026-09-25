cmake_minimum_required(VERSION 3.16)
project(pictochat VERSION 0.1.0 LANGUAGES C)
add_library(pictochat src/session.c src/frame.c src/room.c)
target_include_directories(pictochat PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(pictochat PUBLIC c_std_11)
set_target_properties(pictochat PROPERTIES C_EXTENSIONS OFF)
install(TARGETS pictochat EXPORT pictochatTargets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT pictochatTargets NAMESPACE pictochat:: DESTINATION lib/cmake/pictochat)
install(FILES pictochatConfig.cmake DESTINATION lib/cmake/pictochat)
