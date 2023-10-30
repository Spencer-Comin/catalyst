include(FetchContent)

set(LIGHTNING_GIT_TAG "master" CACHE STRING "GIT_TAG value to build Lightning")
FetchContent_Declare(
    pennylane_lightning
    GIT_REPOSITORY  https://github.com/PennyLaneAI/pennylane-lightning.git
    GIT_TAG         ${LIGHTNING_GIT_TAG}
)

if(ENABLE_LIGHTNING AND ENABLE_LIGHTNING_KOKKOS)
    set(PL_BACKEND "lightning_qubit;lightning_kokkos" CACHE STRING "PennyLane Lightning backend")
elseif(ENABLE_LIGHTNING)
    set(PL_BACKEND "lightning_qubit" CACHE STRING "PennyLane Lightning backend")
elseif(ENABLE_LIGHTNING_KOKKOS)
    set(PL_BACKEND "lightning_kokkos" CACHE STRING "PennyLane Lightning backend")
endif()

set(ENABLE_PYTHON OFF CACHE BOOL "Enable compilation of the Python module")
FetchContent_MakeAvailable(pennylane_lightning)

if(RUNTIME_CLANG_TIDY)
    if(NOT DEFINED CLANG_TIDY_BINARY)
        set(CLANG_TIDY_BINARY clang-tidy)
    endif()
    message(STATUS "Using CLANG_TIDY_BINARY=${CLANG_TIDY_BINARY}")
    set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_BINARY};
            -extra-arg=-std=c++20;
            --use-color;
    )
endif()

if(ENABLE_LIGHTNING)
    list(APPEND source_files
        lightning/lightning_dynamic/StateVectorLQubitDynamic.cpp
        lightning/LightningSimulator.cpp)

    list(APPEND backend_includes
        lightning/lightning_dynamic)
endif()

if(ENABLE_LIGHTNING_KOKKOS)
    list(APPEND source_files
    lightning-kokkos/LightningKokkosSimulator.cpp)
endif()

if(ENABLE_OPENQASM)
    find_package(pybind11 CONFIG)

    if(pybind11_FOUND)
        message(STATUS "Found pybind11")
    else()
        message(STATUS "Cound not find existing pybind11-dev package. Building from source.")
        set(CMAKE_POLICY_DEFAULT_CMP0127 NEW) # To suppress pybind11 CMP0127 warning

        # https://cmake.org/cmake/help/latest/module/FindPython.html
        find_package(Python COMPONENTS Interpreter Development)

        FetchContent_Declare(pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG        v2.10.1
        )

        FetchContent_MakeAvailable(pybind11)
    endif()

    list(APPEND source_files
        openqasm/OpenQasmDevice.cpp)
endif()

add_library(rt_backend SHARED ${source_files})

target_include_directories(rt_backend PRIVATE
    ${backend_includes}
    ${runtime_includes})

target_link_libraries(rt_backend PRIVATE
    ${CMAKE_DL_LIBS}
    pennylane_lightning)

if(ENABLE_OPENQASM)
    # To avoid link to libpython, we use pybind11::module interface library.
    target_link_libraries(rt_backend PRIVATE pybind11::module)
endif()

set_property(TARGET rt_backend PROPERTY POSITION_INDEPENDENT_CODE ON)
