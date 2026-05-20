#!/bin/bash
if [ $# -lt 1 ]; then
    echo "Usage: build.sh name"
    echo " 1 - Binance"
    echo " 2 - Xt"
    echo " 3 - Bybit"
    echo " 4 - Ltp_Binance"
    echo " 5 - Bingx"
    echo " 6 - Bitget"
    echo " 7 - Bitmart"
    echo " 8 - Lighter"
    echo " 9 - Okex"
    echo "10 - Gate"
    echo "11 - Kucoin"
    echo "12 - Coinex"
    echo "13 - Phemex"
    echo "14 - Hbg"
    echo "15 - Hyperliquid"
    echo "16 - Aster"
    echo "17 - Backpack"
    echo "18 - Nado"
    echo "19 - Edgex"
    echo "20 - Paradex"
    echo "21 - Apex"
    echo "22 - Bitunix"
    echo "23 - Dexless"
    echo "24 - Toobit"
    echo "25 - Orangex"
    echo "26 - Weex"
    echo "27 - Crossex_Gate"
    exit 1
fi

exchange_name=$1
case $exchange_name in
    1)
        BUILD_TYPE=USE_BINANCE
        ;;
    2)
        BUILD_TYPE=USE_XT
        ;;
    3)
        BUILD_TYPE=USE_BYBIT
        ;;
    4)
        BUILD_TYPE=USE_LTPBINANCE
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
        BUILD_TYPE=USE_LIGHTER
        ;;
    9)
        BUILD_TYPE=USE_OKEX
        ;;
    10)
        BUILD_TYPE=USE_GATE
        ;;
    11)
        BUILD_TYPE=USE_KUCOIN
        ;;
    12)
        BUILD_TYPE=USE_COINEX
        ;;
    13)
        BUILD_TYPE=USE_PHEMEX
        ;;
    14)
        BUILD_TYPE=USE_HBG
        ;;
    15)
        BUILD_TYPE=USE_HYPERLIQUID
        ;;
    16)
        BUILD_TYPE=USE_ASTER
        ;;
    17)
        BUILD_TYPE=USE_BACKPACK
        ;;
    18)
        BUILD_TYPE=USE_NADO
        ;;
    19)
        BUILD_TYPE=USE_EDGEX
        ;;
    20)
        BUILD_TYPE=USE_PARADEX
        ;;
    21)
        BUILD_TYPE=USE_APEX
        ;;
    22)
        BUILD_TYPE=USE_BITUNIX
        ;;
    23)
        BUILD_TYPE=USE_DEXLESS
        ;;
    24)
        BUILD_TYPE=USE_TOOBIT
        ;;
    25)
        BUILD_TYPE=USE_ORANGEX
        ;;
    26)
        BUILD_TYPE=USE_WEEX
        ;;
    27)
        BUILD_TYPE=USE_CROSSEX_GATE
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
cmake --build . --config Release -j8
