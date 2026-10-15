cmake_minimum_required(VERSION 3.16.0)
project(gru_kernel)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOC_VERSION "$ENV{SOC_VERSION}" CACHE STRING "system on chip type")
set(ASCEND_CANN_PACKAGE_PATH "$ENV{ASCEND_HOME_PATH}" CACHE PATH "ASCEND CANN package installation directory")
set(RUN_MODE "npu" CACHE STRING "run mode: npu")
set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type Release/Debug (default Release)" FORCE)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# Architecture-specific CANN subtree (x86_64 Linux host + dav-c220 device).
set(ASC_ARCH_DIR "${ASCEND_CANN_PACKAGE_PATH}/x86_64-linux")

# Find ascendc cmake
if(EXISTS ${ASC_ARCH_DIR}/tikcpp/ascendc_kernel_cmake)
    set(ASCENDC_CMAKE_DIR ${ASC_ARCH_DIR}/tikcpp/ascendc_kernel_cmake)
elseif(EXISTS ${ASCEND_CANN_PACKAGE_PATH}/tools/tikcpp/ascendc_kernel_cmake)
    set(ASCENDC_CMAKE_DIR ${ASCEND_CANN_PACKAGE_PATH}/tools/tikcpp/ascendc_kernel_cmake)
elseif(EXISTS ${ASCEND_CANN_PACKAGE_PATH}/compiler/tikcpp/ascendc_kernel_cmake)
    set(ASCENDC_CMAKE_DIR ${ASCEND_CANN_PACKAGE_PATH}/compiler/tikcpp/ascendc_kernel_cmake)
else()
    message(FATAL_ERROR "ascendc_kernel_cmake does not exist, please check whether the cann package is installed.")
endif()

include(${ASCENDC_CMAKE_DIR}/ascendc.cmake)

# AscendC kernel library (device side).
# The kernel build tags a core type per translation unit, so the vector kernels
# (AIV) and the GEMM (AIC) must live in separate sources.
ascendc_library(kernels STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/op_kernel/gru_vector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/op_kernel/gru_cube.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/op_kernel/gru_bwd_vector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/op_kernel/gru_bwd_cube.cpp
)

ascendc_include_directories(kernels PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/op_kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${ASCEND_CANN_PACKAGE_PATH}/include
    ${ASCEND_CANN_PACKAGE_PATH}/include/experiment/runtime
    ${ASCEND_CANN_PACKAGE_PATH}/include/experiment/msprof
    ${ASC_ARCH_DIR}/ascendc/include
)

# torch extension library (op_host + register)
add_library(gru_ext SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/op_host/gru.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/register.cpp
)
target_link_libraries(gru_ext PRIVATE
    kernels
    torch_npu
    m
    dl
    ${ASC_ARCH_DIR}/lib64/libtiling_api.a
)

# Resolve torch paths
execute_process(COMMAND python3 -c "import os; import torch; print(os.path.dirname(torch.__file__))"
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE TORCH_PATH
)
message("TORCH_PATH is ${TORCH_PATH}")
set(ENV{ASCEND_HOME_PATH} ${ASCEND_CANN_PACKAGE_PATH})
execute_process(COMMAND python3 -c "import os; import torch_npu; print(os.path.dirname(torch_npu.__file__))"
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE TORCH_NPU_PATH
)
message("TORCH_NPU_PATH is ${TORCH_NPU_PATH}")

target_link_directories(gru_ext PRIVATE
    ${TORCH_PATH}/lib
    ${TORCH_NPU_PATH}/lib
    ${ASC_ARCH_DIR}/lib64
)
target_include_directories(gru_ext PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${CMAKE_CURRENT_SOURCE_DIR}/op_kernel
    ${ASC_ARCH_DIR}/asc/include
    ${ASC_ARCH_DIR}/ascendc/include
    ${TORCH_NPU_PATH}/include
    ${TORCH_PATH}/include
    ${TORCH_PATH}/include/torch/csrc/api/include
)

# Pybind11 includes
execute_process(COMMAND python3 -m pybind11 --includes
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE PYBIND11_INC
)
string(REPLACE " " ";" PYBIND11_INC ${PYBIND11_INC})
target_compile_options(gru_ext PRIVATE
    ${PYBIND11_INC}
    -D_GLIBCXX_USE_CXX11_ABI=1
)

# Output .so naming
execute_process(COMMAND python3 -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX') or '.so')"
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE PYTHON_EXTENSION_SUFFIX
)
set_target_properties(gru_ext PROPERTIES
    OUTPUT_NAME gru_ext
    PREFIX ""
    SUFFIX "${PYTHON_EXTENSION_SUFFIX}"
)
