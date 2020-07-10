cmake_minimum_required (VERSION 3.10.0 FATAL_ERROR)
cmake_policy(VERSION 3.10.0)

SET(CMAKE_CXX_STANDARD 11)
SET(CMAKE_C_STANDARD 11)

# -----------------------------------------------------------------------------
# Set up installation
# Install to correct subdirectories on all platforms
include(GNUInstallDirs)
if(APPLE)
  # See: https://cmake.org/Wiki/CMake_RPATH_handling
  set(CMAKE_MACOSX_RPATH TRUE)
  set(CMAKE_SKIP_BUILD_RPATH FALSE)
  set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
  set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
endif(APPLE)

# -----------------------------------------------------------------------------
# Figure out what to build
# By default, build everything
#if(NOT DEFINED BUILD_CLIENT)
#  set(BUILD_CLIENT True)
#endif()
if(NOT DEFINED BUILD_PROVISIONER)
  set(BUILD_PROVISIONER True)
endif()
if(NOT DEFINED BUILD_SERVER)
  set(BUILD_SERVER True)
endif()
# if(${BUILD_SERVER})
#   if(NOT DEFINED BUILD_SERVER_TESTS)
#     set(BUILD_SERVER_TESTS True)
#   endif()
# else()
#   if(BUILD_SERVER_TESTS)
#     message(FATAL_ERROR "Building the server tests requires building the server")
#   endif()
#   set(BUILD_SERVER_TESTS False)
# endif()
set(BUILD_SERVER_TESTS False)

if(${BUILD_SERVER_TESTS} AND NOT ${BUILD_SERVER})
	message(FATAL_ERROR "Building the server tests requires building the server")
endif()
#if(BUILD_CLIENT)
#  message("Will build client")
#endif()
if(BUILD_PROVISIONER)
  message("Will build account provisioner")
endif()
if(BUILD_SERVER)
  message("Will build server")
  if(BUILD_SERVER_TESTS)
    message("Will build server tests")
  endif()
endif()
#if(NOT ${BUILD_CLIENT} AND NOT ${BUILD_SERVER})
if(NOT ${BUILD_SERVER})
  message(WARNING "nothing will be built. Is this what you want?")
endif()

# Cache settings
#set(BUILD_CLIENT ${BUILD_CLIENT} CACHE BOOL "Build the client")
set(BUILD_SERVER ${BUILD_SERVER} CACHE BOOL "Build the server")
set(BUILD_SERVER_TESTS ${BUILD_SERVER_TESTS} CACHE BOOL "Build the server tests")

# -----------------------------------------------------------------------------
# Look for dependencies
SET(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/Packages)
# These are needed by all components
FIND_PACKAGE(ZLIB)
FIND_PACKAGE(libcrypto)
FIND_PACKAGE(ssl)
FIND_PACKAGE(CURL)
# These are needed only by the server
if(BUILD_SERVER)
  FIND_PACKAGE(Boost COMPONENTS date_time system)
endif()

# -----------------------------------------------------------------------------
# Version embedding
if(BUILD_SERVER)
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/server_version.h ${CMAKE_BINARY_DIR}/server_version.h_
    COMMAND ${CMAKE_SOURCE_DIR}/cmake/embed_version.sh ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR} server
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
  LIST(APPEND EMBED_VERSION_DEPS ${CMAKE_BINARY_DIR}/server_version.h_)
endif()
#if(BUILD_CLIENT)
#  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/client_version.h ${CMAKE_BINARY_DIR}/client_version.h_
#    COMMAND ${CMAKE_SOURCE_DIR}/cmake/embed_version.sh ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR} client
#    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
#  )
#  LIST(APPEND EMBED_VERSION_DEPS ${CMAKE_BINARY_DIR}/client_version.h_)
#endif()
add_custom_target(embed_version
  DEPENDS ${EMBED_VERSION_DEPS} ${CMAKE_SOURCE_DIR}/cmake/embed_version.sh
)

