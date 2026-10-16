cmake_minimum_required(VERSION 3.22.1)
project(fasteink C)
set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../core)
add_library(fasteink SHARED fasteink_jni.c ${CORE}/src/fasteink.c ${CORE}/src/ebc_driver.c ${CORE}/src/rotate.c)
target_include_directories(fasteink PRIVATE ${CORE}/include)
# Keep pen preview and pixel-transfer loops fast in tablet debug builds too.
target_compile_options(fasteink PRIVATE -O2 -Wall -Wextra -Werror)
target_link_libraries(fasteink jnigraphics)
