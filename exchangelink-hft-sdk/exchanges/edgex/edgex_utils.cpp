#include "edgex_utils.h"
#include "common/sign_eth.hpp"
#include "starkware/crypto/ecdsa.h"
#include "starkware/crypto/pedersen_hash.h"
using namespace starkware;

namespace infra::edgex {
Errno extract_error_code(std::string_view sv) {
    if (sv.find("timeout") != std::string_view::npos) {
        return Errno::RequestTimeout;
    } else if (sv.find("API-key") != std::string_view::npos) {
        return Errno::AuthFailed;
    } else if (sv.find("ORDER_LIMIT_PRICE") != std::string_view::npos) {
        return Errno::PercentPrice;
    } else if (sv.find("ORDER_MARGIN_AVAILABLE_AMOUNT_NOT_ENOUGH") != std::string_view::npos) {
        return Errno::InsufficientBalance;
    } else if (sv.find("ORDER_LIMIT_PRICE_BELOW_MIN_BUY_PRICE") != std::string_view::npos) {
        return Errno::SmallSizeOrder;
    } else if (sv.find("ORDER_SIZE_EXCEED_MIN_ORDER_SIZE") != std::string_view::npos) {
        return Errno::SmallSizeOrder;
    } else {
        return Errno::UnknownError;
    }
}

// StarkEx constants
const uint64_t LIMIT_ORDER_WITH_FEE_TYPE = 3;
const cpp_int FIELD_PRIME("0x0800000000000011000000000000000000000000000000000000000000000001");
const cpp_int K_MODULUS("0x0800000000000010ffffffffffffffffb781126dcae7b2321e66a241adc64d2f");
using BigInt4 = starkware::BigInt<4>;

// Convert cpp_int to BigInt<4> (little-endian: limbs[0] = low 64 bits)
BigInt4 CppIntToBigInt4(const cpp_int& input) {
    cpp_int x = input % FIELD_PRIME;
    if (x < 0)
        x += FIELD_PRIME;

    std::array<uint64_t, 4> limbs = {0, 0, 0, 0};

    // Extract up to 4 little-endian 64-bit limbs
    cpp_int temp = x;
    for (size_t i = 0; i < 4 && temp != 0; ++i) {
        limbs[i] = static_cast<uint64_t>(temp & UINT64_MAX);
        temp >>= 64;
    }

    return BigInt4(limbs);
}

PrimeFieldElement CppIntToField(const cpp_int& num) { return PrimeFieldElement::FromBigInt(CppIntToBigInt4(num)); }

PrimeFieldElement HexStringToField(const std::string& hex_str) {
    std::string clean = hex_str;
    if (starts_with_0x(clean)) {
        clean = clean.substr(2);
    }
    if (clean.empty() || clean == "0") {
        return PrimeFieldElement::Zero();
    }
    cpp_int x("0x" + clean); // cpp_int 支持 0x 前缀
    return CppIntToField(x);
}

// Helper: BigInt<4> (little-endian limbs) → 32-byte big-endian vector
std::vector<uint8_t> BigInt4ToBigEndianBytes(const BigInt4& x) {
    std::vector<uint8_t> bytes(32, 0);

    // We'll fill from LSB to MSB in little-endian byte order,
    // then reverse to get big-endian.
    std::array<uint8_t, 32> le_bytes = {};

    for (size_t limb_idx = 0; limb_idx < 4; ++limb_idx) {
        uint64_t limb = x[limb_idx]; // x[0] = low 64 bits
        for (size_t byte_idx = 0; byte_idx < 8; ++byte_idx) {
            le_bytes[limb_idx * 8 + byte_idx] = static_cast<uint8_t>(limb & 0xFF);
            limb >>= 8;
        }
    }

    // Convert to big-endian: reverse the whole array
    std::reverse_copy(le_bytes.begin(), le_bytes.end(), bytes.begin());
    return bytes;
}

// --- Main function: Calculate Limit Order Hash ---
std::string calc_limit_order_hash(const cpp_int& synthetic_asset_id, const cpp_int& collateral_asset_id,
                                  const cpp_int& fee_asset_id, bool is_buy, const cpp_int& amount_synthetic,
                                  const cpp_int& amount_collateral, const cpp_int& amount_fee, uint32_t nonce,
                                  const uint64_t& account_id, uint64_t expire_time,
                                  const std::string& private_key_hex) {
    // Parse asset IDs
    PrimeFieldElement asset_id_synthetic = CppIntToField(synthetic_asset_id);
    PrimeFieldElement asset_id_collateral = CppIntToField(collateral_asset_id);
    PrimeFieldElement asset_id_fee = CppIntToField(fee_asset_id);

    auto asset_id_sell = PrimeFieldElement::Zero();
    auto asset_id_buy = PrimeFieldElement::Zero();
    cpp_int amount_sell, amount_buy;

    if (is_buy) {
        asset_id_sell = asset_id_collateral;
        asset_id_buy = asset_id_synthetic;
        amount_sell = amount_collateral;
        amount_buy = amount_synthetic;
    } else {
        asset_id_sell = asset_id_synthetic;
        asset_id_buy = asset_id_collateral;
        amount_sell = amount_synthetic;
        amount_buy = amount_collateral;
    }

    // First hash: H(asset_id_sell, asset_id_buy)
    auto msg = ::starkware::PedersenHash(asset_id_sell, asset_id_buy);
    msg = ::starkware::PedersenHash(msg, asset_id_fee);
    cpp_int packed_message0 = amount_sell;
    packed_message0 <<= 64;
    packed_message0 += amount_buy;
    packed_message0 <<= 64;
    packed_message0 += amount_fee;
    packed_message0 <<= 32;
    packed_message0 += nonce;
    auto packed0_fe = PrimeFieldElement::FromBigInt(CppIntToBigInt4(packed_message0));

    msg = ::starkware::PedersenHash(msg, packed0_fe);
    uint32_t expire_hour = expire_time / (60 * 60 * 1000L);
    cpp_int packed_message1 = LIMIT_ORDER_WITH_FEE_TYPE;
    packed_message1 <<= 64;
    packed_message1 += account_id;
    packed_message1 <<= 64;
    packed_message1 += account_id;
    packed_message1 <<= 64;
    packed_message1 += account_id;
    packed_message1 <<= 32;
    packed_message1 += expire_hour;
    packed_message1 <<= 17; // padding as in Python
    auto packed1_fe = PrimeFieldElement::FromBigInt(CppIntToBigInt4(packed_message1));

    // Final hash: H(msg, packed_message1)
    auto final_hash = ::starkware::PedersenHash(msg, packed1_fe);
    auto sig = SignEcdsa(HexStringToField(private_key_hex).ToStandardForm(),
                         PrimeFieldElement::FromBigInt(final_hash.ToStandardForm()), CppIntToBigInt4(get_nonce()));
    auto r = BigInt4ToBigEndianBytes(sig.first.ToStandardForm()); // r is already in standard form

    auto w = sig.second.ToStandardForm();
    auto s = w.InvModPrime(CppIntToBigInt4(K_MODULUS)); // invert w modulo curve_order
    auto s_bytes = BigInt4ToBigEndianBytes(s);
    std::vector<uint8_t> result;
    result.insert(result.end(), r.begin(), r.end());             // r
    result.insert(result.end(), s_bytes.begin(), s_bytes.end()); // s
    return bytes_to_hex(result.data(), result.size());           // NO "0x" prefix!
}

// EdgeX 签名：返回 r||s||v 的 hex 字符串（无 0x 前缀）
std::string sign_edgex_params(const std::string& message, const std::string& private_key_hex,
                              const std::string& pubkey65) {
    // Step 1: Keccak256(message)
    auto hash = "0x" + generate_hash_keccak(message);
    boost::multiprecision::cpp_int msg_hash(hash);
    msg_hash %= K_MODULUS;
    if (msg_hash < 0)
        msg_hash += K_MODULUS;
    auto sig = SignEcdsa(HexStringToField(private_key_hex).ToStandardForm(),
                         PrimeFieldElement::FromBigInt(CppIntToBigInt4(msg_hash)),
                         CppIntToBigInt4(get_nonce()));           // using k=1 for deterministic
    auto r = BigInt4ToBigEndianBytes(sig.first.ToStandardForm()); // r is already in standard form

    auto w = sig.second.ToStandardForm();
    auto s = w.InvModPrime(CppIntToBigInt4(K_MODULUS)); // invert w modulo curve_order
    auto s_bytes = BigInt4ToBigEndianBytes(s);
    std::vector<uint8_t> y = hex_to_bytes(pubkey65); // last 32 bytes
    std::vector<uint8_t> result;
    result.insert(result.end(), r.begin(), r.end());             // r
    result.insert(result.end(), s_bytes.begin(), s_bytes.end()); // s
    result.insert(result.end(), y.begin(), y.end());             // v
    return bytes_to_hex(result.data(), result.size());           // NO "0x" prefix!
}

HttpRequestBody get_request_body_with_sign(boost::beast::http::verb method, const std::string& host,
                                           const std::string& path, const std::string& query,
                                           const AccountSecret& secret, const std::string& payload) {
    std::string url_str{};
    std::string request_body = query;
    using namespace boost::beast;
    if (method == http::verb::get) {
        url_str = query.empty() ? path : (path + "?" + query);
    } else if (method == http::verb::post) {
        url_str = path;
    }

    std::string raw_str{};
    std::string timestamp = std::to_string(time_get_now_milli());
    std::string method_text = "";
    if (method == http::verb::get) {
        method_text = "GET";
    } else if (method == http::verb::post) {
        method_text = "POST";
    }

    raw_str.append(timestamp).append(method_text).append(path);
    if (!payload.empty()) {
        raw_str.append(payload);
    } else if (!query.empty()) {
        raw_str.append(query);
    }
    std::string signature = sign_edgex_params(raw_str, secret.api_secret, secret.api_phrase);
    std::transform(signature.begin(), signature.end(), signature.begin(), ::tolower);

    HttpRequestBody req{method, url_str, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http::field::connection, "close");
    req.set("X-edgeX-Api-Signature", signature);
    req.set("X-edgeX-Api-Timestamp", timestamp);

    if (method == http::verb::get) {
        req.set(http::field::content_type, "application/x-www-form-urlencoded");
    } else if (method == http::verb::post) {
        req.set(http::field::content_type, "application/json");
        req.body() = request_body;
        req.prepare_payload();
    }
    return req;
}

void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res) {
    simdjson::dom::object data = doc["data"];
    simdjson::dom::array data_array = data["collateralAssetModelList"];
    for (auto item : data_array) {
        std::string_view coinId_text = item["coinId"];
        std::string currency_text;
        if (!get_symbol_by_contract_id(std::string(coinId_text), currency_text)) {
            continue;
        }
        if (!compare_currency(Currency(currency_text), currency)) {
            continue;
        }
        double total = str_to_float(item["totalEquity"]);
        double available = str_to_float(item["availableAmount"]);
        double withdrawable = str_to_float(item["pendingWithdrawAmount"]);
        auto account_asset = std::make_shared<Balance>(currency, available, total - available);
        account_asset->withdraw = withdrawable;
        res[account_asset->currency] = account_asset;
    }
}

