# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Build-time refresh of scm_rev.cpp (cmake -P). GenerateSCMRev writes the file at configure time
# only, so a build after a commit or a branch switch kept the old commit and branch (the window
# title named a branch three rounds old). This reads them again and rewrites the file only when
# they changed, so an unchanged tree does not recompile it.
#   -DSCM_VARS=<vars file from configure> -DSCM_IN=<scm_rev.cpp.in> -DSCM_OUT=<scm_rev.cpp>
#   -DSCM_GIT=<git executable> -DSCM_SOURCE_DIR=<repository>

include(${SCM_VARS})

foreach(item COMMIT REFSPEC)
    if (item STREQUAL "COMMIT")
        set(args rev-parse HEAD)
    else()
        set(args rev-parse --abbrev-ref HEAD)
    endif()
    execute_process(COMMAND "${SCM_GIT}" ${args}
        WORKING_DIRECTORY "${SCM_SOURCE_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE out
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (NOT result EQUAL 0 OR out STREQUAL "")
        return() # keep what configure wrote
    endif()
    set(GIT_${item} "${out}")
endforeach()

if (EXISTS "${SCM_OUT}")
    file(READ "${SCM_OUT}" current)
    string(FIND "${current}" "#define GIT_REV \"${GIT_COMMIT}\"" has_commit)
    string(FIND "${current}" "#define GIT_BRANCH \"${GIT_REFSPEC}\"" has_branch)
    if (NOT has_commit EQUAL -1 AND NOT has_branch EQUAL -1)
        return()
    endif()
endif()

string(SUBSTRING "${GIT_COMMIT}" 0 10 BUILD_VERSION)
set(BUILD_VERSION "${BUILD_VERSION}-${GIT_REFSPEC}")
set(GIT_DESC "${BUILD_VERSION}")
set(BUILD_ID "${GIT_REFSPEC}")
set(BUILD_FULLNAME "${REPO_NAME} ${BUILD_VERSION} ")
string(TIMESTAMP BUILD_DATE UTC)
configure_file("${SCM_IN}" "${SCM_OUT}" @ONLY)
message(STATUS "scm_rev.cpp: ${BUILD_VERSION}")
