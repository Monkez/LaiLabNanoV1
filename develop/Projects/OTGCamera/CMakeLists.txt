cmake_minimum_required(VERSION 3.5)

SET(CMAKE_C_COMPILER "$ENV{COMPILER}/riscv64-unknown-linux-musl-gcc")
SET(CMAKE_CXX_COMPILER "$ENV{COMPILER}/riscv64-unknown-linux-musl-g++")
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

project(Yolo_CSIStream)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "-march=rv64imafd -O3 -DNDEBUG -D_MIDDLEWARE_V2_ -DC906 -DUSE_TPU_IVE -fsigned-char -Wno-format-truncation -fdiagnostics-color=always -s")

set(OpenCV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/opencv-mobile-4.10.0-licheerv-nano/lib/cmake/opencv4")
find_package(OpenCV REQUIRED)
include_directories(
    ${OpenCV_INCLUDE_DIRS}
    $ENV{SDK_PATH}/cvitek_tdl_sdk/include
    $ENV{SDK_PATH}/cvitek_tdl_sdk/include/cvi_tdl
    $ENV{SDK_PATH}/cvitek_tdl_sdk/sample/3rd/middleware/v2/include
    $ENV{SDK_PATH}/cvitek_tdl_sdk/sample/3rd/middleware/v2/include/linux
    )

add_executable(Yolo_CSIStream main.cpp)

target_link_libraries(Yolo_CSIStream 
    ${OpenCV_LIBS}
    -mcpu=c906fdv
    -L$ENV{SDK_PATH}/cvitek_tdl_sdk/sample/3rd/middleware/v2/lib
    -L$ENV{SDK_PATH}/cvitek_tdl_sdk/sample/3rd/middleware/v2/lib/3rd
    -lini -lsns_full -lsample -lisp -lvdec -lvenc -lawb -lae -laf -lcvi_bin -lcvi_bin_isp -lmisc -lisp_algo -lsys -lvpu
    -L$ENV{SDK_PATH}/cvitek_tdl_sdk/sample/3rd/opencv/lib
    -lopencv_core -lopencv_imgproc -lopencv_imgcodecs
    -L$ENV{SDK_PATH}/cvitek_tdl_sdk/sample/3rd/tpu/lib
    -lcnpy -lcvikernel -lcvimath -lcviruntime -lz -lm
    -lcvi_ive
    -L$ENV{SDK_PATH}/cvitek_tdl_sdk/lib
    -lcvi_tdl
    -L$ENV{SDK_PATH}/cvitek_tdl_sdk/sample/3rd/lib
    -lpthread -latomic
    
)

# Reset Button Daemon (standalone, no SDK dependencies)
add_executable(reset_btn reset_btn.cpp)
target_compile_options(reset_btn PRIVATE -O2)
target_link_libraries(reset_btn -lpthread)

# Hardware diagnostic: validates USB/UVC dequeue-requeue independently of VPSS.
add_executable(v4l2_requeue_test tools/v4l2_requeue_test.c)
target_compile_options(v4l2_requeue_test PRIVATE -O2)
