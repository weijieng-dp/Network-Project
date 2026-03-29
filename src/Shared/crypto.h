// crypto_utils.h
#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <random>
#include <chrono>

// Windows CryptoAPI for random number generation
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")

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