cmake_minimum_required(VERSION 3.10)

project(js)

add_library(quickjs SHARED quickjs/quickjs.c quickjs/libregexp.c quickjs/libunicode.c quickjs/cutils.c quickjs/libbf.c)
file(STRINGS "quickjs/VERSION" QUICKJS_VERSION)
target_compile_definitions(quickjs PUBLIC -D_GNU_SOURCE PUBLIC -DCONFIG_VERSION=\"${QUICKJS_VERSION}\" -DCONFIG_BIGNUM -DDUMP_LEAKS)
target_link_libraries(quickjs m)
target_include_directories(quickjs PUBLIC quickjs)

add_executable(js src/main.c src/console.c src/eventloop.c src/xmlhttprequest.c)
target_link_libraries(js pthread quickjs readline ncursesw curl)
