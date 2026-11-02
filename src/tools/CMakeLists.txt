add_library(llme_tool_support STATIC experiment_log.cpp)
target_include_directories(llme_tool_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(llme_tool_support
  PUBLIC llme_io llme_workload llme_testing CLI11::CLI11 nlohmann_json::nlohmann_json
         tomlplusplus::tomlplusplus)
llme_target_defaults(llme_tool_support)

function(llme_add_tool name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE llme_tool_support)
  llme_target_defaults(${name})
endfunction()

llme_add_tool(llme-gen llme_gen.cpp)
llme_add_tool(llme-run llme_run.cpp)
llme_add_tool(llme-diff llme_diff.cpp)
