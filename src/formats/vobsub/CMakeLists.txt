add_subdirectory(mplayer)

set(formats_vobsub_SRCS
	${CMAKE_CURRENT_SOURCE_DIR}/vobsubinputformat.h
	CACHE INTERNAL EXPORTEDVARIABLE
)

set(formats_vobsub_LIBS
	vobsubmplayer
	CACHE INTERNAL EXPORTEDVARIABLE
)
