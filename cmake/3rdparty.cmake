set(EXTERNAL_PROJECTS_PREFIX ${CMAKE_BINARY_DIR}/external-projects)
set(EXTERNAL_PROJECTS_INSTALL_PREFIX ${EXTERNAL_PROJECTS_PREFIX}/installed)

include(GNUInstallDirs)
include(ExternalProject)

# MUST be called before any add_executable() # https://stackoverflow.com/a/40554704/8766845
link_directories(${EXTERNAL_PROJECTS_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR})
include_directories($<BUILD_INTERFACE:${EXTERNAL_PROJECTS_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}>)

if (YAHAT_WITH_LOGFAULT)
    find_path(LOGFAULT_DIR NAMES logfault.h PATH_SUFFIXES logfault)
    if (NOT LOGFAULT_DIR STREQUAL "LOGFAULT-NOTFOUND" )
        message ("Using existing logfault")
        add_library(logfault INTERFACE IMPORTED)
        include_directories(LOGFAULT)
    else()
        message ("Embedding logfault header only library")
        ExternalProject_Add(logfault
            PREFIX "${EXTERNAL_PROJECTS_PREFIX}"
            GIT_REPOSITORY "https://github.com/jgaa/logfault.git"
            GIT_TAG "master"
            CMAKE_ARGS
                -DCMAKE_INSTALL_PREFIX=${EXTERNAL_PROJECTS_INSTALL_PREFIX}
                -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        )
    endif() # embed
    add_compile_definitions(USE_LOGFAULT)
endif() # use logfault