# -----------------------------------------------------------------------------
# Account provisioner executable
if(BUILD_PROVISIONER)
  LIST(APPEND PROVISIONER_SOURCES
    ${CMAKE_SOURCE_DIR}/src/sync_users.cpp
    ${CMAKE_SOURCE_DIR}/src/Entities.cpp
    ${CMAKE_SOURCE_DIR}/src/HTTPRequests.cpp
    ${CMAKE_SOURCE_DIR}/src/Logging.cpp
    ${CMAKE_SOURCE_DIR}/src/Process.cpp
    ${CMAKE_SOURCE_DIR}/src/Utilities.cpp
  )
  add_executable(sync_users ${PROVISIONER_SOURCES})
  target_include_directories(sync_users
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    ${CURL_INCLUDE_DIRS}
    ${LIBCRYPTO_CFLAGS}
    ${SSL_CFLAGS}
  )
  if(STATIC_PROVISIONER)
    if(APPLE)
      message("Attempting partially static link")
      set(PROVISIONER_LIBRARIES
        ${CURL_LIBRARIES}
        z
        pthread
      )
    else(APPLE) # assume anything which is not Apple is Linux
      message("Attempting fully static link")
      # Try to make sure we have a static curl library
      foreach(LIB IN LISTS CURL_LIBRARIES)
        get_filename_component(LIB_NAME "${LIB}" NAME)
        if(LIB_NAME MATCHES ".so")
          message(STATUS "Need to replace dynamic Curl library ${LIB}")
          get_filename_component(LIB_DIR "${LIB}" DIRECTORY)
          string(REPLACE "\.so.*" ".a" REPLACEMENT_LIB_NAME "$LIB_NAME")
          message(STATUS "Searching for ${REPLACEMENT_LIB_NAME} in ${LIB_DIR}")
          find_library(REPLACEMENT_LIB NAMES ${REPLACEMENT_LIB_NAME} PATHS ${LIB_DIR})
          if(REPLACEMENT_LIB STREQUAL "REPLACEMENT_LIB-NOTFOUND")
            message(FATAL_ERROR "Unable to find a static curl library")
          endif()
          set(LIB "${REPLACEMENT_LIB}")
        endif()
        LIST(APPEND NEW_CURL_LIBRARIES ${LIB})
      endforeach()
      set(CURL_LIBRARIES "${NEW_CURL_LIBRARIES}")
      set(PROVISIONER_LIBRARIES
        -static
        -static-libstdc++
        -static-libgcc
        ${CURL_LIBRARIES}
        nghttp2
        ${SSL_LDFLAGS}
        ${LIBCRYPTO_LDFLAGS}
        ssh2
        z
        pthread
      )
    endif()
  else(STATIC_PROVISIONER)
    set(PROVISIONER_LIBRARIES
      ${CURL_LIBRARIES}
      ${SSL_LDFLAGS}
      ${LIBCRYPTO_LDFLAGS}
      z
      pthread
    )
  endif()

  target_link_libraries(sync_users
    PUBLIC
      ${PROVISIONER_LIBRARIES}
  )
  target_compile_options(sync_users PRIVATE -DRAPIDJSON_HAS_STDSTRING -Os -std=c++11)
  
  LIST(APPEND RPM_SOURCES ${PROVISIONER_SOURCES})

endif()