void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res) {
    simdjson::dom::array data_array = doc["data"];
    for (auto item : data_array) {
        std::string_view contractId_text = item["contractId"];
        Symbol pair;
        if (!get_symbol_by_contract_id(std::string(contractId_text), pair)) {
            continue;
        }
        std::string_view openSize = item["openSize"];
        std::string_view openValue = item["openValue"];
        SpPosition pos_info{nullptr};
        auto it = res.find(pair);
        if (it == res.end()) {
            pos_info = std::make_shared<Position>();
            double position_amount = str_to_float(openSize);
            double position_value = str_to_float(openValue);
            if (position_amount > 0) {
                pos_info->long_size = position_amount;
                pos_info->long_open_price = position_value / position_amount;
            } else if (position_amount < 0) {
                pos_info->short_size = -position_amount; // 取正数
                pos_info->short_open_price = position_value / pos_info->short_size;
            }
            pos_info->position_mode = PositionMode::one_way_mode;
            pos_info->symbol = pair;
            pos_info->update_time = time_get_now_milli();
            res[pos_info->symbol] = pos_info;
        } else {
            pos_info = it->second;
            pos_info->update_time = time_get_now_milli();
        }
    }
}

SpOrder parse_rtn_order(const simdjson::dom::object& obj) {
    std::string_view contractId_text = obj["contractId"];
    Symbol pair;
    if (!get_symbol_by_contract_id(std::string(contractId_text), pair)) {
        return nullptr;
    }
    std::string_view client_order_id = obj["clientOrderId"];
    std::string_view price = obj["price"];
    std::string_view size = obj["size"];
    std::string_view deal_size = obj["cumMatchSize"];
    std::string_view deal_value = obj["cumMatchValue"];
    std::string_view order_status_text = obj["status"];
    std::string_view create_milli = obj["createdTime"];
    std::string_view update_milli = obj["updatedTime"];
    ClientOrderId client_oid(client_order_id);
    auto rtn_order = std::make_shared<Order>(pair, client_oid, "");

    double filled_qty = str_to_float(deal_size);
    double filled_value = str_to_float(deal_value);
    double deal_avg_price = (filled_qty > 0.0) ? (filled_value / filled_qty) : double(0.0);
    std::string status_text(order_status_text);
    OrderStatus order_status;
    if (status_text == "CANCELING") {
        order_status = OrderStatus::Canceling;
    } else {
        order_status = to_order_status(status_text);
    }

    rtn_order->status = order_status;
    rtn_order->price = str_to_float(price);
    rtn_order->quantity = str_to_float(size);
    rtn_order->avg_price = deal_avg_price;
    rtn_order->cum_deal_base = filled_qty;
    rtn_order->cum_deal_quote = filled_value;
    rtn_order->exchange_create_time = std::stoll(std::string(create_milli));
    rtn_order->exchange_update_time = std::stoll(std::string(update_milli));
    return rtn_order;
}

