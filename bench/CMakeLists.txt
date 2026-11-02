add_executable(llme-gbench gbench_components.cpp)
target_link_libraries(llme-gbench PRIVATE llme_io llme_core benchmark::benchmark)
llme_target_defaults(llme-gbench)
