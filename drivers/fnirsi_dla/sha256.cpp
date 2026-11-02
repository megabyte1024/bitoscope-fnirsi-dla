/*
 * SPDX-License-Identifier: MIT
 */

#include "sha256.h"

#include <algorithm>
#include <bit>
#include <format>

namespace fnirsi {

namespace {

constexpr size_t kBlockSize = 64;
constexpr size_t kLengthFieldSize = 8;

constexpr std::array<uint32_t, 64> kRoundConstants{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::array<uint32_t, 8> kInitialHash{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

void compress(std::array<uint32_t, 8>& hash, std::span<const uint8_t> block) {
  std::array<uint32_t, 64> w{};
  for (size_t t = 0; t < 16; ++t) {
    w[t] = (uint32_t{block[4 * t]} << 24) | (uint32_t{block[4 * t + 1]} << 16) | (uint32_t{block[4 * t + 2]} << 8) |
           uint32_t{block[4 * t + 3]};
  }
  for (size_t t = 16; t < 64; ++t) {
    const uint32_t s0 = std::rotr(w[t - 15], 7) ^ std::rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
    const uint32_t s1 = std::rotr(w[t - 2], 17) ^ std::rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
    w[t] = w[t - 16] + s0 + w[t - 7] + s1;
  }
  uint32_t a = hash[0];
  uint32_t b = hash[1];
  uint32_t c = hash[2];
  uint32_t d = hash[3];
  uint32_t e = hash[4];
  uint32_t f = hash[5];
  uint32_t g = hash[6];
  uint32_t h = hash[7];
  for (size_t t = 0; t < 64; ++t) {
    const uint32_t sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const uint32_t choice = (e & f) ^ (~e & g);
    const uint32_t t1 = h + sum1 + choice + kRoundConstants[t] + w[t];
    const uint32_t sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  hash[0] += a;
  hash[1] += b;
  hash[2] += c;
  hash[3] += d;
  hash[4] += e;
  hash[5] += f;
  hash[6] += g;
  hash[7] += h;
}

}

Sha256Digest sha256(std::span<const uint8_t> data) {
  std::array<uint32_t, 8> hash = kInitialHash;
  const size_t wholeBlocks = data.size() / kBlockSize;
  for (size_t i = 0; i < wholeBlocks; ++i) compress(hash, data.subspan(i * kBlockSize, kBlockSize));

  std::array<uint8_t, 2 * kBlockSize> tail{};
  const auto rest = data.subspan(wholeBlocks * kBlockSize);
  std::ranges::copy(rest, tail.begin());
  tail[rest.size()] = 0x80;
  const size_t tailSize = rest.size() < kBlockSize - kLengthFieldSize ? kBlockSize : 2 * kBlockSize;
  const uint64_t bitLength = static_cast<uint64_t>(data.size()) * 8;
  for (size_t i = 0; i < kLengthFieldSize; ++i) {
    tail[tailSize - 1 - i] = static_cast<uint8_t>(bitLength >> (8 * i));
  }
  for (size_t offset = 0; offset < tailSize; offset += kBlockSize) {
    compress(hash, std::span<const uint8_t>(tail).subspan(offset, kBlockSize));
  }

  Sha256Digest digest{};
  for (size_t i = 0; i < hash.size(); ++i) {
    for (size_t j = 0; j < 4; ++j) digest[4 * i + j] = static_cast<uint8_t>(hash[i] >> (24 - 8 * j));
  }
  return digest;
}

std::string sha256Hex(const Sha256Digest& digest) {
  std::string hex;
  hex.reserve(2 * digest.size());
  for (const uint8_t byte : digest) hex += std::format("{:02x}", byte);
  return hex;
}

}
