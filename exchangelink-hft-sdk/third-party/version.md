# 第三方库说明

* boost-1.83.0 (系统安装)
* opensssl-3.0.13 (通过系统安装)
* fmtlog-2.3.0
* sbepp-1.7.0
* simdjson-4.2.1
* lighterSDK：lighter交易所官方库
* ethash-1.1.0，https://github.com/chfast/ethash/tree/v1.1.0
* secp256k1-0.7.0，https://github.com/bitcoin-core/secp256k1/tree/v0.7.0
* crypto-cpp，https://github.com/starkware-libs/crypto-cpp
    改动1：注释了各层级CMakeLists.txt的测试代码编译
    改动2：crypto-cpp\src\third_party\gsl\gsl-lite.hpp，修复了编译问题