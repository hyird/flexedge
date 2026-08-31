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

class StateCipher final {
  public:
    explicit StateCipher(std::string_view deviceKeyMaterial) {
        if (deviceKeyMaterial.empty()) {
            throw std::runtime_error("node device key material is empty");
        }
        std::string material("flexedge-node-state-v1:");
        material.append(deviceKeyMaterial);
        unsigned int digestSize{};
        if (EVP_Digest(material.data(), material.size(), key_.data(), &digestSize, EVP_sha256(),
                       nullptr) != 1 ||
            digestSize != key_.size()) {
            OPENSSL_cleanse(material.data(), material.size());
            throw std::runtime_error("could not derive node state key");
        }
        OPENSSL_cleanse(material.data(), material.size());
    }

    ~StateCipher() { OPENSSL_cleanse(key_.data(), key_.size()); }
    StateCipher(const StateCipher&) = delete;
    StateCipher& operator=(const StateCipher&) = delete;

    [[nodiscard]] std::string seal(std::string_view plaintext) const {
        std::array<unsigned char, kNonceSize> nonce{};
        std::array<unsigned char, kTagSize> tag{};
        if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
            throw std::runtime_error("could not generate node state nonce");
        }
        Context context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
        if (!context ||
            EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
            EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                                static_cast<int>(nonce.size()), nullptr) != 1 ||
            EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key_.data(), nonce.data()) != 1) {
            throw std::runtime_error("could not initialize node state encryption");
        }
        std::vector<unsigned char> ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
        int written{};
        int finalSize{};
        if (EVP_EncryptUpdate(context.get(), ciphertext.data(), &written,
                              reinterpret_cast<const unsigned char*>(plaintext.data()),
                              checkedSize(plaintext.size())) != 1 ||
            EVP_EncryptFinal_ex(context.get(), ciphertext.data() + written, &finalSize) != 1 ||
            EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()),
                                tag.data()) != 1) {
            throw std::runtime_error("could not encrypt node state");
        }
        ciphertext.resize(static_cast<std::size_t>(written) + static_cast<std::size_t>(finalSize));
        std::string result(kMagic);
        result.append(reinterpret_cast<const char*>(nonce.data()), nonce.size());
        result.append(reinterpret_cast<const char*>(tag.data()), tag.size());
        result.append(reinterpret_cast<const char*>(ciphertext.data()), ciphertext.size());
        OPENSSL_cleanse(ciphertext.data(), ciphertext.size());
        return result;
    }

    [[nodiscard]] std::string open(std::string_view envelope) const {
        if (!envelope.starts_with(kMagic) ||
            envelope.size() < kMagic.size() + kNonceSize + kTagSize) {
            throw std::runtime_error("stored node state is not an encrypted v1 envelope");
        }
        const auto nonce = envelope.substr(kMagic.size(), kNonceSize);
        const auto tag = envelope.substr(kMagic.size() + kNonceSize, kTagSize);
        const auto ciphertext = envelope.substr(kMagic.size() + kNonceSize + kTagSize);
        std::array<unsigned char, kTagSize> mutableTag{};
        std::copy(tag.begin(), tag.end(), mutableTag.begin());
        Context context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
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
        int finalSize{};
        if (EVP_DecryptUpdate(context.get(), plaintext.data(), &written,
                              reinterpret_cast<const unsigned char*>(ciphertext.data()),
                              checkedSize(ciphertext.size())) != 1 ||
            EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()),
                                mutableTag.data()) != 1 ||
            EVP_DecryptFinal_ex(context.get(), plaintext.data() + written, &finalSize) != 1) {
            OPENSSL_cleanse(plaintext.data(), plaintext.size());
            throw std::runtime_error("stored node state authentication failed");
        }
        plaintext.resize(static_cast<std::size_t>(written) + static_cast<std::size_t>(finalSize));
        std::string result(reinterpret_cast<const char*>(plaintext.data()), plaintext.size());
        OPENSSL_cleanse(plaintext.data(), plaintext.size());
        return result;
    }

  private:
    static constexpr std::string_view kMagic{"FES1"};
    static constexpr std::size_t kNonceSize = 12;
    static constexpr std::size_t kTagSize = 16;
    using Context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

    static int checkedSize(std::size_t size) {
        if (size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            throw std::runtime_error("node state is too large");
        }
        return static_cast<int>(size);
    }

    std::array<unsigned char, 32> key_{};
};

} // namespace flexedge::node
