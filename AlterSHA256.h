/*
    AlterSHA256.h
    ------------------------------------------------------------------
    Samostatny SHA-256 + HMAC-SHA256.

    PRECO VLASTNA IMPLEMENTACIA A NIE juce_cryptography:
    ziadny z troch .jucer projektov nema juce_cryptography v MODULES.
    Pridavat modul do troch projektov (a do AlterCreator, ktory ma v
    exportere VS2026 nekonzistentne MODULEPATHs "../../juce") je vacsi
    zasah nez 150 riadkov header-only kodu bez zavislosti.

    Ciste C++17, header-only, ziadne zavislosti. Da sa unit-testovat samo.
*/

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <array>
#include <vector>

namespace alter::crypto
{

//==============================================================================
// SHA-256 (FIPS 180-4)
//==============================================================================

class SHA256
{
public:
    SHA256() { reset(); }

    void reset() noexcept
    {
        state = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                  0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
        bitLen = 0;
        bufLen = 0;
    }

    void update (const void* data, std::size_t len) noexcept
    {
        const auto* p = static_cast<const std::uint8_t*> (data);

        for (std::size_t i = 0; i < len; ++i)
        {
            buffer[bufLen++] = p[i];

            if (bufLen == 64)
            {
                transform (buffer.data());
                bitLen += 512;
                bufLen = 0;
            }
        }
    }

    void update (const std::string& s) noexcept { update (s.data(), s.size()); }

    std::array<std::uint8_t, 32> digest() noexcept
    {
        std::uint64_t totalBits = bitLen + (std::uint64_t) bufLen * 8;

        // padding: 0x80, potom nuly, poslednych 8 bajtov = dlzka v bitoch (BE)
        buffer[bufLen++] = 0x80;

        if (bufLen > 56)
        {
            while (bufLen < 64) buffer[bufLen++] = 0;
            transform (buffer.data());
            bufLen = 0;
        }

        while (bufLen < 56) buffer[bufLen++] = 0;

        for (int i = 7; i >= 0; --i)
            buffer[bufLen++] = (std::uint8_t) ((totalBits >> (i * 8)) & 0xff);

        transform (buffer.data());

        std::array<std::uint8_t, 32> out {};
        for (int i = 0; i < 8; ++i)
        {
            out[(std::size_t) i * 4 + 0] = (std::uint8_t) ((state[(std::size_t) i] >> 24) & 0xff);
            out[(std::size_t) i * 4 + 1] = (std::uint8_t) ((state[(std::size_t) i] >> 16) & 0xff);
            out[(std::size_t) i * 4 + 2] = (std::uint8_t) ((state[(std::size_t) i] >>  8) & 0xff);
            out[(std::size_t) i * 4 + 3] = (std::uint8_t) ( state[(std::size_t) i]        & 0xff);
        }
        return out;
    }

    static std::array<std::uint8_t, 32> hash (const void* data, std::size_t len) noexcept
    {
        SHA256 h; h.update (data, len); return h.digest();
    }

    static std::array<std::uint8_t, 32> hash (const std::string& s) noexcept
    {
        return hash (s.data(), s.size());
    }

private:
    static constexpr std::uint32_t rotr (std::uint32_t x, int n) noexcept
    {
        return (x >> n) | (x << (32 - n));
    }

    void transform (const std::uint8_t* block) noexcept
    {
        static constexpr std::uint32_t K[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u };

        std::uint32_t w[64];

        for (int i = 0; i < 16; ++i)
            w[i] = ((std::uint32_t) block[i * 4 + 0] << 24)
                 | ((std::uint32_t) block[i * 4 + 1] << 16)
                 | ((std::uint32_t) block[i * 4 + 2] <<  8)
                 | ((std::uint32_t) block[i * 4 + 3]);

        for (int i = 16; i < 64; ++i)
        {
            const auto s0 = rotr (w[i - 15], 7) ^ rotr (w[i - 15], 18) ^ (w[i - 15] >> 3);
            const auto s1 = rotr (w[i -  2], 17) ^ rotr (w[i - 2], 19) ^ (w[i -  2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        auto a = state[0], b = state[1], c = state[2], d = state[3];
        auto e = state[4], f = state[5], g = state[6], h = state[7];

        for (int i = 0; i < 64; ++i)
        {
            const auto S1    = rotr (e, 6) ^ rotr (e, 11) ^ rotr (e, 25);
            const auto ch    = (e & f) ^ (~e & g);
            const auto temp1 = h + S1 + ch + K[i] + w[i];
            const auto S0    = rotr (a, 2) ^ rotr (a, 13) ^ rotr (a, 22);
            const auto maj   = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = S0 + maj;

            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    std::array<std::uint32_t, 8> state {};
    std::array<std::uint8_t, 64> buffer {};
    std::uint64_t bitLen = 0;
    std::size_t   bufLen = 0;
};

//==============================================================================
// Helpers
//==============================================================================

inline std::string toHex (const std::array<std::uint8_t, 32>& d)
{
    static constexpr char hexDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve (64);

    for (auto b : d)
    {
        s += hexDigits[b >> 4];
        s += hexDigits[b & 0x0f];
    }
    return s;
}

inline std::string sha256Hex (const std::string& s)
{
    return toHex (SHA256::hash (s));
}

//==============================================================================
// HMAC-SHA256 (RFC 2104)
//==============================================================================

inline std::string hmacSha256Hex (const std::string& key, const std::string& message)
{
    constexpr std::size_t blockSize = 64;

    std::vector<std::uint8_t> k (key.begin(), key.end());

    if (k.size() > blockSize)
    {
        const auto h = SHA256::hash (k.data(), k.size());
        k.assign (h.begin(), h.end());
    }

    k.resize (blockSize, 0);

    std::vector<std::uint8_t> inner (blockSize), outer (blockSize);
    for (std::size_t i = 0; i < blockSize; ++i)
    {
        inner[i] = (std::uint8_t) (k[i] ^ 0x36);
        outer[i] = (std::uint8_t) (k[i] ^ 0x5c);
    }

    inner.insert (inner.end(), message.begin(), message.end());
    const auto innerHash = SHA256::hash (inner.data(), inner.size());

    outer.insert (outer.end(), innerHash.begin(), innerHash.end());
    return toHex (SHA256::hash (outer.data(), outer.size()));
}

/** Porovnanie odolne voci timing attackom. */
inline bool constantTimeEquals (const std::string& a, const std::string& b) noexcept
{
    if (a.size() != b.size())
        return false;

    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= (unsigned char) (a[i] ^ b[i]);

    return diff == 0;
}

} // namespace alter::crypto
