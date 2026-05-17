cmake_minimum_required(VERSION 3.15)
project(XInputEmu)

set(CMAKE_CXX_STANDARD 20)

# 1. Buscar SDL2 (vcpkg lo maneja excelente)
find_package(SDL2 CONFIG REQUIRED)

# 2. Configurar ViGEmClient usando la carpeta que descargaremos en la nube
set(VIGEM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ViGEmClient")
include_directories("${VIGEM_DIR}/include")

add_executable(XInputEmu 
    main.cpp 
    "${VIGEM_DIR}/src/ViGEmClient.cpp"
)

# 3. Enlazar todo
target_link_libraries(XInputEmu PRIVATE SDL2::SDL2 setupapi.lib)
