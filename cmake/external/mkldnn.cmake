# Copyright (c) 2017 PaddlePaddle Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

INCLUDE(ExternalProject)

SET(MKLDNN_PROJECT        "extern_mkldnn")
SET(MKLDNN_PREFIX_DIR     ${THIRD_PARTY_PATH}/mkldnn)
SET(MKLDNN_INSTALL_DIR    ${THIRD_PARTY_PATH}/install/mkldnn)
SET(MKLDNN_INC_DIR        "${MKLDNN_INSTALL_DIR}/include" CACHE PATH "mkldnn include directory." FORCE)
SET(MKLDNN_REPOSITORY     ${GIT_URL}/oneapi-src/oneDNN.git)
SET(MKLDNN_TAG            145c4b50196ac90ec1b946fb80cb5cef6e7d2d35)


# Introduce variables:
# * CMAKE_INSTALL_LIBDIR
INCLUDE(GNUInstallDirs)
SET(LIBDIR "lib")
if(CMAKE_INSTALL_LIBDIR MATCHES ".*lib64$")
  SET(LIBDIR "lib64")
endif()

MESSAGE(STATUS "Set ${MKLDNN_INSTALL_DIR}/${LIBDIR} to runtime path")
SET(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
SET(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_RPATH}" "${MKLDNN_INSTALL_DIR}/${LIBDIR}")

INCLUDE_DIRECTORIES(${MKLDNN_INC_DIR}) # For MKLDNN code to include internal headers.


IF(NOT WIN32)
    SET(MKLDNN_FLAG "-Wno-error=strict-overflow -Wno-error=unused-result -Wno-error=array-bounds")
    SET(MKLDNN_FLAG "${MKLDNN_FLAG} -Wno-unused-result -Wno-unused-value")
    SET(MKLDNN_CFLAG "${CMAKE_C_FLAGS} ${MKLDNN_FLAG}")
    SET(MKLDNN_CXXFLAG "${CMAKE_CXX_FLAGS} ${MKLDNN_FLAG}")
    SET(MKLDNN_CXXFLAG_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    SET(MKLDNN_CFLAG_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    SET(MKLDNN_LIB "${MKLDNN_INSTALL_DIR}/${LIBDIR}/libdnnl.so" CACHE FILEPATH "mkldnn library." FORCE)
ELSE()
    SET(MKLDNN_CXXFLAG "${CMAKE_CXX_FLAGS} /EHsc")
    SET(MKLDNN_CFLAG "${CMAKE_C_FLAGS}")
    string(REPLACE "/O2 " "" MKLDNN_CFLAG_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    string(REPLACE "/O2 " "" MKLDNN_CXXFLAG_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    SET(MKLDNN_LIB "${MKLDNN_INSTALL_DIR}/bin/mkldnn.lib" CACHE FILEPATH "mkldnn library." FORCE)
ENDIF(NOT WIN32)

ExternalProject_Add(
    ${MKLDNN_PROJECT}
    ${EXTERNAL_PROJECT_LOG_ARGS}
    ${SHALLOW_CLONE}
    GIT_REPOSITORY      ${MKLDNN_REPOSITORY}
    GIT_TAG             ${MKLDNN_TAG}
    DEPENDS             ${MKLDNN_DEPENDS}
    PREFIX              ${MKLDNN_PREFIX_DIR}
    UPDATE_COMMAND      ""
    #BUILD_ALWAYS        1
    CMAKE_ARGS          -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                        -DCMAKE_CXX_FLAGS=${MKLDNN_CXXFLAG}
                        -DCMAKE_CXX_FLAGS_RELEASE=${MKLDNN_CXXFLAG_RELEASE}
                        -DCMAKE_CXX_FLAGS_DEBUG=${CMAKE_CXX_FLAGS_DEBUG}
                        -DCMAKE_C_FLAGS=${MKLDNN_CFLAG}
                        -DCMAKE_C_FLAGS_DEBUG=${CMAKE_C_FLAGS_DEBUG}
                        -DCMAKE_C_FLAGS_RELEASE=${MKLDNN_CFLAG_RELEASE}
                        -DCMAKE_INSTALL_PREFIX=${MKLDNN_INSTALL_DIR}
                        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
                        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
                        -DDNNL_BUILD_TESTS=OFF -DDNNL_BUILD_EXAMPLES=OFF
    CMAKE_CACHE_ARGS    -DCMAKE_INSTALL_PREFIX:PATH=${MKLDNN_INSTALL_DIR}
)

MESSAGE(STATUS "MKLDNN library: ${MKLDNN_LIB}")
add_definitions(-DPADDLE_WITH_MKLDNN)
# copy the real so.0 lib to install dir
# it can be directly contained in wheel or capi
if(WIN32)
    SET(MKLDNN_SHARED_LIB ${MKLDNN_INSTALL_DIR}/bin/mkldnn.dll)

    file(TO_NATIVE_PATH ${MKLDNN_INSTALL_DIR} NATIVE_MKLDNN_INSTALL_DIR)
    file(TO_NATIVE_PATH ${MKLDNN_SHARED_LIB} NATIVE_MKLDNN_SHARED_LIB)

    ADD_CUSTOM_COMMAND(OUTPUT ${MKLDNN_LIB}
        COMMAND (copy ${NATIVE_MKLDNN_INSTALL_DIR}\\bin\\dnnl.dll ${NATIVE_MKLDNN_SHARED_LIB} /Y)
        COMMAND dumpbin /exports ${MKLDNN_INSTALL_DIR}/bin/mkldnn.dll > ${MKLDNN_INSTALL_DIR}/bin/exports.txt
        COMMAND echo LIBRARY mkldnn > ${MKLDNN_INSTALL_DIR}/bin/mkldnn.def
        COMMAND echo EXPORTS >> ${MKLDNN_INSTALL_DIR}/bin/mkldnn.def
        COMMAND echo off && (for /f "skip=19 tokens=4" %A in (${MKLDNN_INSTALL_DIR}/bin/exports.txt) do echo %A >> ${MKLDNN_INSTALL_DIR}/bin/mkldnn.def) && echo on
        COMMAND lib /def:${MKLDNN_INSTALL_DIR}/bin/mkldnn.def /out:${MKLDNN_LIB} /machine:x64
        COMMENT "Generate mkldnn.lib manually--->"
        DEPENDS ${MKLDNN_PROJECT}
        VERBATIM)
    ADD_CUSTOM_TARGET(mkldnn_cmd ALL DEPENDS ${MKLDNN_LIB})
else(WIN32)
    SET(MKLDNN_SHARED_LIB ${MKLDNN_INSTALL_DIR}/libmkldnn.so.0)
    SET(MKLDNN_SHARED_LIB_1 ${MKLDNN_INSTALL_DIR}/libdnnl.so.1)
    SET(MKLDNN_SHARED_LIB_2 ${MKLDNN_INSTALL_DIR}/libdnnl.so.2)
    ADD_CUSTOM_COMMAND(OUTPUT ${MKLDNN_SHARED_LIB_2}
        COMMAND ${CMAKE_COMMAND} -E copy ${MKLDNN_LIB} ${MKLDNN_SHARED_LIB}
        COMMAND ${CMAKE_COMMAND} -E copy ${MKLDNN_LIB} ${MKLDNN_SHARED_LIB_1}
        COMMAND ${CMAKE_COMMAND} -E copy ${MKLDNN_LIB} ${MKLDNN_SHARED_LIB_2}
        DEPENDS ${MKLDNN_PROJECT})
    ADD_CUSTOM_TARGET(mkldnn_cmd ALL DEPENDS ${MKLDNN_SHARED_LIB_2})
endif(WIN32)

# generate a static dummy target to track mkldnn dependencies
# for cc_library(xxx SRCS xxx.c DEPS mkldnn)
generate_dummy_static_lib(LIB_NAME "mkldnn" GENERATOR "mkldnn.cmake")

TARGET_LINK_LIBRARIES(mkldnn ${MKLDNN_LIB} ${MKLML_IOMP_LIB})
ADD_DEPENDENCIES(mkldnn ${MKLDNN_PROJECT} mkldnn_cmd)
