#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_streambuf.hpp>

namespace {
const EVP_MD* choose_algo(std::string_view name) {
    const EVP_MD* engine = nullptr;
    if (name == "sha512") {
        engine = EVP_sha512();
    } else if (name == "sha256") {
        engine = EVP_sha256();
    } else if (name == "sha384") {
        engine = EVP_sha384();
    } else if (name == "md5") {
        engine = EVP_md5();
    } else {
        assert(false);
    }
    return engine;
}

std::vector<uint8_t> hash_with_algo(const std::string& data, std::string_view name) {
    unsigned int len = EVP_MAX_MD_SIZE;
    std::vector<uint8_t> out(EVP_MAX_MD_SIZE);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, choose_algo(name), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    EVP_DigestFinal_ex(ctx, out.data(), &len);
    EVP_MD_CTX_free(ctx);
    out.resize(len);
    return out;
}

std::vector<uint8_t> hmac_with_algo(const std::string& key, const std::string& data, std::string_view name) {
    unsigned int len = EVP_MAX_MD_SIZE;
    std::vector<uint8_t> out(EVP_MAX_MD_SIZE);
    HMAC(choose_algo(name), reinterpret_cast<const uint8_t*>(key.data()), static_cast<int>(key.size()),
         reinterpret_cast<const uint8_t*>(data.data()), static_cast<int>(data.size()), out.data(), &len);
    out.resize(len);
    return out;
}
} // namespace

namespace infra {
inline std::string decompress_gzip(std::string_view msg) {
    std::string compressed_str(msg);
    std::stringstream compressed_stream(compressed_str);
    std::stringstream decompressed_stream;

    boost::iostreams::filtering_streambuf<boost::iostreams::input> in;
    in.push(boost::iostreams::gzip_decompressor());
    in.push(compressed_stream);

    boost::iostreams::copy(in, decompressed_stream);
    return decompressed_stream.str();
}

inline std::string bytes_to_hex(const uint8_t* data, size_t len) {
    std::string result{};
    result.reserve(len * 2);
    static constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        result.push_back(hex[data[i] >> 4]);
        result.push_back(hex[data[i] & 0x0F]);
    }
    return result;
}

