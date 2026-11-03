# WebAssembly build of the engine for the browser demo. Configure with:
#   emcmake cmake -S . -B build-wasm -DLOB_BUILD_WASM=ON -DLOB_BUILD_TESTS=OFF -DLOB_BUILD_TOOLS=OFF
# Output: build-wasm/wasm_out/lob.js + lob.wasm
add_executable(lob_wasm wasm_api.cpp)
target_link_libraries(lob_wasm PRIVATE lob)
set_target_properties(lob_wasm PROPERTIES
  OUTPUT_NAME lob
  SUFFIX ".js"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/wasm_out)
target_link_options(lob_wasm PRIVATE
  -O3
  --no-entry
  -sMODULARIZE=1
  -sEXPORT_NAME=createLob
  -sALLOW_MEMORY_GROWTH=1
  -sENVIRONMENT=web,node
  -sFILESYSTEM=0)
