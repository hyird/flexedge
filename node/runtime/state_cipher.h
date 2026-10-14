#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace flexedge::node {

class state_cipher final {
public:
    explicit state_cipher(std::string_view device_key_material) {
        if (device_key_material.empty()) {
            throw std::runtime_error("node device key material is empty");
        }
        std::string material("flexedge-node-state-v1:");
        material.append(device_key_material);
        unsigned int digest_size{};
        if (EVP_Digest(material.data(), material.size(), key_.data(), &digest_size, EVP_sha256(),
                nullptr) != 1 ||
            digest_size != key_.size()) {
            OPENSSL_cleanse(material.data(), material.size());
            throw std::runtime_error("could not derive node state key");
        }
        OPENSSL_cleanse(material.data(), material.size());
    }

    ~state_cipher() {
        OPENSSL_cleanse(key_.data(), key_.size());
    }
    state_cipher(const state_cipher&) = delete;
    state_cipher& operator=(const state_cipher&) = delete;

    [[nodiscard]] std::string seal(std::string_view plaintext) const {
        std::array<unsigned char, nonce_size> nonce{};
        std::array<unsigned char, tag_size> tag{};
        if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
            throw std::runtime_error("could not generate node state nonce");
        }
        context context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
        if (!context ||
            EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
            EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                static_cast<int>(nonce.size()), nullptr) != 1 ||
            EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key_.data(), nonce.data()) != 1) {
            throw std::runtime_error("could not initialize node state encryption");
        }
        std::vector<unsigned char> ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
        int written{};
        int final_size{};
        if (EVP_EncryptUpdate(context.get(), ciphertext.data(), &written,
                reinterpret_cast<const unsigned char*>(plaintext.data()),
                checked_size(plaintext.size())) != 1 ||
            EVP_EncryptFinal_ex(context.get(), ciphertext.data() + written, &final_size) != 1 ||
            EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()),
                tag.data()) != 1) {
            throw std::runtime_error("could not encrypt node state");
        }
        ciphertext.resize(static_cast<std::size_t>(written) + static_cast<std::size_t>(final_size));
        std::string result(magic);
        result.append(reinterpret_cast<const char*>(nonce.data()), nonce.size());
        result.append(reinterpret_cast<const char*>(tag.data()), tag.size());
        result.append(reinterpret_cast<const char*>(ciphertext.data()), ciphertext.size());
        OPENSSL_cleanse(ciphertext.data(), ciphertext.size());
        return result;
    }

    [[nodiscard]] std::string open(std::string_view envelope) const {
        if (!envelope.starts_with(magic) ||
            envelope.size() < magic.size() + nonce_size + tag_size) {
            throw std::runtime_error("stored node state is not an encrypted v1 envelope");
        }
        const auto nonce = envelope.substr(magic.size(), nonce_size);
        const auto tag = envelope.substr(magic.size() + nonce_size, tag_size);
        const auto ciphertext = envelope.substr(magic.size() + nonce_size + tag_size);
        std::array<unsigned char, tag_size> mutable_tag{};
        std::copy(tag.begin(), tag.end(), mutable_tag.begin());
        context context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
        if (!context ||
            EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
            EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                static_cast<int>(nonce.size()), nullptr) != 1 ||
            EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key_.data(),
                reinterpret_cast<const unsigned char*>(nonce.data())) != 1) {
            throw std::runtime_error("could not initialize node state decryption");
        }
        std::vector<unsigned char> plaintext(ciphertext.size() + EVP_MAX_BLOCK_LENGTH);
        int written{};
        int final_size{};
        if (EVP_DecryptUpdate(context.get(), plaintext.data(), &written,
                reinterpret_cast<const unsigned char*>(ciphertext.data()),
                checked_size(ciphertext.size())) != 1 ||
            EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()),
                mutable_tag.data()) != 1 ||
            EVP_DecryptFinal_ex(context.get(), plaintext.data() + written, &final_size) != 1) {
            OPENSSL_cleanse(plaintext.data(), plaintext.size());
            throw std::runtime_error("stored node state authentication failed");
        }
        plaintext.resize(static_cast<std::size_t>(written) + static_cast<std::size_t>(final_size));
        std::string result(reinterpret_cast<const char*>(plaintext.data()), plaintext.size());
        OPENSSL_cleanse(plaintext.data(), plaintext.size());
        return result;
    }

private:
    static constexpr std::string_view magic{"FES1"};
    static constexpr std::size_t nonce_size = 12;
    static constexpr std::size_t tag_size = 16;
    using context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

    static int checked_size(std::size_t size) {
        if (size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            throw std::runtime_error("node state is too large");
        }
        return static_cast<int>(size);
    }

    std::array<unsigned char, 32> key_{};
};

}  // namespace flexedge::node
