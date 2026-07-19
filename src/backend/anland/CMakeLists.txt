# src/backend/anland/CMakeLists.txt

target_sources(aquamarine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/AnlandBackend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AnlandOutput.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AnlandBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AnlandAllocator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AnlandPointer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AnlandKeyboard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AnlandTouch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/display_producer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/socket_utils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/anland_audio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/anland_camera.c
)

target_include_directories(aquamarine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(aquamarine PRIVATE HAS_ANLAND_BACKEND)

if(PIPEWIRE_FOUND)
    target_compile_definitions(aquamarine PRIVATE HAVE_PIPEWIRE)
    target_link_libraries(aquamarine PRIVATE ${PIPEWIRE_LIBRARIES})
    target_include_directories(aquamarine PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
endif()