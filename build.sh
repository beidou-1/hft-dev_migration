#!/bin/bash
if [ $# -lt 1 ]; then
    echo "Usage: build.sh name"
    echo " 1 - Apex"
    echo " 2 - Aster"
    echo " 3 - Backpack"
    echo " 4 - Binance"
    echo " 5 - Bingx"
    echo " 6 - Bitget"
    echo " 7 - Bitmart"
    echo " 8 - Bitunix"
    echo " 9 - Bybit"
    echo "10 - Coinex"
    echo "11 - Crossex_Gate"
    echo "12 - Dexless"
    echo "13 - Edgex"
    echo "14 - Gate"
    echo "15 - Hbg"
    echo "16 - Hyperliquid"
    echo "17 - Kucoin"
    echo "18 - Lighter"
    echo "19 - Ltp_Binance"
    echo "20 - Nado"
    echo "21 - Okex"
    echo "22 - Orangex"
    echo "23 - Paradex"
    echo "24 - Phemex"
    echo "25 - Toobit"
    echo "26 - Weex"
    echo "27 - Xt"
    exit 1
fi

exchange_name=$1
case $exchange_name in
    1)
        BUILD_TYPE=USE_APEX
        ;;
    2)
        BUILD_TYPE=USE_ASTER
        ;;
    3)
        BUILD_TYPE=USE_BACKPACK
        ;;
    4)
        BUILD_TYPE=USE_BINANCE
        ;;
    5)
        BUILD_TYPE=USE_BINGX
        ;;
    6)
        BUILD_TYPE=USE_BITGET
        ;;
    7)
        BUILD_TYPE=USE_BITMART
        ;;
    8)
        BUILD_TYPE=USE_BITUNIX
        ;;
    9)
        BUILD_TYPE=USE_BYBIT
        ;;
    10)
        BUILD_TYPE=USE_COINEX
        ;;
    11)
        BUILD_TYPE=USE_CROSSEX_GATE
        ;;
    12)
        BUILD_TYPE=USE_DEXLESS
        ;;
    13)
        BUILD_TYPE=USE_EDGEX
        ;;
    14)
        BUILD_TYPE=USE_GATE
        ;;
    15)
        BUILD_TYPE=USE_HBG
        ;;
    16)
        BUILD_TYPE=USE_HYPERLIQUID
        ;;
    17)
        BUILD_TYPE=USE_KUCOIN
        ;;
    18)
        BUILD_TYPE=USE_LIGHTER
        ;;
    19)
        BUILD_TYPE=USE_LTP_BINANCE
        ;;
    20)
        BUILD_TYPE=USE_NADO
        ;;
    21)
        BUILD_TYPE=USE_OKEX
        ;;
    22)
        BUILD_TYPE=USE_ORANGEX
        ;;
    23)
        BUILD_TYPE=USE_PARADEX
        ;;
    24)
        BUILD_TYPE=USE_PHEMEX
        ;;
    25)
        BUILD_TYPE=USE_TOOBIT
        ;;
    26)
        BUILD_TYPE=USE_WEEX
        ;;
    27)
        BUILD_TYPE=USE_XT
        ;;
    *)
        echo "parameter error"
        exit 1
esac

base_dir=$(dirname $0)
cd $base_dir

name=$(echo "${BUILD_TYPE#USE_}" | tr '[:upper:]' '[:lower:]')
build_dir=Build_${name}
rm -rf $build_dir
mkdir $build_dir && cd $build_dir

cmake .. -DCMAKE_BUILD_TYPE=Release -D$BUILD_TYPE=YES
cmake --build . --config Release -j1
