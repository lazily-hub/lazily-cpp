if(NOT DEFINED LAZILY_BUILD_DIR OR NOT DEFINED LAZILY_PACKAGE_DIR)
  message(FATAL_ERROR "LAZILY_BUILD_DIR and LAZILY_PACKAGE_DIR are required")
endif()

file(REMOVE_RECURSE "${LAZILY_PACKAGE_DIR}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${LAZILY_BUILD_DIR}" --prefix "${LAZILY_PACKAGE_DIR}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_stdout
  ERROR_VARIABLE install_stderr
)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR
    "cmake --install failed (${install_result})\n${install_stdout}\n${install_stderr}")
endif()

set(required_entries
  "include/lazily/core.hpp"
  "include/lazily/cell.hpp"
  "lib/cmake/lazily/lazilyConfig.cmake"
  "lib/cmake/lazily/lazilyConfigVersion.cmake"
  "lib/cmake/lazily/lazilyTargets.cmake"
  "share/doc/lazily/LICENSE"
  "share/doc/lazily/NOTICE"
  "share/doc/lazily/README.md"
)

foreach(entry IN LISTS required_entries)
  if(NOT EXISTS "${LAZILY_PACKAGE_DIR}/${entry}")
    message(FATAL_ERROR "installed package is missing ${entry}")
  endif()
endforeach()
