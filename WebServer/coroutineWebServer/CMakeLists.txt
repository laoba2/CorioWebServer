set(COROUTINE_SRCS
    AsyncLogger.cpp
    CoroutineMain.cpp
    HttpServer.cpp
    MemoryResource.cpp
    UringScheduler.cpp
)

find_path(LIBURING_INCLUDE_DIR liburing.h)
find_library(LIBURING_LIBRARY uring)

if(LIBURING_INCLUDE_DIR AND LIBURING_LIBRARY)
    add_executable(CoroutineWebServer ${COROUTINE_SRCS})
    target_include_directories(CoroutineWebServer PRIVATE
        ${LIBURING_INCLUDE_DIR}
        ${PROJECT_SOURCE_DIR}/base
    )
    target_compile_features(CoroutineWebServer PRIVATE cxx_std_20)
    target_compile_options(CoroutineWebServer PRIVATE -std=c++20)
    target_link_libraries(CoroutineWebServer PRIVATE ${LIBURING_LIBRARY} pthread rt)
else()
    message(WARNING "liburing not found; CoroutineWebServer target is disabled")
endif()
