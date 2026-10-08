add_library(qbspnet STATIC net.cc ../include/qbspserver/net.hh)
if (WIN32)
	target_link_libraries(qbspnet PUBLIC ws2_32)
endif()

add_executable(qbspserver server.cc)
target_link_libraries(qbspserver PRIVATE libqbsp qbspnet ${CMAKE_THREAD_LIBS_INIT} fmt::fmt)

add_executable(qbspreceive receive.cc)
target_link_libraries(qbspreceive PRIVATE qbspnet)

# HACK: copy .dll dependencies
add_custom_command(TARGET qbspserver POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:TBB::tbb>" "$<TARGET_FILE_DIR:qbspserver>"
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:TBB::tbbmalloc>" "$<TARGET_FILE_DIR:qbspserver>"
                   )
copy_mingw_dlls(qbspserver)
copy_mingw_dlls(qbspreceive)
add_loader_path_to_rpath(qbspserver)

install(TARGETS qbspserver qbspreceive RUNTIME DESTINATION .)
