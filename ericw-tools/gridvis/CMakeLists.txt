add_executable(gridvis main.cc gridvis.cc ../include/gridvis/gridvis.hh)
target_link_libraries(gridvis PRIVATE common ${CMAKE_THREAD_LIBS_INIT} fmt::fmt)

# HACK: copy .dll dependencies
add_custom_command(TARGET gridvis POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:TBB::tbb>" "$<TARGET_FILE_DIR:gridvis>"
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:TBB::tbbmalloc>" "$<TARGET_FILE_DIR:gridvis>"
                   )
copy_mingw_dlls(gridvis)
add_loader_path_to_rpath(gridvis)

install(TARGETS gridvis RUNTIME DESTINATION .)
