# Create an INTERFACE library for our C module.
add_library(usermod_twai  INTERFACE)

# Add our source files to the lib
target_sources(usermod_twai INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/twai_mod.c
    ${CMAKE_CURRENT_LIST_DIR}/twai_message.c
)

# Add the current directory as an include directory.
target_include_directories(usermod_twai  INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(usermod_twai  INTERFACE)

# Link our INTERFACE library to the usermod target.
target_link_libraries(usermod INTERFACE usermod_twai)