inline std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("Hex string must have even length");
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    size_t start = (hex.find("0x") != std::string::npos) ? 2 : 0;
    for (size_t i = start; i < hex.size(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byteString, nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

inline std::string base64_encode(const uint8_t* data, size_t len) {
    BIO *bio, *b64;
    BUF_MEM* bufferPtr;
    bio = BIO_new(BIO_s_mem());
    b64 = BIO_new(BIO_f_base64());

    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

    bio = BIO_push(b64, bio);
    BIO_write(bio, data, static_cast<int>(len));
    BIO_flush(bio);
    BIO_get_mem_ptr(bio, &bufferPtr);

    std::string b64text(bufferPtr->data, bufferPtr->length);
    BIO_free_all(bio);
    return b64text;
}

inline std::string base64_decode(const std::string& encoded) {
    BIO *bio, *b64;
    int decodeLen = encoded.size() * 3 / 4 + 4;
    char* buffer = new char[decodeLen];
    memset(buffer, 0, decodeLen);
    bio = BIO_new_mem_buf(encoded.c_str(), -1);
    b64 = BIO_new(BIO_f_base64());

    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

    bio = BIO_push(b64, bio);
    int len = BIO_read(bio, buffer, encoded.size());
    BIO_free_all(bio);

    std::string result(buffer, len);
    delete[] buffer;
    return result;
}

inline std::string base58_encode(const std::vector<std::uint8_t>& input) {
    static const char* alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

    std::vector<std::uint8_t> digits((input.size() * 138) / 100 + 1, 0);
    std::size_t length = 0;

    for (std::uint8_t byte : input) {
        int carry = byte;
        for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
            carry += 256 * (*it);
            *it = static_cast<std::uint8_t>(carry % 58);
            carry /= 58;
        }

        if (carry != 0) {
            return "";
        }

        while (length < digits.size() && digits[digits.size() - 1 - length] != 0) {
            ++length;
        }
    }

    std::size_t leading_zeroes = 0;
    while (leading_zeroes < input.size() && input[leading_zeroes] == 0) {
        ++leading_zeroes;
    }

    std::string result(leading_zeroes, '1');
    const auto start = digits.end() - static_cast<std::ptrdiff_t>(length);
    for (auto it = start; it != digits.end(); ++it) {
        result.push_back(alphabet[*it]);
    }
    return result;
}

inline std::vector<uint8_t> base58_decode(const std::string& input) {
    static const char* alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    static const size_t base = 58;

    std::vector<uint8_t> result;
    result.reserve(input.size() * 733 / 1000 + 1);

    for (char ch : input) {
        const char* pos = strchr(alphabet, ch);
        if (!pos)
            return {};

        size_t carry = static_cast<size_t>(pos - alphabet);

        for (size_t i = 0; i < result.size(); ++i) {
            carry += static_cast<size_t>(result[i]) * base;
            result[i] = static_cast<uint8_t>(carry & 0xFF);
            carry >>= 8;
        }
        while (carry > 0) {
            result.push_back(static_cast<uint8_t>(carry & 0xFF));
            carry >>= 8;
        }
    }

    size_t leading_zeros = 0;
    while (leading_zeros < input.size() && input[leading_zeros] == '1')
        ++leading_zeros;
    result.insert(result.begin(), leading_zeros, 0u);

    std::reverse(result.begin(), result.end());
    return result;
}

inline std::string url_encode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex << std::uppercase;
    for (uint8_t c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return escaped.str();
}

inline std::string generate_hash_md5(const std::string& data) {
    auto result = hash_with_algo(data, "md5");
    return bytes_to_hex(result.data(), result.size());
}

inline std::string generate_hash_sha256(const std::string& data) {
    auto result = hash_with_algo(data, "sha256");
    return bytes_to_hex(result.data(), result.size());
}

inline std::string generate_hash_sha512(const std::string& data) {
    auto result = hash_with_algo(data, "sha512");
    return bytes_to_hex(result.data(), result.size());
}

inline std::string generate_sign_hmac384(const std::string& secret, const std::string& data) {
    auto result = hmac_with_algo(secret, data, "sha384");
    return bytes_to_hex(result.data(), result.size());
}

inline std::string generate_sign_hmac256(const std::string& secret, const std::string& data) {
    auto result = hmac_with_algo(secret, data, "sha256");
    return bytes_to_hex(result.data(), result.size());
}

inline std::string generate_sign_hmac256_b64(const std::string& secret, const std::string& data) {
    auto result = hmac_with_algo(secret, data, "sha256");
    return base64_encode(result.data(), result.size());
}

inline std::string generate_sign_hmac512(const std::string& secret, const std::string& data) {
    auto result = hmac_with_algo(secret, data, "sha512");
    return bytes_to_hex(result.data(), result.size());
}

inline std::string generate_sign_hmac512_b64(const std::string& secret, const std::string& data) {
    auto result = hmac_with_algo(secret, data, "sha512");
    return base64_encode(result.data(), result.size());
}

inline std::string generate_sign_ed25519_file(const std::string& pkey_str, const std::string& data) {
    BIO* bio = BIO_new_mem_buf(pkey_str.c_str(), pkey_str.size());
    if (!bio) {
        throw std::runtime_error("Failed to create BIO from memory");
    }
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        throw std::runtime_error("Failed to read private key from PEM string");
    }

    size_t len = 64;
    std::vector<uint8_t> out(64);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) <= 0) {
        throw std::runtime_error("Failed to initialize signature context");
    }

    if (EVP_DigestSign(ctx, out.data(), &len, reinterpret_cast<const uint8_t*>(data.data()), data.size()) <= 0) {
        throw std::runtime_error("Failed to sign data");
    }
    if (len != 64) {
        throw std::runtime_error("Unexpected signature length");
    }

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return base64_encode(out.data(), len);
}

inline std::string generate_sign_ed25519_b64(const std::string& secret, const std::string& data) {
    std::string priv_str = base64_decode(secret);
    if (priv_str.size() != 32) {
        throw std::runtime_error("Private key must be 32 bytes");
    }

    std::vector<uint8_t> priv(32);
    std::copy(priv_str.begin(), priv_str.end(), priv.begin());

    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, priv.data(), 32);
    if (!pkey) {
        throw std::runtime_error("Failed to create private key");
    }

    size_t len = 64;
    std::vector<uint8_t> out(64);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey);
    if (EVP_DigestSign(ctx, out.data(), &len, reinterpret_cast<const uint8_t*>(data.data()), data.size()) != 1) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Failed to sign data");
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return base64_encode(out.data(), len);
}

inline std::string generate_sign_ed25519_b58(const std::string& secret, const std::string& data) {
    std::string api_secret;
    if (secret.size() >= 8 && secret.substr(0, 8) == "ed25519:") {
        api_secret = secret.substr(8); // 纯 Base58 的 Secret（去掉 ed25519:）
    } else {
        api_secret = secret;
    }

    auto seed = base58_decode(api_secret);
    if (seed.size() != 32) {
        throw std::runtime_error("Private key must be 32 bytes");
    }

    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), 32);
    if (!pkey) {
        throw std::runtime_error("Failed to create private key");
    }

    size_t len = 64;
    std::vector<uint8_t> out(64);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey);
    if (EVP_DigestSign(ctx, out.data(), &len, reinterpret_cast<const uint8_t*>(data.data()), data.size()) != 1) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Failed to sign data");
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);

    std::string result = base64_encode(out.data(), len);

    std::replace(result.begin(), result.end(), '+', '-');
    std::replace(result.begin(), result.end(), '/', '_');
    while (!result.empty() && result.back() == '=') {
        result.pop_back();
    }
    return result;
}
} // namespace infra