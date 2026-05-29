#include "hyperliquid_utils.h"
#include <nlohmann/json.hpp>
#include "exchanges/signature_dex.h"

namespace {
// 大端序转换
std::vector<uint8_t> to_bytes_big_endian(uint64_t value) {
    std::vector<uint8_t> result(8);
    for (int i = 7; i >= 0; --i) {
        result[i] = static_cast<uint8_t>(value & 0xFF);
        value >>= 8;
    }
    return result;
}

// 地址转换函数
std::vector<uint8_t> address_to_bytes(const std::string& address) {
    std::vector<uint8_t> bytes;
    std::string addr_clean = address.compare(0, 2, "0x") == 0 ? address.substr(2) : address;
    for (size_t i = 0; i < addr_clean.length(); i += 2) {
        uint8_t b = static_cast<uint8_t>(std::stoi(addr_clean.substr(i, 2), nullptr, 16));
        bytes.push_back(b);
    }
    return bytes;
}
} // namespace

namespace infra::hyperliquid {
Errno extract_error_code(std::string_view sv) {
    if (sv.find("timeout") != std::string_view::npos) {
        return Errno::RequestTimeout;
    } else if (sv.find("API-key") != std::string_view::npos) {
        return Errno::AuthFailed;
    } else if (sv.find("Insufficient") != std::string_view::npos) {
        return Errno::InsufficientBalance;
    } else if (sv.find("price cannot be more than") != std::string_view::npos) {
        return Errno::SmallSizeOrder;
    } else if (sv.find("Post only order") != std::string_view::npos) {
        return Errno::PostOnlyRejected;
    } else if (sv.find("unknownOid") != std::string_view::npos) {
        return Errno::OrderNotFound;
    } else if (sv.find("minimum value") != std::string_view::npos) {
        return Errno::SmallSizeOrder;
    } else {
        return Errno::UnknownError;
    }
}

Currency get_right_currency(const Currency& currency) {
    std::string str = currency;
    std::transform(str.begin(), str.end(), str.begin(), ::tolower);
    return str;
}

double parse_margin_ratio(const simdjson::dom::element& doc) {
    std::string_view withdrawable = doc["withdrawable"];
    std::string_view accountValue = doc["marginSummary"]["accountValue"];
    double available = str_to_float(withdrawable);
    double equity = str_to_float(accountValue);
    double margin = equity - available;
    if (margin <= 0.0) {
        return 999.0;
    }
    return equity / margin;
}

void parse_balance(const simdjson::dom::element& doc, const Currency& currency, UMCurrencyBalance& res) {
    res.clear();
    std::string asset("usdc"); // 仅支持USDC
    std::string_view withdrawable = doc["withdrawable"];
    std::string_view accountValue = doc["marginSummary"]["accountValue"];
    double available = str_to_float(withdrawable);
    double frozen = str_to_float(accountValue) - available;

    auto balance_info = std::make_shared<Balance>(asset, available, frozen);
    balance_info->withdraw = balance_info->available;
    res[balance_info->currency] = balance_info;
}

void parse_position(const simdjson::dom::element& doc, UMSymbolPosition& res) {
    res.clear();
    simdjson::dom::array array = doc["assetPositions"];
    for (auto pos_item : array) {
        // std::string_view pos_type = pos_item["type"]; // oneWay
        simdjson::dom::object item = pos_item["position"];

        std::string_view coin = item["coin"];
        std::string_view szi = item["szi"];
        std::string_view entryPx = item["entryPx"];
        std::string_view liquidationPx = item["liquidationPx"];
        std::string_view type = item["leverage"]["type"];
        int64_t leverage = item["leverage"]["value"];

        double entry_price = str_to_float(entryPx);
        double position_amount = str_to_float(szi);
        Symbol pair = transfer_to_infra_pair(coin);
        SpPosition pos_info{nullptr};
        auto it = res.find(pair);
        if (it == res.end()) {
            pos_info = std::make_shared<Position>();
            pos_info->position_mode = PositionMode::one_way_mode; // 仅支持单向持仓
            pos_info->margin_mode = (type == "isolated") ? MarginMode::ISOLATED : MarginMode::CROSS;
            pos_info->symbol = pair;
            pos_info->bankrupt_price = str_to_float(liquidationPx);
            pos_info->leverage = leverage;
            pos_info->update_time = time_get_now_milli();
            res[pos_info->symbol] = pos_info;
        } else {
            pos_info = it->second;
            pos_info->update_time = time_get_now_milli();
        }

        if (position_amount > 0) {
            pos_info->long_size = position_amount;
            pos_info->long_open_price = entry_price;
        } else if (position_amount < 0) {
            pos_info->short_size = -position_amount;
            pos_info->short_open_price = entry_price;
        }
    }
}

SpOrder parse_rtn_order(const simdjson::dom::object& obj) {
    simdjson::dom::object item = obj["order"];
    std::string_view status = obj["status"];
    Timestamp statusTimestamp = obj["statusTimestamp"];
    std::string_view coin = item["coin"];
    int64_t oid = item["oid"];
    std::string_view limitPx = item["limitPx"];
    std::string_view origSz = item["origSz"];
    Timestamp timestamp = item["timestamp"];

    Symbol pair = transfer_to_infra_pair(coin);
    ClientOrderId client_oid{};
    if (item["cloid"].error() == simdjson::SUCCESS) {
        std::string_view cloid = item["cloid"];
        client_oid = cloid;
    }
    OrderId market_oid = std::to_string(oid);
    auto rtn_order = std::make_shared<Order>(pair, client_oid, market_oid);

    OrderStatus order_status = OrderStatus::New;
    if (status == "open") {
        order_status = OrderStatus::New;
    } else if (status == "filled") {
        order_status = OrderStatus::Filled;
    } else if (status == "canceled") {
        order_status = OrderStatus::Canceled;
    } else if (status == "iocCancelRejected") {
        order_status = OrderStatus::Rejected;
    }

    rtn_order->status = order_status;
    rtn_order->price = str_to_float(limitPx);
    rtn_order->quantity = str_to_float(origSz);
    rtn_order->exchange_create_time = timestamp;
    rtn_order->exchange_update_time = statusTimestamp;
    return rtn_order;
}

SpFundingFee parse_funding_fee(const simdjson::dom::element& doc, const Symbol& sym) {
    simdjson::dom::array base_array = doc.get_array();
    simdjson::dom::object infos = base_array.at(0);
    simdjson::dom::array fee_array = base_array.at(1);
    simdjson::dom::array array = infos["universe"];
    for (size_t i = 0; i < array.size(); ++i) {
        simdjson::dom::object item = array.at(i);
        std::string_view name = item["name"];
        Symbol pair = transfer_to_infra_pair(name);
        if (compare_currency(pair, sym)) {
            auto fee_item = fee_array.at(i);
            std::string_view funding = fee_item["funding"];
            double fee = str_to_float(funding);
            return std::make_shared<FundingFee>(pair, time_get_now_milli(), fee, 0, 0);
        }
    }
    return nullptr;
}

void parse_pairs_info(const simdjson::dom::element& doc, const Currency& currency) {
    g_pairs_info_cache.clear();
    g_all_symbols.clear();
    simdjson::dom::array array = doc["universe"];
    size_t asset_id = 0;
    for (auto item : array) {
        std::string_view name = item["name"];
        int64_t szDecimals = item["szDecimals"];

        Symbol pair = transfer_to_infra_pair(name);
        auto pair_info = std::make_shared<ExchangePairInfo>();
        pair_info->pair = pair;
        pair_info->step_size_base = get_step_by_decimals(szDecimals);
        pair_info->step_size_quote = get_step_by_decimals(std::min(4L, 5 - szDecimals));
        pair_info->trading_min_base = pair_info->step_size_base;
        pair_info->min_size_quote = double(10);
        pair_info->alias = std::to_string(asset_id);

        g_pairs_info_cache[pair] = pair_info;
        g_all_symbols.push_back(std::move(pair));
        asset_id++;
    }
}

// 哈希计算函数
std::string get_action_hash_hex(const std::string& action, const std::string* vault_address, uint64_t nonce,
                                const uint64_t* expires_after) {
    // 1. MsgPack序列化
    using ordered_json = nlohmann::ordered_json;
    ordered_json action_obj = ordered_json::parse(action);
    std::vector<uint8_t> data = ordered_json::to_msgpack(action_obj);

    // 2. 追加nonce（8字节大端序）
    auto nonce_bytes = to_bytes_big_endian(nonce);
    data.insert(data.end(), nonce_bytes.begin(), nonce_bytes.end());

    // 3. 处理vault_address
    if (!vault_address) {
        data.push_back(0x00);
    } else {
        data.push_back(0x01);
        auto addr_bytes = address_to_bytes(*vault_address);
        data.insert(data.end(), addr_bytes.begin(), addr_bytes.end());
    }

    // 4. 处理expires_after
    if (expires_after) {
        data.push_back(0x00);
        auto expire_bytes = to_bytes_big_endian(*expires_after);
        data.insert(data.end(), expire_bytes.begin(), expire_bytes.end());
    }

    // 5. 使用现有的Keccak256实现计算哈希
    auto hash = generate_hash_keccak(data);
    return "0x" + bytes_to_hex(hash.data(), hash.size());
}

// NOTE: EIP712签名格式，取值参考官方Python SDK代码
void sign_action(int64_t nonce, const std::string& action, const AccountSecret& secret, EcdsaSignature& sign) {
    auto header = eip712_domain_encode("Exchange", "1", "1337", "0x0000000000000000000000000000000000000000");

    std::string primary_type = "Agent(string source,bytes32 connectionId)";
    std::string type_hash_hex = generate_hash_keccak_hex(primary_type);
    std::string source_hash_hex = generate_hash_keccak_hex("a");
    std::string action_hash_hex = get_action_hash_hex(action, nullptr, nonce, nullptr);
    std::vector<std::string> types = {"bytes32", "bytes32", "bytes32"};
    std::vector<std::string> args = {type_hash_hex, source_hash_hex, action_hash_hex};
    auto body = eip712_struct_encode(types, args);

    auto message = eip712_message(header, body);
    sign = generate_sign_ecdsa(message, secret.api_secret);
}
} // namespace infra::hyperliquid