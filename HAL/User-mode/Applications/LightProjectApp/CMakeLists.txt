project(LightProjectApp C)
add_executable(LightProjectApp main.c)

target_include_directories(LightProjectApp PRIVATE 
    ${CMAKE_SOURCE_DIR}/Shared/common
    ${CMAKE_SOURCE_DIR}/Shared/kernel
    ${CMAKE_SOURCE_DIR}/CommonLib/inc
    ${CMAKE_SOURCE_DIR}/User-mode/Utils/UsermodeLibrary/inc
    ${CMAKE_SOURCE_DIR}/CompilerAbstraction/inc
)

target_link_libraries(LightProjectApp UsermodeLibrary CommonLibNoLocks CompilerAbstraction)
target_compile_options(LightProjectApp PRIVATE -fno-stack-protector -fno-builtin -ffreestanding)