## -----------------------------------------------------------------------------
## Client executable
#if(BUILD_CLIENT)
#  LIST(APPEND CLIENT_SOURCES
#    ${CMAKE_SOURCE_DIR}/src/client/ci-connect_client.cpp
#    ${CMAKE_SOURCE_DIR}/src/client/Client.cpp
#    ${CMAKE_SOURCE_DIR}/src/client/ClusterRegistration.cpp
#    ${CMAKE_SOURCE_DIR}/src/client/Completion.cpp
#    ${CMAKE_SOURCE_DIR}/src/client/SecretLoading.cpp
#    ${CMAKE_SOURCE_DIR}/src/Archive.cpp
#    ${CMAKE_SOURCE_DIR}/src/FileHandle.cpp
#    ${CMAKE_SOURCE_DIR}/src/FileSystem.cpp
#    ${CMAKE_SOURCE_DIR}/src/HTTPRequests.cpp
#    ${CMAKE_SOURCE_DIR}/src/Process.cpp
#    ${CMAKE_SOURCE_DIR}/src/Utilities.cpp
#  )
#  add_executable(ci-connect ${CLIENT_SOURCES})
#  target_include_directories (ci-connect
#    PUBLIC
#      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
#      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/resources>
#      $<INSTALL_INTERFACE:include>
#      ${CURL_INCLUDE_DIRS}
#      ${LIBCRYPTO_CFLAGS}
#      ${SSL_CFLAGS}
#      $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}>
#  )
#  add_dependencies(ci-connect embed_version)
#  
#  if(STATIC_CLIENT)
#    if(APPLE)
#      message("Attempting partially static link")
#      set(CLIENT_LIBRARIES
#        ${CURL_LIBRARIES}
#        z
#        pthread
#      )
#    else(APPLE) # assume any thing which is not Apple is Linux
#      # static building may only work on alpine with a manually built, statically linked copy of nghttp2 installed
#      message("Attempting fully static link")
#      set(CLIENT_LIBRARIES
#        -static
#        -static-libstdc++
#        -static-libgcc
#        ${CURL_LIBRARIES}
#        nghttp2
#        ${SSL_LDFLAGS}
#        ${LIBCRYPTO_LDFLAGS}
#        ssh2
#        z
#        pthread
#      )
#    endif(APPLE)
#  else(STATIC_CLIENT)
#    set(CLIENT_LIBRARIES
#      ${CURL_LIBRARIES}
#      ${SSL_LDFLAGS}
#      ${LIBCRYPTO_LDFLAGS}
#      z
#      pthread
#    )
#  endif(STATIC_CLIENT)
#  
#  target_link_libraries(ci-connect
#    PUBLIC
#      ${CLIENT_LIBRARIES}
#  )
#  target_compile_options(ci-connect PRIVATE -DRAPIDJSON_HAS_STDSTRING -Os -std=c++11)
#  install(TARGETS ci-connect RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
#  
#  LIST(APPEND RPM_SOURCES ${CLIENT_SOURCES})
#
#  # TODO: uninstall
#endif(BUILD_CLIENT)

