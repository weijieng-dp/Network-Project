// crypto_utils.h
#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <random>
#include <chrono>

// OpenSSL lib
#include <openssl/evp.h>
#include <openssl/rand.h>

// Windows CryptoAPI for random number generation
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "libcrypto.lib")
#pragma comment(lib, "libssl.lib")

/**
 * Simple Diffie-Hellman key exchange implementation
 * Uses a safe prime for secure key exchange
 */
class DiffieHellman {
private:
    // RFC 3526 2048-bit MODP Group (safe prime)
    static const uint64_t P = 0xFFFFFFFFFFFFFBFFull;  // Large prime (2^64 - 2^10 - 1)
    static const uint64_t G = 5;                      // Generator

    uint64_t privateKey;
    uint64_t publicKey;
    uint64_t sharedSecret;
    bool hasSharedSecret;

    // Generate cryptographically secure random number
    static uint64_t generateRandomKey() {
        uint64_t key = 0;
        HCRYPTPROV prov;
        if (CryptAcquireContext(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
            CryptGenRandom(prov, sizeof(key), (BYTE*)&key);
            CryptReleaseContext(prov, 0);
        }
        else {
            // Fallback to mt19937 if CryptoAPI fails
            std::random_device rd;
            std::mt19937_64 gen(rd());
            std::uniform_int_distribution<uint64_t> dis;
            key = dis(gen);
        }
        return key;
    }

    // Modular exponentiation: base^exp mod mod
    static uint64_t modPow(uint64_t base, uint64_t exp, uint64_t mod) {
        uint64_t result = 1;
        base %= mod;

        while (exp > 0) {
            if (exp & 1) {
                result = (result * base) % mod;
            }
            base = (base * base) % mod;
            exp >>= 1;
        }
        return result;
    }

public:
    DiffieHellman() : privateKey(0), publicKey(0), sharedSecret(0), hasSharedSecret(false) {
        // Generate private key (2 to P-2)
        privateKey = generateRandomKey() % (P - 3) + 2;

        // Compute public key: g^private mod p
        publicKey = modPow(G, privateKey, P);
    }

    uint64_t getPublicKey() const {
        return publicKey;
    }

    void computeSharedSecret(uint64_t otherPublicKey) {
        // Compute shared secret: otherPublicKey^private mod p
        sharedSecret = modPow(otherPublicKey, privateKey, P);
        hasSharedSecret = true;
    }

    bool hasSecret() const {
        return hasSharedSecret;
    }

    uint64_t getSharedSecret() const {
        return sharedSecret;
    }

    // Derive XOR encryption key from shared secret
    std::vector<uint8_t> getEncryptionKey(size_t keySize = 32) const {
        std::vector<uint8_t> key(keySize);
        uint64_t secret = sharedSecret;

        // Expand the 64-bit secret to desired key size using simple expansion
        for (size_t i = 0; i < keySize; ++i) {
            key[i] = (secret >> ((i % 8) * 8)) & 0xFF;
            if (i > 0 && i % 8 == 0) {
                // Mix in additional entropy
                secret = (secret * 0x9e3779b97f4a7c15ull) ^ (secret >> 31);
            }
        }
        return key;
    }

    // XOR encrypt/decrypt (symmetric)
    static std::vector<uint8_t> xorEncryptDecrypt(const std::vector<uint8_t>& data,
        const std::vector<uint8_t>& key) {
        std::vector<uint8_t> result = data;
        for (size_t i = 0; i < result.size(); ++i) {
            result[i] ^= key[i % key.size()];
        }
        return result;
    }

    static std::string xorEncryptDecrypt(const std::string& data,
        const std::vector<uint8_t>& key) {
        std::string result = data;
        for (size_t i = 0; i < result.size(); ++i) {
            result[i] ^= key[i % key.size()];
        }
        return result;
    }
};

/**
 * Simple XOR encryption for session keys (alternative for testing)
 */
class XORCipher {
private:
    std::vector<uint8_t> key;

public:
    XORCipher() {}

    XORCipher(const std::vector<uint8_t>& k) : key(k) {}

    void setKey(const std::vector<uint8_t>& k) {
        key = k;
    }

    std::string encrypt(const std::string& plaintext) {
        std::string ciphertext = plaintext;
        for (size_t i = 0; i < ciphertext.size(); ++i) {
            if (!key.empty()) {
                ciphertext[i] ^= key[i % key.size()];
            }
        }
        return ciphertext;
    }

    std::string decrypt(const std::string& ciphertext) {
        return encrypt(ciphertext);  // XOR is symmetric
    }

    std::vector<uint8_t> encrypt(const std::vector<uint8_t>& plaintext) {
        std::vector<uint8_t> ciphertext = plaintext;
        for (size_t i = 0; i < ciphertext.size(); ++i) {
            if (!key.empty()) {
                ciphertext[i] ^= key[i % key.size()];
            }
        }
        return ciphertext;
    }
};


class AESGCMCipher {
    public:
        AESGCMCipher() { }
        AESGCMCipher(const std::vector<uint8_t>& k) : key(k) { 
            if(key.size() != KEY_SIZE) { key.resize(KEY_SIZE, 0); } // validate size
        }

        void setKey(const std::vector<uint8_t>& k) {
            key = k;
            if(key.size() != KEY_SIZE) { key.resize(KEY_SIZE, 0); } // validate size
        }

