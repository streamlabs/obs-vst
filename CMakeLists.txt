if(DISABLE_UI)
	message(STATUS "UI disabled,so vst plugin disabled")
	return()
endif()

if(DISABLE_VST)
	message(STATUS "obs-vst is disabled")
	return()
endif()

cmake_minimum_required(VERSION 3.24...3.25)

project(obs-vst C CXX)

if(WIN32)
	include(common.cmake)

	# Proto file
	get_filename_component(papi_proto "obs_vst_api.proto" ABSOLUTE)
	get_filename_component(papi_proto_path "${papi_proto}" PATH)

	# Generated sources
	set(papi_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/obs_vst_api.pb.cc")
	set(papi_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/obs_vst_api.pb.h")
	set(papi_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/obs_vst_api.grpc.pb.cc")
	set(papi_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/obs_vst_api.grpc.pb.h")

	add_custom_command(
		OUTPUT "${papi_proto_srcs}" "${papi_proto_hdrs}" "${papi_grpc_srcs}" "${papi_grpc_hdrs}"
		COMMAND ${_PROTOBUF_PROTOC}
		ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
		--cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
		-I "${papi_proto_path}"
		--plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
		"${papi_proto}"
		DEPENDS "${papi_proto}")

	include_directories("${CMAKE_CURRENT_BINARY_DIR}")

	# papi_grpc_proto
	add_library(papi_grpc_proto
		${papi_grpc_srcs}
		${papi_grpc_hdrs}
		${papi_proto_srcs}
		${papi_proto_hdrs})
	target_link_libraries(papi_grpc_proto
		${_REFLECTION}
		${_GRPC_GRPCPP}
		${_PROTOBUF_LIBPROTOBUF})
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}")

option(VST_USE_BUNDLED_HEADERS "Build with Bundled Headers" ON)

if(VST_USE_BUNDLED_HEADERS)
	message(STATUS "Using the bundled VST header.")
	include_directories(vst_header)
	set(vst_HEADER
		vst_header/aeffectx.h)
else()
	set(VST_INCLUDE_DIR "" CACHE PATH
		"Path to Steinburg headers (e.g. C:/VST3 SDK/pluginterfaces/vst2.x)")

	message(WARNING "You should only use the Steinburg headers for debugging or local
	 builds. It is illegal to distribute the Steinburg headers with anything, and
	 possibly against the GPL to distribute the binaries from the resultant compile.")
	include_directories(${VST_INCLUDE_DIR})
	set(vst_HEADER
		${VST_INCLUDE_DIR}/aeffectx.h)
endif()

if(APPLE)
	find_library(FOUNDATION_FRAMEWORK Foundation)
	find_library(COCOA_FRAMEWORK Cocoa)
endif(APPLE)

set(obs-vst_SOURCES
	obs-vst.cpp
	VSTPlugin.cpp
	grpc_vst_communicatorClient.cpp)

if(APPLE)
	list(APPEND obs-vst_SOURCES mac/VSTPlugin-osx.mm)

elseif(WIN32)
	list(APPEND obs-vst_SOURCES
		win/VSTPlugin-win.cpp
		${papi_proto_srcs}
		${papi_grpc_srcs})

elseif("${CMAKE_SYSTEM_NAME}" MATCHES "Linux")
	list(APPEND obs-vst_SOURCES linux/VSTPlugin-linux.cpp)
endif()

list(APPEND obs-vst_HEADERS
	headers/VSTPlugin.h
	headers/grpc_vst_communicatorClient.h)

add_library(obs-vst MODULE)
add_library(OBS::vst ALIAS obs-vst)

target_sources(obs-vst PRIVATE
	${obs-vst_SOURCES}
	${obs-vst_HEADERS}
	${vst-HEADER})

# target_link_libraries(obs-vst PRIVATE OBS::libobs)
set_target_properties(obs-vst PROPERTIES FOLDER "plugins")

target_link_libraries(obs-vst PRIVATE OBS::libobs)

if(APPLE)
	target_link_libraries(obs-vst PRIVATE ${COCOA_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
endif()

if(WIN32)
	target_link_libraries(obs-vst PRIVATE ${_REFLECTION} ${_GRPC_GRPCPP} ${_PROTOBUF_LIBPROTOBUF})
endif()

# cmake-format: off
set_target_properties_obs(obs-vst PROPERTIES FOLDER plugins/obs-vst PREFIX "")

# cmake-format: on
target_compile_features(obs-vst PRIVATE cxx_std_17)

if(WIN32)
	project(win-streamlabs-vst C CXX)

	set(win-streamlabs-vst_SOURCES
		WIN32 proxy/win-streamlabs-vst.cpp
		proxy/VstWindow.cpp
		proxy/VstModule.cpp
		${papi_proto_srcs}
		${papi_grpc_srcs}
	)

	add_executable(win-streamlabs-vst
		${win-streamlabs-vst_SOURCES}
		${papi_proto_srcs}
		${papi_grpc_srcs}
	)

	target_link_libraries(win-streamlabs-vst
		papi_grpc_proto
		${_REFLECTION}
		${_GRPC_GRPCPP}
		${_PROTOBUF_LIBPROTOBUF}
	)

	# ###########################
	# Setup crashpad libraries
	# ###########################
	if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
		include(ExternalProject)
		include(DownloadProject.cmake)

		# Select the latest crashpad release files
		if(WIN32)
			download_project(
				PROJ crashpad
				URL https://obsstudionodes3.streamlabs.com/crashpad/crashpad-release-win-bd479a12-dec-2022-x86_64.tar.gz
				UPDATE_DISCONNECTED 1
			)
		elseif(APPLE)
			download_project(
				PROJ crashpad
				URL https://obsstudionodes3.streamlabs.com/crashpad/crashpad-release-osx-bd479a12-jan-2023-x86_64.zip
				UPDATE_DISCONNECTED 1
			)
		endif()

		ExternalProject_Add(
			crashpad_ep
			DOWNLOAD_COMMAND ""
			SOURCE_DIR "${crashpad_SOURCE_DIR}"
			INSTALL_COMMAND ""
			BUILD_COMMAND ""
			CONFIGURE_COMMAND ""
			BUILD_BYPRODUCTS
			"<SOURCE_DIR>/lib/${CMAKE_STATIC_LIBRARY_PREFIX}base${CMAKE_STATIC_LIBRARY_SUFFIX}"
			"<SOURCE_DIR>/lib/${CMAKE_STATIC_LIBRARY_PREFIX}util${CMAKE_STATIC_LIBRARY_SUFFIX}"
			"<SOURCE_DIR>/lib/${CMAKE_STATIC_LIBRARY_PREFIX}client${CMAKE_STATIC_LIBRARY_SUFFIX}"
		)

		# Setup imported static libraries for crashpad
		add_library(crashpad_util STATIC IMPORTED)
		add_library(crashpad_base STATIC IMPORTED)
		add_library(crashpad_client STATIC IMPORTED)
		add_library(crashpad_common STATIC IMPORTED)

		ExternalProject_Get_Property(crashpad_ep source_dir)

		# Define imported library locations
		set_property(TARGET crashpad_base PROPERTY IMPORTED_LOCATION
			"${source_dir}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}base${CMAKE_STATIC_LIBRARY_SUFFIX}")

		set_property(TARGET crashpad_util PROPERTY IMPORTED_LOCATION
			"${source_dir}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}util${CMAKE_STATIC_LIBRARY_SUFFIX}")

		set_property(TARGET crashpad_common PROPERTY IMPORTED_LOCATION
			"${source_dir}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}common${CMAKE_STATIC_LIBRARY_SUFFIX}")
		
		set_property(TARGET crashpad_client PROPERTY IMPORTED_LOCATION
			"${source_dir}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}client${CMAKE_STATIC_LIBRARY_SUFFIX}")

		# Include directories for crashpad libraries
		target_include_directories(
			crashpad_base
			INTERFACE "${source_dir}/include/third_party/mini_chromium/mini_chromium"
		)

		target_include_directories(
			crashpad_client
			INTERFACE "${source_dir}/include"
		)

		# Aggregate crashpad libraries into a single interface target
		add_library(crashpad INTERFACE)

		target_link_libraries(
			crashpad
			INTERFACE
			crashpad_base
			crashpad_client
			crashpad_util
			crashpad_common
		)

		target_link_libraries(crashpad_util INTERFACE crashpad_client)

		# Add dependencies for correct build order
		add_dependencies(crashpad_base crashpad_ep)
		add_dependencies(crashpad_client crashpad_base)
		add_dependencies(crashpad_util crashpad_client)
		add_dependencies(crashpad_common crashpad_util)

		# Link crashpad to win-streamlabs-vst
		target_link_libraries(win-streamlabs-vst crashpad)
	endif()

	target_compile_features(win-streamlabs-vst PRIVATE cxx_std_17)
endif()

if(WIN32)
	set_target_properties(win-streamlabs-vst PROPERTIES LINK_FLAGS "/ignore:4099")
	set_target_properties(obs-vst PROPERTIES LINK_FLAGS "/ignore:4099")

	set_target_properties_obs(win-streamlabs-vst PROPERTIES FOLDER plugins/obs-vst PREFIX "")
endif(WIN32)
