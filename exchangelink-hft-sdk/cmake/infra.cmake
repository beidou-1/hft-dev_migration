cmake_minimum_required(VERSION 3.14)

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
