option(KYTY_ENABLE_DIRECTSTORAGE "Build optional Windows DirectStorage raw file transport" OFF)
set(KYTY_DIRECTSTORAGE_SDK "" CACHE PATH "Extracted Microsoft.Direct3D.DirectStorage NuGet package")

function(kyty_configure_directstorage target)
	if(NOT KYTY_ENABLE_DIRECTSTORAGE)
		return()
	endif()
	if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
		message(FATAL_ERROR "DirectStorage transport requires Windows x64")
	endif()
	find_path(KYTY_DIRECTSTORAGE_INCLUDE dstorage.h
		PATHS "${KYTY_DIRECTSTORAGE_SDK}/native/include" NO_DEFAULT_PATH REQUIRED)
	foreach(runtime dstorage.dll dstoragecore.dll)
		if(NOT EXISTS "${KYTY_DIRECTSTORAGE_SDK}/native/bin/x64/${runtime}")
			message(FATAL_ERROR "DirectStorage runtime missing: ${runtime}")
		endif()
	endforeach()
	target_compile_definitions(${target} PRIVATE KYTY_ENABLE_DIRECTSTORAGE=1)
	target_include_directories(${target} SYSTEM PRIVATE "${KYTY_DIRECTSTORAGE_INCLUDE}")
endfunction()

function(kyty_stage_directstorage target)
	if(KYTY_ENABLE_DIRECTSTORAGE)
		foreach(runtime dstorage.dll dstoragecore.dll)
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
					"${KYTY_DIRECTSTORAGE_SDK}/native/bin/x64/${runtime}"
					"$<TARGET_FILE_DIR:${target}>/${runtime}" VERBATIM)
		endforeach()
	endif()
endfunction()
