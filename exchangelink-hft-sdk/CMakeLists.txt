cmake_minimum_required(VERSION 3.16)
project(coinrisings-infra)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -O3 -march=native -mtune=native")

# libraries
find_package(Boost REQUIRED COMPONENTS system date_time iostreams)
set(INFRA_LIBRARIES ${Boost_LIBRARIES} pthread uring)

find_package(OpenSSL REQUIRED)
list(APPEND INFRA_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)

add_subdirectory(third-party)
list(APPEND INFRA_LIBRARIES fmt fmtlog-static sbepp::sbepp)

# definitions
include(cmake/infra.cmake)

# build
add_library(infra STATIC ${INFRA_SOURCES})
target_include_directories(infra PUBLIC ${INFRA_INCLUDE})
target_link_libraries(infra PUBLIC ${INFRA_LIBRARIES})
target_compile_definitions(
  infra
  PUBLIC ${INFRA_DEFINITIONS} BOOST_ASIO_NO_EXCEPTIONS ASIO_CONCURRENCY_HINT=1
         BOOST_ASIO_DISABLE_EPOLL BOOST_ASIO_HAS_IO_URING)
