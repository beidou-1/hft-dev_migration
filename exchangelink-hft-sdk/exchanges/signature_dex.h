#pragma once

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>
#include <ethash/ethash.hpp>
#include <ethash/keccak.hpp>
#include <ethash/hash_types.hpp>
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include "common/structs.h"
#include <boost/multiprecision/cpp_int.hpp>

namespace infra {
inline std::string generate_hash_keccak_hex(const std::string& data) {
    const uint8_t* msg = reinterpret_cast<const uint8_t*>(data.data());
    auto hash = ethash::keccak256(msg, data.size());
    auto result = std::vector<uint8_t>(hash.bytes, hash.bytes + sizeof(hash.bytes));
    return bytes_to_hex(result.data(), result.size());
}

inline std::vector<uint8_t> generate_hash_keccak(const std::string& data) {
    const uint8_t* msg = reinterpret_cast<const uint8_t*>(data.data());
    auto hash = ethash::keccak256(msg, data.size());
    return std::vector<uint8_t>(hash.bytes, hash.bytes + sizeof(hash.bytes));
}

inline std::vector<uint8_t> generate_hash_keccak(const std::vector<uint8_t>& data) {
    auto hash = ethash::keccak256(data.data(), data.size());
    return std::vector<uint8_t>(hash.bytes, hash.bytes + sizeof(hash.bytes));
}

inline std::vector<uint8_t> encode_address(const std::string& value) {
    auto bytes = hex_to_bytes(value);
    std::vector<uint8_t> result(32, 0); // 地址需要左对齐（高位补零）
    std::copy(bytes.begin(), bytes.end(), result.end() - bytes.size());
    return result;
}

inline std::vector<uint8_t> encode_bytes32(const std::string& value) { return hex_to_bytes(value); }

inline std::vector<uint8_t> encode_bytes32_array(const std::vector<std::string>& values) {
    std::vector<uint8_t> result;
    for (const auto& value : values) {
        std::vector<uint8_t> element_encoded = encode_bytes32(value);
        result.insert(result.end(), element_encoded.begin(), element_encoded.end());
    }
    return generate_hash_keccak(result);
}

inline std::vector<uint8_t> encode_uint(const std::string& value) {
    // 判断是否是十六进制格式（带0x前缀）
    bool is_hex = (value.size() >= 2 && value.substr(0, 2) == "0x");

    uint64_t num;
    if (is_hex) {
        // 十六进制解析
        std::stringstream ss;
        ss << std::hex << value.substr(2);
        ss >> num;
    } else {
        // 十进制解析
        num = std::stoull(value);
    }

    // 转换为32字节大端序
    std::vector<uint8_t> bytes(32, 0);
    for (int i = 31; i >= 0 && num > 0; i--) {
        bytes[i] = num & 0xFF;
        num >>= 8;
    }
    return bytes;
}

inline std::vector<uint8_t> encode_uint32_array(std::vector<std::string>& values) {
    std::vector<uint8_t> result;
    for (const auto& value : values) {
        std::vector<uint8_t> element_encoded = encode_uint(value);
        result.insert(result.end(), element_encoded.begin(), element_encoded.end());
    }
    return generate_hash_keccak(result);
}

template <typename T> std::vector<uint8_t> encode_int128(T value) {
    using namespace boost::multiprecision;
    bool is_hex = (value.size() >= 2 && value.substr(0, 2) == "0x");
    bool is_negative = (!value.empty() && value[0] == '-');

    cpp_int num;
    try {
        if (is_hex) {
            // 处理十六进制
            std::string hex_str;
            if (is_negative) {
                hex_str = value.substr(3); // 去掉 "-0x"
            } else {
                hex_str = value.substr(2); // 去掉 "0x"
            }

            // 移除可能的下划线（大数字的可读性分隔符）
            hex_str.erase(std::remove(hex_str.begin(), hex_str.end(), '_'), hex_str.end());

            if (hex_str.empty()) {
                throw std::invalid_argument("Invalid hex format");
            }

            // 使用boost的十六进制解析
            num = cpp_int("0x" + hex_str);
            if (is_negative) {
                num = -num;
            }
        } else {
            // 处理十进制
            // 移除可能的下划线
            std::string dec_str = value;
            dec_str.erase(std::remove(dec_str.begin(), dec_str.end(), '_'), dec_str.end());

            num = cpp_int(dec_str);
        }
    } catch (const std::exception& e) {
        throw std::invalid_argument("Failed to parse integer: " + std::string(e.what()));
    }

    // 转换为32字节（256位）大端序补码
    const size_t BYTES_SIZE = 32;
    std::vector<uint8_t> bytes(BYTES_SIZE, 0);

    // 将cpp_int导出为字节数组
    cpp_int mask = 0xFF;

    // 对于有符号256位整数，我们需要处理补码
    // 先获取无符号表示的字节
    cpp_int unsigned_num = num;
    if (num < 0) {
        // 计算256位的补码：2^256 - |num|
        cpp_int modulus = cpp_int(1) << (BYTES_SIZE * 8);
        unsigned_num = modulus + num; // num是负数
    }

    // 提取字节（大端序）
    for (int i = BYTES_SIZE - 1; i >= 0; i--) {
        bytes[i] = static_cast<uint8_t>((unsigned_num & mask).convert_to<unsigned>());
        unsigned_num >>= 8;
    }

    return bytes;
}

inline std::vector<uint8_t> abi_encode(const std::vector<std::string>& types, const std::vector<std::string>& args) {
    if (types.size() != args.size()) {
        throw std::runtime_error("Types and arguments count mismatch");
    }
    std::vector<uint8_t> result;
    for (size_t i = 0; i < types.size(); i++) {
        const auto& type = types[i];
        const auto& arg = args[i];
        std::vector<uint8_t> encoded;
        if (type == "uint64") {
            encoded = encode_uint(arg);
        } else if (type == "uint128") {
            encoded = encode_uint(arg);
        } else if (type == "uint256") {
            encoded = encode_uint(arg);
        } else if (type == "int128") {
            encoded = encode_int128(arg);
        } else if (type == "address") {
            encoded = encode_address(arg);
        } else if (type == "bytes32") {
            encoded = encode_bytes32(arg);
        } else if (type == "bytes32[]") {
            std::vector<std::string> value{arg};
            encoded = encode_bytes32_array(value);
        } else if (type == "uint32[]") {
            std::vector<std::string> value{arg};
            encoded = encode_uint32_array(value);
        } else {
            throw std::runtime_error("Unsupported type: " + type);
        }
        result.insert(result.end(), encoded.begin(), encoded.end());
    }
    return result;
}

inline std::vector<uint8_t> eip712_struct_encode(const std::vector<std::string>& types,
                                                 const std::vector<std::string>& args) {
    auto encoded_data = abi_encode(types, args);
    return generate_hash_keccak(encoded_data);
}

inline std::vector<uint8_t> eip712_domain_encode(const std::string& name, const std::string& version,
                                                 const std::string& chain_id, const std::string& contract) {
    std::string domain = "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";
    std::string domain_hash_hex = generate_hash_keccak_hex(domain);
    std::string name_hash_hex = generate_hash_keccak_hex(name);
    std::string version_hash_hex = generate_hash_keccak_hex(version);

    std::vector<std::string> types = {"bytes32", "bytes32", "bytes32", "uint256", "address"};
    std::vector<std::string> args = {domain_hash_hex, name_hash_hex, version_hash_hex, chain_id, contract};
    return eip712_struct_encode(types, args);
}

inline std::vector<uint8_t> eip712_message(const std::vector<uint8_t>& header, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> joined;
    joined.reserve(2 + header.size() + body.size());
    joined.push_back(0x19);
    joined.push_back(0x01);
    joined.insert(joined.end(), header.begin(), header.end());
    joined.insert(joined.end(), body.begin(), body.end());
    return generate_hash_keccak(joined);
}

// NOTE: 对应Python函数: eth_account.messages.encode_defunct
inline std::vector<uint8_t> eip191_message(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> joined;
    std::string len = std::to_string(data.size());
    const std::string prefix = std::string("\x19") + "Ethereum Signed Message:\n";
    joined.insert(joined.end(), prefix.begin(), prefix.end());
    joined.insert(joined.end(), len.begin(), len.end());
    joined.insert(joined.end(), data.begin(), data.end());
    return generate_hash_keccak(joined);
}

inline EcdsaSignature generate_sign_ecdsa(const std::vector<uint8_t>& msg_hash, const std::string& private_key_hex) {
    using PrivateKey = std::array<uint8_t, 32>;
    using Signature = std::array<uint8_t, 64>;
    using EthSignature = std::array<uint8_t, 65>;

    auto ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    PrivateKey private_key;
    auto key_bytes = hex_to_bytes(private_key_hex);
    if (key_bytes.size() != 32) {
        throw std::runtime_error("Private key must be 32 bytes");
    }
    std::copy(key_bytes.begin(), key_bytes.end(), private_key.begin());

    // 创建可恢复签名
    secp256k1_ecdsa_recoverable_signature recoverable_sig;
    if (secp256k1_ecdsa_sign_recoverable(ctx, &recoverable_sig, msg_hash.data(), private_key.data(), nullptr,
                                         nullptr) != 1) {
        throw std::runtime_error("Failed to create recoverable signature");
    }

    // 序列化签名并获取恢复ID
    Signature signature_bytes;
    int recovery_id;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(ctx, signature_bytes.data(), &recovery_id,
                                                            &recoverable_sig);

    // 转换为以太坊格式
    std::array<uint8_t, 32> r, s;
    std::copy(signature_bytes.begin(), signature_bytes.begin() + 32, r.begin());
    std::copy(signature_bytes.begin() + 32, signature_bytes.end(), s.begin());
    uint8_t v = static_cast<uint8_t>(recovery_id + 27); // 以太坊v值为27或28

    // 构建65字节以太坊签名
    EthSignature eth_sig;
    std::copy(r.begin(), r.end(), eth_sig.begin());
    std::copy(s.begin(), s.end(), eth_sig.begin() + 32);
    eth_sig[64] = v;

    return {"0x" + bytes_to_hex(r.data(), r.size()), "0x" + bytes_to_hex(s.data(), s.size()), v,
            "0x" + bytes_to_hex(eth_sig.data(), eth_sig.size())};
}
} // namespace infra