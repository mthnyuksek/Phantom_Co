cmake_minimum_required(VERSION 3.20)
project(phantom_co LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The web UI, the Vault template, the skill templates and the WebView2 loader are embedded
# into the executable so the program is one self-contained file.
set(UI_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/../Web_Ui)
set(VAULT_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../Vault_Template)
set(SKILLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../Skill_Templates)
set(LOADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Third_Party/Web_View2/Bin)
set(EMBED_CPP  ${CMAKE_CURRENT_BINARY_DIR}/embedded_files.cpp)

file(GLOB_RECURSE EMBED_INPUTS CONFIGURE_DEPENDS ${UI_DIR}/* ${VAULT_DIR}/* ${SKILLS_DIR}/* ${LOADER_DIR}/*)
add_custom_command(
  OUTPUT ${EMBED_CPP}
  COMMAND ${CMAKE_COMMAND} -DOUT=${EMBED_CPP} -DUI_DIR=${UI_DIR} -DVAULT_DIR=${VAULT_DIR} -DSKILLS_DIR=${SKILLS_DIR} -DLOADER_DIR=${LOADER_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/Cmake/embed.cmake
  DEPENDS ${EMBED_INPUTS} ${CMAKE_CURRENT_SOURCE_DIR}/Cmake/embed.cmake
  COMMENT "Embedding Web_Ui, Vault_Template, Skill_Templates and the WebView2 loader"
  VERBATIM)

add_executable(phantom_co WIN32
  Source/main.cpp
  Source/util.cpp
  Source/md.cpp
  Source/auth.cpp
  Source/store.cpp
  Source/prompt.cpp
  Source/process.cpp
  Source/providers.cpp
  Source/runner.cpp
  Source/worker_hub.cpp
  Source/worker.cpp
  Source/http_client.cpp
  Source/tunnel.cpp
  Source/server.cpp
  Source/web_view_host.cpp
  Resources/phantom_co.rc
  ${EMBED_CPP})

set_source_files_properties(Resources/phantom_co.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Resources/phantom_co.ico;${CMAKE_CURRENT_SOURCE_DIR}/Resources/phantom_co.manifest")
target_include_directories(phantom_co PRIVATE Source Third_Party Third_Party/Web_View2/Include)
target_compile_definitions(phantom_co PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(phantom_co PRIVATE ws2_32 shell32 ole32 bcrypt iphlpapi dwmapi winhttp)

if(MINGW)
  target_compile_options(phantom_co PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_options(phantom_co PRIVATE -static)
endif()
