# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2019 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# generate git/build information
include(GetSCMRev)

function(get_timestamp _var)
    string(TIMESTAMP timestamp UTC)
    set(${_var} "${timestamp}" PARENT_SCOPE)
endfunction()

get_timestamp(BUILD_DATE)

if (DEFINED GIT_RELEASE)
    set(BUILD_VERSION "${GIT_TAG}")
    set(GIT_REFSPEC "${GIT_RELEASE}")
    set(IS_DEV_BUILD false)
else()
    string(SUBSTRING ${GIT_COMMIT} 0 10 BUILD_VERSION)
    set(BUILD_VERSION "${BUILD_VERSION}-${GIT_REFSPEC}")
    set(IS_DEV_BUILD true)
endif()

if (NIGHTLY_BUILD)
    set(IS_NIGHTLY_BUILD true)
else()
    set(IS_NIGHTLY_BUILD false)
endif()

set(GIT_DESC ${BUILD_VERSION})

# Generate cpp with Git revision from template

# TODO(crueter): Stable releases feed.
set(BUILD_AUTO_UPDATE_STABLE_REPO "eden-emu/eden")
set(BUILD_AUTO_UPDATE_STABLE_API "git.eden-emu.dev")
set(BUILD_AUTO_UPDATE_STABLE_API_PATH "/api/v1/repos/")

set(BUILD_AUTO_UPDATE_API_PATH "/latest/release.json")
if (NIGHTLY_BUILD)
    set(BUILD_AUTO_UPDATE_WEBSITE "https://git.eden-emu.dev")
    set(BUILD_AUTO_UPDATE_API "nightly.eden-emu.dev")
    set(BUILD_AUTO_UPDATE_REPO "eden-ci/nightly")
    set(REPO_NAME "Eglantine Nightly")
else()
    set(BUILD_AUTO_UPDATE_WEBSITE "https://git.eden-emu.dev")
    set(BUILD_AUTO_UPDATE_API "stable.eden-emu.dev")
    set(BUILD_AUTO_UPDATE_REPO "eden-emu/eden")
    set(REPO_NAME "Eglantine")
endif()

set(BUILD_ID ${GIT_REFSPEC})
set(BUILD_FULLNAME "${REPO_NAME} ${BUILD_VERSION} ")
set(CXX_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

configure_file(scm_rev.cpp.in scm_rev.cpp @ONLY)

# Refresh the commit and branch at build time (RefreshSCMRev.cmake): configure-time values went
# stale after commits and branch switches.
if (GIT_FOUND AND NOT DEFINED GIT_RELEASE AND NOT (EXISTS ${REF_FILE} AND EXISTS ${COMMIT_FILE}))
    set(SCM_VARS_FILE ${CMAKE_CURRENT_BINARY_DIR}/scm_rev_vars.cmake)
    set(scm_vars "")
    foreach(var REPO_NAME TITLE_BAR_FORMAT_IDLE TITLE_BAR_FORMAT_RUNNING IS_DEV_BUILD CXX_COMPILER
            BUILD_AUTO_UPDATE_WEBSITE BUILD_AUTO_UPDATE_API BUILD_AUTO_UPDATE_API_PATH
            BUILD_AUTO_UPDATE_REPO BUILD_AUTO_UPDATE_STABLE_API BUILD_AUTO_UPDATE_STABLE_API_PATH
            BUILD_AUTO_UPDATE_STABLE_REPO IS_NIGHTLY_BUILD)
        string(APPEND scm_vars "set(${var} [==[${${var}}]==])\n")
    endforeach()
    file(WRITE ${SCM_VARS_FILE} "${scm_vars}")
    add_custom_target(scm_rev_refresh
        COMMAND ${CMAKE_COMMAND} -DSCM_VARS=${SCM_VARS_FILE}
                -DSCM_IN=${CMAKE_CURRENT_SOURCE_DIR}/scm_rev.cpp.in
                -DSCM_OUT=${CMAKE_CURRENT_BINARY_DIR}/scm_rev.cpp -DSCM_GIT=${GIT_EXECUTABLE}
                -DSCM_SOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_CURRENT_LIST_DIR}/RefreshSCMRev.cmake
        BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/scm_rev.cpp
        VERBATIM)
endif()
