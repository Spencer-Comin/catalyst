##################################
# Object Lib catalyst_qir_qis_obj
##################################

add_library(catalyst_qir_qis_obj OBJECT RuntimeCAPI.cpp)

# link to rt_backend
target_link_libraries(catalyst_qir_qis_obj ${CMAKE_DL_LIBS}
    "-L${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
    -lrt_backend
    pennylane_lightning)  # TODO: find a solution to remove dependencies of
                          # LightningSimulator.hpp to the monorepo header files.

if(ENABLE_OPENQASM)
    target_link_libraries(catalyst_qir_qis_obj pybind11::module)
endif()

target_link_libraries(catalyst_qir_qis_obj "-L${QIR_STDLIB_PATH}"
    qir_stdlib
    pthread
    dl)

target_include_directories(catalyst_qir_qis_obj PUBLIC .
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${QIR_STDLIB_PATH}
    ${runtime_includes}
    ${backend_includes}
    ${lightning_dynamic_includes})  # TODO: remove this after adding dlopen support
                                    # fatal error: 'StateVectorLQubitDynamic.hpp'
                                    # file not found in LightningSimulator.hpp


set_property(TARGET catalyst_qir_qis_obj PROPERTY POSITION_INDEPENDENT_CODE ON)

#####################
# Shared Lib rt_capi
#####################

add_library(rt_capi SHARED)

# link to rt_backend
target_link_libraries(rt_capi ${CMAKE_DL_LIBS} catalyst_qir_qis_obj)

add_dependencies(rt_capi rt_backend)

target_include_directories(rt_capi PUBLIC .
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${QIR_STDLIB_PATH}
    ${runtime_includes}
    ${capi_utils_includes}
    ${backend_includes})

set_property(TARGET rt_capi PROPERTY POSITION_INDEPENDENT_CODE ON)
