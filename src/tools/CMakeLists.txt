function(llme_add_tool name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE llme_io CLI11::CLI11 nlohmann_json::nlohmann_json)
  llme_target_defaults(${name})
endfunction()

llme_add_tool(llme-gen llme_gen.cpp)
llme_add_tool(llme-run llme_run.cpp)
llme_add_tool(llme-diff llme_diff.cpp)
