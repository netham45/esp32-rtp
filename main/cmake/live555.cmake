set(LIVE555_SOURCE_DIRS
    UsageEnvironment
    BasicUsageEnvironment
    groupsock
    liveMedia
)

set(LIVE555_SOURCES "")
foreach(LIVE555_SUBDIR ${LIVE555_SOURCE_DIRS})
    file(GLOB LIVE555_${LIVE555_SUBDIR}_SRCS
         CONFIGURE_DEPENDS
         "${LIVE555_ROOT}/${LIVE555_SUBDIR}/*.cpp"
         "${LIVE555_ROOT}/${LIVE555_SUBDIR}/*.c")
    list(APPEND LIVE555_SOURCES ${LIVE555_${LIVE555_SUBDIR}_SRCS})
endforeach()

if(NOT LIVE555_SOURCES)
    message(FATAL_ERROR "live555 sources not found under ${LIVE555_ROOT}")
endif()

add_library(live555 STATIC ${LIVE555_SOURCES})

set(LIVE555_PUBLIC_INCLUDES
    "${LIVE555_ROOT}/UsageEnvironment"
    "${LIVE555_ROOT}/UsageEnvironment/include"
    "${LIVE555_ROOT}/BasicUsageEnvironment"
    "${LIVE555_ROOT}/BasicUsageEnvironment/include"
    "${LIVE555_ROOT}/groupsock"
    "${LIVE555_ROOT}/groupsock/include"
    "${LIVE555_ROOT}/liveMedia"
    "${LIVE555_ROOT}/liveMedia/include"
)

target_include_directories(live555 PUBLIC ${LIVE555_PUBLIC_INCLUDES})

set(LIVE555_PORT_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../ports/live555_compat")
if(EXISTS "${LIVE555_PORT_INCLUDE_DIR}")
    target_include_directories(live555 BEFORE PRIVATE "${LIVE555_PORT_INCLUDE_DIR}")
endif()

target_compile_features(live555 PUBLIC cxx_std_17)

target_compile_definitions(live555 PUBLIC
    LOCALE_NOT_USED=1
    NEWLOCALE_NOT_USED=1
    NO_OPENSSL=1
    NO_GETIFADDRS=1
    SOCKLEN_T=socklen_t
    _LARGEFILE_SOURCE=1
    _FILE_OFFSET_BITS=64
    ALLOW_RTSP_SERVER_PORT_REUSE=1
)

target_compile_options(live555 PRIVATE
    -Wno-unused-const-variable
    -Wno-array-bounds
    -Wno-array-compare
    -Wno-error=array-bounds
    -Wno-error=array-compare
    -Wno-type-limits
    -Wno-format
)

target_compile_options(live555 PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
)

idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
if(lwip_lib AND TARGET ${lwip_lib})
    target_link_libraries(live555 PUBLIC ${lwip_lib})
endif()