        void setKey(const std::string& password, const std::vector<uint8_t>& salt) {
            key.resize(KEY_SIZE);
            if(PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()), salt.data(), static_cast<int>(salt.size()), 100000, EVP_sha256(), KEY_SIZE, key.data()) != 1)
                throw std::runtime_error("Failed to derive AES key from password");
        }

        std::vector<uint8_t> encrypt(const std::vector<uint8_t>& plainTxt, const std::vector<uint8_t>& aad = {}) {
            if(key.empty() || plainTxt.empty()) return plainTxt;

            std::vector<uint8_t> iv{ generateIV() };
            std::vector<uint8_t> cipherTxt(plainTxt.size());
            std::vector<uint8_t> tag(TAG_SIZE);

            EVP_CIPHER_CTX *ctx { EVP_CIPHER_CTX_new() };
            if(!ctx) return plainTxt;

            // Initialize AES 256 GCM Encryption
            if(EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return plainTxt;
            }

            // Set IV length
            if(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), NULL) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return plainTxt;
            }

            // Initialize Key and IV
            if(EVP_EncryptInit_ex(ctx, NULL, NULL, key.data(), iv.data()) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return plainTxt;
            }

            // Additional Authentication Data (AAD)
            int len{}, finalLen{};
            const uint8_t* aadPtr{ aad.empty() ? nullptr : aad.data() };
            if(EVP_EncryptUpdate(ctx, NULL, &len, aadPtr, static_cast<int>(aad.size())) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return plainTxt;
            }

            // Encrypt the plain text
            if(EVP_EncryptUpdate(ctx, cipherTxt.data(), &len, plainTxt.data(), static_cast<int>(plainTxt.size())) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return plainTxt;
            }

            // Finalize
            if(EVP_EncryptFinal_ex(ctx, cipherTxt.data() + len, &finalLen) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return plainTxt;
            }

            // Get authentication tag
            if(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_SIZE, tag.data()) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return plainTxt;
            }

            EVP_CIPHER_CTX_free(ctx);

            cipherTxt.resize(len + finalLen);

            // Combine IV + Cipher Text + tag
            std::vector<uint8_t> result;
            result.reserve(iv.size() + cipherTxt.size() + tag.size());
            result.insert(result.end(), iv.begin(), iv.end());
            result.insert(result.end(), cipherTxt.begin(), cipherTxt.end());
            result.insert(result.end(), tag.begin(), tag.end());

            return result;
        }

        // Decrypt with authentication verifcation
        // cipher text includes IV and Tag
        std::vector<uint8_t> decrypt(const std::vector<uint8_t>& cipherTxtWithIvAndTag, const std::vector<uint8_t>& aad = {}) {
            if(key.empty() || cipherTxtWithIvAndTag.size() < IV_SIZE + TAG_SIZE) return cipherTxtWithIvAndTag;

            // Extract IV, Cipher Text, and Tag
            std::vector<uint8_t> iv(cipherTxtWithIvAndTag.begin(), cipherTxtWithIvAndTag.begin() + IV_SIZE);

            size_t cipherTxtLen { cipherTxtWithIvAndTag.size() - IV_SIZE - TAG_SIZE };
            
            std::vector<uint8_t> cipherTxt( cipherTxtWithIvAndTag.begin() + IV_SIZE,  cipherTxtWithIvAndTag.begin() + IV_SIZE + cipherTxtLen);
            std::vector<uint8_t> tag( cipherTxtWithIvAndTag.begin() + IV_SIZE + cipherTxtLen, cipherTxtWithIvAndTag.end());

            std::vector<uint8_t> plainTxt(cipherTxt.size());

            EVP_CIPHER_CTX *ctx { EVP_CIPHER_CTX_new() };
            if(!ctx) return cipherTxtWithIvAndTag;

            // Initialize AES 256 GCM Decryption
            if(EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return cipherTxtWithIvAndTag;
            }

            // Set IV length
            if(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), NULL) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return cipherTxtWithIvAndTag;
            }

            // Initialize Key and IV
            if(EVP_DecryptInit_ex(ctx, NULL, NULL, key.data(), iv.data()) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return cipherTxtWithIvAndTag;
            }

            // Provide AAD
            int len{}, finalLen{};
            const uint8_t* aadPtr{ aad.empty() ? nullptr : aad.data() };
            if(EVP_DecryptUpdate(ctx, NULL, &len, aadPtr, static_cast<int>(aad.size())) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return cipherTxtWithIvAndTag;
            }

            // Decrypt cipher text
            if(EVP_DecryptUpdate(ctx, plainTxt.data(), &len, cipherTxt.data(), static_cast<int>(cipherTxt.size())) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return cipherTxtWithIvAndTag;
            }

            // Set expected Tag for verification
            if(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_SIZE, tag.data()) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return cipherTxtWithIvAndTag;
            }
            
            // Finalize and Verify
            int ret{ EVP_DecryptFinal_ex(ctx, plainTxt.data() + len, &finalLen) };
            EVP_CIPHER_CTX_free(ctx);
            if(ret <= 0) return {}; // Authentication  failed (tampered data) - Return Empty
            
            plainTxt.resize(len + finalLen);
            return plainTxt;
        }

        bool hasKey() const { return !key.empty(); }

    private:
        // Used for generating a random IV for each encryption operation
        static std::vector<uint8_t> generateIV() {
            std::vector<uint8_t> iv(IV_SIZE);
            HCRYPTPROV prov;
            if(CryptAcquireContext(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
                CryptGenRandom(prov, static_cast<DWORD>(iv.size()), iv.data());
                CryptReleaseContext(prov, 0);
            } else RAND_bytes(iv.data(), static_cast<int>(iv.size()));   // fallback to using OpenSSL RAND
            return iv;
        }
    private:
        std::vector<uint8_t> key;

        static const int KEY_SIZE{ 32 };    // 256-bits Key
        static const int IV_SIZE { 12 };    // 96-bits Initialization Vector (IV, a sort of nounce). Recommended size for GCM
        static const int TAG_SIZE{ 16 };    // 128-bits authentication tag

};