SpFundingFee parse_funding_fee(const simdjson::dom::element& doc) {
    simdjson::dom::array symbols_array = doc["data"];
    if (symbols_array.size() == 0) {
        return nullptr;
    }
    simdjson::dom::object obj = *(symbols_array.begin());
    std::string_view symbol = obj["contractName"];
    std::string_view fee_text = obj["fundingRate"];
    std::string_view next_milli = obj["nextFundingTime"];
    double fee = str_to_float(fee_text);
    Timestamp milli = time_get_now_milli();
    std::string pair = transfer_to_infra_pair(symbol);
    return std::make_shared<FundingFee>(pair, milli, fee, std::stoll(std::string(next_milli)), 0);
}

void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency) {
    g_pairs_info_cache.clear();
    g_all_symbols.clear();
    coinid_to_currency_map.clear();
    g_stark_info_cache.clear();
    simdjson::dom::object data = doc["data"];
    simdjson::dom::array coin_array = data["coinList"];
    for (auto coin_item : coin_array) {
        if (coin_item["starkExAssetId"].error() != simdjson::SUCCESS || coin_item["starkExAssetId"].is_null())
            continue;
        auto stark_info = std::make_shared<starkInfo>();
        std::string_view stackAssetId_text = coin_item["starkExAssetId"];
        std::string_view starkResolution_text = coin_item["starkExResolution"];
        cpp_int stackAssetId(stackAssetId_text);
        stark_info->starkAssetId = stackAssetId;
        cpp_int starkResolution(starkResolution_text);
        stark_info->starkResolution = starkResolution;

        std::string_view coinName_text = coin_item["coinName"];
        Currency coinName(coinName_text);
        std::transform(coinName.begin(), coinName.end(), coinName.begin(), ::tolower);
        std::string_view coinId_text = coin_item["coinId"];
        coinid_to_currency_map[std::string(coinId_text)] = coinName;
        g_stark_info_cache[coinName] = stark_info;
    }
    simdjson::dom::array symbols_array = data["contractList"];
    for (auto symbol_item : symbols_array) {
        bool state = symbol_item["enableTrade"].get_bool();
        std::string_view symbol_text = symbol_item["contractName"];
        std::string_view constractId_text = symbol_item["contractId"];
        std::string_view minOrderSize = symbol_item["minOrderSize"];
        std::string_view stepSize = symbol_item["stepSize"];
        std::string_view tickSize = symbol_item["tickSize"];
        std::string_view settleCcy = symbol_item["quoteCoinId"];
        std::string_view stackAssetId_text = symbol_item["starkExSyntheticAssetId"];
        std::string_view starkResolution_text = symbol_item["starkExResolution"];
        std::string_view takerFee_text = symbol_item["defaultTakerFeeRate"];
        auto it = coinid_to_currency_map.find(std::string(settleCcy));
        if (it == coinid_to_currency_map.end())
            continue;
        std::string quote(it->second);
        if (!state || !compare_currency(quote, currency))
            continue;

        std::string pair = transfer_to_infra_pair(symbol_text);
        SpExPairInfo pair_info = std::make_shared<ExchangePairInfo>();
        pair_info->pair = pair;
        pair_info->alias = constractId_text;
        pair_info->trading_min_base = str_to_float(minOrderSize);
        pair_info->step_size_base = str_to_float(stepSize);
        pair_info->step_size_quote = str_to_float(tickSize);
        g_pairs_info_cache[pair] = pair_info;
        coinid_to_currency_map[pair_info->alias] = pair;

        auto stark_info = std::make_shared<starkInfo>();
        cpp_int stackAssetId(stackAssetId_text);
        cpp_int starkResolution(starkResolution_text);
        stark_info->starkAssetId = stackAssetId;
        stark_info->starkResolution = starkResolution;
        stark_info->takerFee = str_to_float(takerFee_text);
        g_stark_info_cache[pair] = stark_info;
        g_all_symbols.push_back(std::move(pair));
    }
}

bool get_contract_id(const Symbol& pair, std::string& contract_id) {
    auto it = g_pairs_info_cache.find(pair);
    if (it != g_pairs_info_cache.end()) {
        contract_id = it->second->alias;
        return true;
    }
    return false;
}

bool get_symbol_by_contract_id(const std::string& contract_id, Symbol& pair) {
    auto it = coinid_to_currency_map.find(contract_id);
    if (it != coinid_to_currency_map.end()) {
        pair = it->second;
        return true;
    }
    return false;
}
} // namespace infra::edgex