# -----------------------------------------------------------------------------
# Main executable
if(BUILD_SERVER)
  LIST(APPEND SERVER_SOURCES
    ${CMAKE_SOURCE_DIR}/src/ciconnect_service.cpp
    ${CMAKE_SOURCE_DIR}/src/Entities.cpp
    ${CMAKE_SOURCE_DIR}/src/PersistentStore.cpp
    ${CMAKE_SOURCE_DIR}/src/Utilities.cpp
    ${CMAKE_SOURCE_DIR}/src/ServerUtilities.cpp
    ${CMAKE_SOURCE_DIR}/src/UserCommands.cpp
    ${CMAKE_SOURCE_DIR}/src/GroupCommands.cpp
    ${CMAKE_SOURCE_DIR}/src/VersionCommands.cpp
    ${CMAKE_SOURCE_DIR}/src/HTTPRequests.cpp
    ${CMAKE_SOURCE_DIR}/src/Logging.cpp
#   
#     ${CMAKE_SOURCE_DIR}/src/Archive.cpp
#     ${CMAKE_SOURCE_DIR}/src/FileHandle.cpp
#     ${CMAKE_SOURCE_DIR}/src/FileSystem.cpp
#     ${CMAKE_SOURCE_DIR}/src/Process.cpp
  )
  
  SET(CONNECT_SERVER_COMPILE_OPTIONS 
    ${CURL_CFLAGS}
    ${LIBCRYPTO_CFLAGS}
    ${SSL_CFLAGS}
    -DRAPIDJSON_HAS_STDSTRING
    -DCONNECT_SERVER
    -O2
    -std=c++11
  )
  
  add_library(ci-connect-server STATIC ${SERVER_SOURCES})
  target_include_directories(ci-connect-server
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${Boost_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}>
  )
  target_link_libraries(ci-connect-server
    PUBLIC
    pthread
    aws-cpp-sdk-dynamodb
#    aws-cpp-sdk-route53
    aws-cpp-sdk-core
    ${CURL_LIBRARIES}
    ${SSL_LDFLAGS}
    ${LIBCRYPTO_LDFLAGS}
    ${Boost_LIBRARIES}
    ${ZLIB_LIBRARIES}
#    ${YAMLCPP_LDFLAGS}
    )
  target_compile_options(ci-connect-server PRIVATE ${CONNECT_SERVER_COMPILE_OPTIONS} )
  add_dependencies(ci-connect-server embed_version)
  
  add_executable(ci-connect-service ${CMAKE_SOURCE_DIR}/src/ciconnect_service.cpp)
  target_compile_options(ci-connect-service PRIVATE -DRAPIDJSON_HAS_STDSTRING -DCONNECT_SERVER -std=c++11)
  target_link_libraries(ci-connect-service ci-connect-server)
  install(TARGETS ci-connect-service RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  # TODO: uninstall

  # -----------------------------------------------------------------------------
  # Testing
  if(BUILD_SERVER_TESTS)
    configure_file(test/CTestCustom.cmake CTestCustom.cmake)
    enable_testing()
    
    add_executable(ci-connect-test-database-server
      test/DBServer.cpp
      src/Archive.cpp
      src/FileHandle.cpp
      src/FileSystem.cpp
      src/Process.cpp
    )
    target_include_directories (ci-connect-test-database-server
      PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        ${CURL_INCLUDE_DIRS}
        ${LIBCRYPTO_CFLAGS}
        ${SSL_CFLAGS}
        ${Boost_INCLUDE_DIRS}
        ${CURL_INCLUDE_DIRS}
        ${LIBCRYPTO_CFLAGS}
        ${SSL_CFLAGS}
    )
    target_link_libraries(ci-connect-test-database-server
      PUBLIC
        ${CURL_LIBRARIES}
        ${SSL_LDFLAGS}
        ${LIBCRYPTO_LDFLAGS}
        ${Boost_LIBRARIES}
        ${ZLIB_LIBRARIES}
        pthread
    )
    
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    set (CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    
    list(APPEND BASE_TEST_COMPONENTS
      test/test_main.cpp
      test/HTTPRequests.cpp
    )
    
    add_library(ci-connect-testing STATIC
      ${BASE_TEST_COMPONENTS}
    )
    target_include_directories(ci-connect-testing
      PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      ${CURL_INCLUDE_DIRS}
      ${LIBCRYPTO_CFLAGS}
      ${SSL_CFLAGS}
      $<INSTALL_INTERFACE:include>
    )
    target_compile_options(ci-connect-testing PRIVATE -g -DRAPIDJSON_HAS_STDSTRING)
    target_link_libraries(ci-connect-testing
      PUBLIC
      curl
      boost_system
      pthread
      ssl
      crypto
    )
    
    macro(list_contains var value)
      set(${var})
      foreach (value2 ${ARGN})
        if (${value} STREQUAL ${value2})
          set(${var} TRUE)
        endif (${value} STREQUAL ${value2})
      endforeach (value2)
    endmacro(list_contains)
    
    MACRO(PARSE_ARGUMENTS prefix arg_names option_names)
      SET(DEFAULT_ARGS)
      FOREACH(arg_name ${arg_names})
        SET(${prefix}_${arg_name})
      ENDFOREACH(arg_name)
      FOREACH(option ${option_names})
        SET(${prefix}_${option} FALSE)
      ENDFOREACH(option)
    
      SET(current_arg_name DEFAULT_ARGS)
      SET(current_arg_list)
      FOREACH(arg ${ARGN})
        LIST_CONTAINS(is_arg_name ${arg} ${arg_names})
        IF (is_arg_name)
          SET(${prefix}_${current_arg_name} ${current_arg_list})
          SET(current_arg_name ${arg})
          SET(current_arg_list)
        ELSE (is_arg_name)
          LIST_CONTAINS(is_option ${arg} ${option_names})
          IF (is_option)
          SET(${prefix}_${arg} TRUE)
          ELSE (is_option)
          SET(current_arg_list ${current_arg_list} ${arg})
          ENDIF (is_option)
        ENDIF (is_arg_name)
      ENDFOREACH(arg)
      SET(${prefix}_${current_arg_name} ${current_arg_list})
    ENDMACRO(PARSE_ARGUMENTS)
    
    macro(connect_add_test TEST_NAME)
      PARSE_ARGUMENTS(${TEST_NAME}_ARGS "SOURCE_FILES;COMPILE_FLAGS;LINK_LIBRARIES" "" ${ARGN})
      add_executable(${TEST_NAME}
        ${${TEST_NAME}_ARGS_SOURCE_FILES}
        )
      target_compile_options(${TEST_NAME} PRIVATE -g -DRAPIDJSON_HAS_STDSTRING)
      target_link_libraries(${TEST_NAME}
        PUBLIC
        ${${TEST_NAME}_ARGS_LINK_LIBRARIES}
        ci-connect-testing
        ci-connect-server
      )
      list(APPEND ALL_TESTS
        tests/${TEST_NAME}
      )
    endmacro(connect_add_test)
    
    connect_add_test(test-user-listing
        SOURCE_FILES test/TestUserListing.cpp)
    
    connect_add_test(test-user-creation
        SOURCE_FILES test/TestUserCreation.cpp)
    
    connect_add_test(test-user-info
        SOURCE_FILES test/TestUserInfo.cpp)
    
    connect_add_test(test-user-update
        SOURCE_FILES test/TestUserUpdate.cpp)
    
    connect_add_test(test-user-deletion
        SOURCE_FILES test/TestUserDeletion.cpp)
    
    connect_add_test(test-user-group-addition
        SOURCE_FILES test/TestUserGroupAddition.cpp)
    
    connect_add_test(test-user-group-removal
        SOURCE_FILES test/TestUserGroupRemoval.cpp)
    
    connect_add_test(test-user-group-listing
        SOURCE_FILES test/TestUserGroupListing.cpp)
    
    connect_add_test(test-user-token-replacement
        SOURCE_FILES test/TestUserTokenReplacement.cpp)
    
    connect_add_test(test-find-user
        SOURCE_FILES test/TestFindUser.cpp)
    
    connect_add_test(test-group-listing
        SOURCE_FILES test/TestGroupListing.cpp)
    
    connect_add_test(test-group-info
        SOURCE_FILES test/TestGroupInfo.cpp)
    
    connect_add_test(test-group-update
        SOURCE_FILES test/TestGroupUpdate.cpp)
    
    connect_add_test(test-group-creation
        SOURCE_FILES test/TestGroupCreation.cpp)
    
    connect_add_test(test-group-deletion
        SOURCE_FILES test/TestGroupDeletion.cpp)
    
    connect_add_test(test-group-member-listing
        SOURCE_FILES test/TestGroupMemberListing.cpp)
    
    connect_add_test(test-group-cluster-listing
        SOURCE_FILES test/TestGroupClusterListing.cpp)
    
    connect_add_test(test-cluster-creation
        SOURCE_FILES test/TestClusterCreation.cpp)
    
    connect_add_test(test-cluster-listing
        SOURCE_FILES test/TestClusterListing.cpp)
    
    connect_add_test(test-cluster-info
        SOURCE_FILES test/TestClusterInfo.cpp)
    
    connect_add_test(test-cluster-deletion
        SOURCE_FILES test/TestClusterDeletion.cpp)
    
    connect_add_test(test-cluster-update
        SOURCE_FILES test/TestClusterUpdate.cpp)
    
    connect_add_test(test-cluster-allowed-group-listing
        SOURCE_FILES test/TestClusterAllowedGroupListing.cpp)
    
    connect_add_test(test-cluster-allowed-group-addition
        SOURCE_FILES test/TestClusterAllowedGroupAddition.cpp)
    
    connect_add_test(test-cluster-allowed-group-deletion
        SOURCE_FILES test/TestClusterAllowedGroupDeletion.cpp)
    
    connect_add_test(test-cluster-allowed-application-addition
        SOURCE_FILES test/TestClusterAllowedApplicationAddition.cpp)
    
    connect_add_test(test-cluster-allowed-application-deletion
        SOURCE_FILES test/TestClusterAllowedApplicationDeletion.cpp)
    
    connect_add_test(test-cluster-allowed-application-listing
        SOURCE_FILES test/TestClusterAllowedApplicationListing.cpp)
    
    connect_add_test(test-app-listing
        SOURCE_FILES test/TestApplicationListing.cpp)
    
    connect_add_test(test-fetch-app-config
        SOURCE_FILES test/TestFetchApplicationConfig.cpp)
    
    connect_add_test(test-app-install
        SOURCE_FILES test/TestApplicationInstall.cpp
        LINK_LIBRARIES boost_date_time)
    
    connect_add_test(test-ad-hoc-app-install
        SOURCE_FILES test/TestAdHocApplicationInstall.cpp
        LINK_LIBRARIES boost_date_time)
    
    connect_add_test(test-instance-listing
        SOURCE_FILES test/TestInstanceListing.cpp)
    
    connect_add_test(test-instance-info-fetching
        SOURCE_FILES test/TestInstanceInfoFetching.cpp)
    
    connect_add_test(test-instance-restarting
        SOURCE_FILES test/TestInstanceRestarting.cpp)
    
    connect_add_test(test-instance-deletion
        SOURCE_FILES test/TestInstanceDeletion.cpp)
    
    connect_add_test(test-secret-listing
        SOURCE_FILES test/TestSecretListing.cpp)
    
    connect_add_test(test-secret-creation
        SOURCE_FILES test/TestSecretCreation.cpp
        LINK_LIBRARIES boost_date_time)
    
    connect_add_test(test-secret-deletion
        SOURCE_FILES test/TestSecretDeletion.cpp)
    
    connect_add_test(test-secret-fetching
        SOURCE_FILES test/TestSecretFetching.cpp)
      
    foreach(TEST ${ALL_TESTS})
      get_filename_component(TEST_NAME ${TEST} NAME_WE)
      add_test(${TEST_NAME} ${TEST})
    endforeach()
    
    set_tests_properties(test-ad-hoc-app-install PROPERTIES 
        ENVIRONMENT TEST_SRC=${CMAKE_CURRENT_SOURCE_DIR}/test
        )
    
    add_custom_target(check 
      COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
      DEPENDS ${ALL_TESTS} ci-connect-test-database-server ci-connect-service)
  endif(BUILD_SERVER_TESTS)
  
  LIST(APPEND RPM_SOURCES ${SERVER_SOURCES})

endif(BUILD_SERVER)

add_custom_target(rpm-sources 
  # FOO=BAR is a sacrificial dummy variable to absorb the extra 
  # quotes that cmake erroneously puts on the first variable
  COMMAND FOO=BAR 
    CMAKE_BINARY_DIR=${CMAKE_BINARY_DIR} 
    CMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR} 
    ${CMAKE_SOURCE_DIR}/resources/build_rpm.sh
  DEPENDS ${RPM_SOURCES})

  add_custom_target(client-rpm
    COMMAND rpmbuild --define "_topdir ${CMAKE_BINARY_DIR}" --define "version `${CMAKE_SOURCE_DIR}/cmake/extract_version.sh" "${CMAKE_SOURCE_DIR}`" -ba "${CMAKE_SOURCE_DIR}/resources/rpm_specs/ci-connect-api.spec"
    DEPENDS rpm-sources)
  LIST(APPEND RPMS client-rpm)

add_custom_target(rpm
  DEPENDS ${RPMS})
