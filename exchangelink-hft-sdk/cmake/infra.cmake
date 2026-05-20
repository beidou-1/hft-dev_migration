cmake_minimum_required(VERSION 3.16)

set(INFRA_EXTERNAL_SDK)
set(INFRA_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(INFRA_INCLUDE "${INFRA_ROOT}" "${INFRA_ROOT}/third-party")
set(EXCHANGE_DEFINITIONS)

file(GLOB INFRA_SOURCES ${INFRA_ROOT}/common/*.cpp
     ${INFRA_ROOT}/shm/shm_manager.cpp
     ${INFRA_ROOT}/third-party/network/*.cpp
     ${INFRA_ROOT}/third-party/simdjson/simdjson.cpp)

function(add_exchange_module EXCHANGE_NAME)
  string(TOUPPER ${EXCHANGE_NAME} EXCHANGE_UPPER)
  if(USE_${EXCHANGE_UPPER})
    message(STATUS "Infra enable exchange: ${EXCHANGE_NAME}")
    file(GLOB ${EXCHANGE_UPPER}_SOURCES
         ${INFRA_ROOT}/exchanges/${EXCHANGE_NAME}/*.cpp)
    list(APPEND INFRA_SOURCES ${${EXCHANGE_UPPER}_SOURCES})
    list(APPEND EXCHANGE_DEFINITIONS -DENABLED_${EXCHANGE_UPPER})
    # 传递变量
    set(INFRA_SOURCES
        ${INFRA_SOURCES}
        PARENT_SCOPE)
    set(EXCHANGE_DEFINITIONS
        ${EXCHANGE_DEFINITIONS}
        PARENT_SCOPE)
  endif()
endfunction()

add_exchange_module(apex)
add_exchange_module(aster)
add_exchange_module(backpack)
add_exchange_module(binance)
add_exchange_module(bingx)
add_exchange_module(bitget)
add_exchange_module(bitmart)
add_exchange_module(bitunix)
add_exchange_module(bybit)
add_exchange_module(coinex)
add_exchange_module(crossex_gate)
add_exchange_module(dexless)
add_exchange_module(edgex)
add_exchange_module(gate)
add_exchange_module(hbg)
add_exchange_module(hyperliquid)
add_exchange_module(kucoin)
add_exchange_module(lighter)
add_exchange_module(ltp_binance)
add_exchange_module(nado)
add_exchange_module(okex)
add_exchange_module(orangex)
add_exchange_module(paradex)
add_exchange_module(phemex)
add_exchange_module(toobit)
add_exchange_module(weex)
add_exchange_module(xt)

set(INFRA_DEFINITIONS
    ${EXCHANGE_DEFINITIONS}
    CACHE INTERNAL "")

# DEX的第三方库
if(USE_LIGHTER)
    list(APPEND INFRA_EXTERNAL_SDK ${INFRA_ROOT}/third-party/lighterSDK/liblighter-signer-linux-amd64.so)
endif()

if(USE_HYPERLIQUID
   OR USE_NADO
   OR USE_EDGEX
   OR USE_ASTER)
  set(ETHASH_BUILD_TESTS
      OFF
      CACHE BOOL "Disable ethash tests" FORCE)
  set(ETHASH_BUILD_GLOBAL_CONTEXT
      OFF
      CACHE BOOL "Disable global context" FORCE)
  
  set(SECP256K1_BUILD_TESTS
      OFF
      CACHE BOOL "Disable secp256k1 tests" FORCE)
  set(SECP256K1_BUILD_EXHAUSTIVE_TESTS
      OFF
      CACHE BOOL "Disable secp256k1 exhaustive tests" FORCE)
  set(SECP256K1_BUILD_BENCHMARK
      OFF
      CACHE BOOL "Disable secp256k1 benchmarks" FORCE)
  set(SECP256K1_BUILD_EXAMPLES
      OFF
      CACHE BOOL "Disable secp256k1 examples" FORCE)
  set(SECP256K1_ENABLE_MODULE_RECOVERY
      ON
      CACHE BOOL "Enable secp256k1 recovery module")

  add_subdirectory(${INFRA_ROOT}/third-party/ethash ${CMAKE_CURRENT_BINARY_DIR}/ethash)
  add_subdirectory(${INFRA_ROOT}/third-party/secp256k1 ${CMAKE_CURRENT_BINARY_DIR}/secp256k1)
  list(APPEND INFRA_EXTERNAL_SDK ethash secp256k1)
endif()

if(USE_EDGEX OR USE_PARADEX)
  include_directories(${INFRA_ROOT}/third-party/crypto-cpp/src)
  add_subdirectory(${INFRA_ROOT}/third-party/crypto-cpp ${CMAKE_CURRENT_BINARY_DIR}/crypto-cpp)
endif()

if(USE_EDGEX)
  list(APPEND INFRA_EXTERNAL_SDK crypto algebra)
endif()