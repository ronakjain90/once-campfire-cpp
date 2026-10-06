// A port of rqrcode_core 2.1.0 and rqrcode 3.2.0 `as_svg` (Rect output). The output must be byte equal to the gem:
// this follows its algorithms, not the optimal choices of the QR standard. A single segment, error correction level
// H, the smallest version whose capacity is strictly greater than the bits, the mask with the fewest lost points as
// the gem scores them (with its floating point dark ratio term).
// Rust: crates/campfire/src/controllers/qr_code/rqrcode.rs.
#include "app/rqrcode.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace campfire::app::rqrcode {

namespace {

enum class Mode : std::uint32_t { Number = 1, AlphaNumeric = 2, Byte = 4 };

constexpr std::string_view kAlphanumeric = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";
constexpr std::uint32_t kLevelH = 2;

// `QRMAXBITS[:h]`
constexpr std::array<std::size_t, 40> kMaxBitsH = {
    72,   128,  208,  288,  368,  480,  528,  688,  800,  976,  1120, 1264, 1440, 1576, 1784, 2024, 2264, 2504, 2728, 3080,
    3248, 3536, 3712, 4112, 4304, 4768, 5024, 5288, 5608, 5960, 6344, 6760, 7208, 7688, 7888, 8432, 8768, 9136, 9776, 10208};

// The H rows of `QRRSBlock::RS_BLOCK_TABLE`: (count, total, data) groups, 0 ends a row of 1 or 2 groups.
struct Group {
  std::size_t count, total, data;
};
struct RsRow {
  Group first;
  Group second;  // count 0: none
};
constexpr std::array<RsRow, 40> kRsBlocksH = {{
    {{1, 26, 9}, {0, 0, 0}},     {{1, 44, 16}, {0, 0, 0}},    {{2, 35, 13}, {0, 0, 0}},    {{4, 25, 9}, {0, 0, 0}},
    {{2, 33, 11}, {2, 34, 12}},  {{4, 43, 15}, {0, 0, 0}},    {{4, 39, 13}, {1, 40, 14}},  {{4, 40, 14}, {2, 41, 15}},
    {{4, 36, 12}, {4, 37, 13}},  {{6, 43, 15}, {2, 44, 16}},  {{3, 36, 12}, {8, 37, 13}},  {{7, 42, 14}, {4, 43, 15}},
    {{12, 33, 11}, {4, 34, 12}}, {{11, 36, 12}, {5, 37, 13}}, {{11, 36, 12}, {7, 37, 13}}, {{3, 45, 15}, {13, 46, 16}},
    {{2, 42, 14}, {17, 43, 15}}, {{2, 42, 14}, {19, 43, 15}}, {{9, 39, 13}, {16, 40, 14}}, {{15, 43, 15}, {10, 44, 16}},
    {{19, 46, 16}, {6, 47, 17}}, {{34, 37, 13}, {0, 0, 0}},   {{16, 45, 15}, {14, 46, 16}}, {{30, 46, 16}, {2, 47, 17}},
    {{22, 45, 15}, {13, 46, 16}}, {{33, 46, 16}, {4, 47, 17}}, {{12, 45, 15}, {28, 46, 16}}, {{11, 45, 15}, {31, 46, 16}},
    {{19, 45, 15}, {26, 46, 16}}, {{23, 45, 15}, {25, 46, 16}}, {{23, 45, 15}, {28, 46, 16}}, {{19, 45, 15}, {35, 46, 16}},
    {{11, 45, 15}, {46, 46, 16}}, {{59, 46, 16}, {1, 47, 17}}, {{22, 45, 15}, {41, 46, 16}}, {{2, 45, 15}, {64, 46, 16}},
    {{24, 45, 15}, {46, 46, 16}}, {{42, 45, 15}, {32, 46, 16}}, {{10, 45, 15}, {67, 46, 16}}, {{20, 45, 15}, {61, 46, 16}},
}};

// `QRUtil::PATTERN_POSITION_TABLE`
constexpr std::array<std::array<std::uint8_t, 7>, 40> kPatternPositions = {{
    {}, {6, 18}, {6, 22}, {6, 26}, {6, 30}, {6, 34}, {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50},
    {6, 30, 54}, {6, 32, 58}, {6, 34, 62}, {6, 26, 46, 66}, {6, 26, 48, 70}, {6, 26, 50, 74}, {6, 30, 54, 78},
    {6, 30, 56, 82}, {6, 30, 58, 86}, {6, 34, 62, 90}, {6, 28, 50, 72, 94}, {6, 26, 50, 74, 98}, {6, 30, 54, 78, 102},
    {6, 28, 54, 80, 106}, {6, 32, 58, 84, 110}, {6, 30, 58, 86, 114}, {6, 34, 62, 90, 118}, {6, 26, 50, 74, 98, 122},
    {6, 30, 54, 78, 102, 126}, {6, 26, 52, 78, 104, 130}, {6, 30, 56, 82, 108, 134}, {6, 34, 60, 86, 112, 138},
    {6, 30, 58, 86, 114, 142}, {6, 34, 62, 90, 118, 146}, {6, 30, 54, 78, 102, 126, 150}, {6, 24, 50, 76, 102, 128, 154},
    {6, 28, 54, 80, 106, 132, 158}, {6, 32, 58, 84, 110, 136, 162}, {6, 26, 54, 82, 110, 138, 166},
    {6, 30, 58, 86, 114, 142, 170},
}};

constexpr std::uint32_t kG15 = (1U << 10) | (1U << 8) | (1U << 5) | (1U << 4) | (1U << 2) | (1U << 1) | 1U;
constexpr std::uint32_t kG18 = (1U << 12) | (1U << 11) | (1U << 10) | (1U << 9) | (1U << 8) | (1U << 5) | (1U << 2) | 1U;
constexpr std::uint32_t kG15Mask = (1U << 14) | (1U << 12) | (1U << 10) | (1U << 4) | (1U << 1);

// `QRUtil.get_length_in_bits`
std::size_t length_in_bits(Mode mode, std::size_t version) {
  const std::size_t macro = version <= 9 ? 0 : version <= 26 ? 1 : 2;
  switch (mode) {
    case Mode::Number: return std::array<std::size_t, 3>{10, 12, 14}[macro];
    case Mode::AlphaNumeric: return std::array<std::size_t, 3>{9, 11, 13}[macro];
    case Mode::Byte: return std::array<std::size_t, 3>{8, 16, 16}[macro];
  }
  return 8;
}

// `QRSegment`: the whole input in one mode.
struct Segment {
  std::string_view data;
  Mode mode = Mode::Byte;

  explicit Segment(std::string_view bytes) : data(bytes) {
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (std::all_of(data.begin(), data.end(), digit)) {
      mode = Mode::Number;
    } else if (std::all_of(data.begin(), data.end(), [](char c) { return kAlphanumeric.find(c) != std::string_view::npos; })) {
      mode = Mode::AlphaNumeric;
    } else {
      mode = Mode::Byte;
    }
  }

  [[nodiscard]] std::size_t content_size() const {
    const std::size_t length = data.size();
    std::size_t chunk = 1;
    std::size_t bits = 8;
    std::size_t extra = 0;
    switch (mode) {
      case Mode::Number:
        chunk = 3;
        bits = 10;
        extra = std::array<std::size_t, 3>{0, 4, 7}[length % 3];
        break;
      case Mode::AlphaNumeric:
        chunk = 2;
        bits = 11;
        extra = 6;
        break;
      case Mode::Byte: break;
    }
    return (length / chunk) * bits + (length % chunk == 0 ? 0 : extra);
  }
  // `QRSegment#size(version)`: the mode indicator, the length and the content.
  [[nodiscard]] std::size_t size(std::size_t version) const { return 4 + length_in_bits(mode, version) + content_size(); }
};

// `QRBitBuffer`
struct BitBuffer {
  std::size_t version;
  std::vector<std::uint8_t> buffer;
  std::size_t length = 0;

  explicit BitBuffer(std::size_t v) : version(v) {}

  void put_bit(bool bit) {
    const std::size_t index = length / 8;
    if (buffer.size() <= index) buffer.push_back(0);
    if (bit) buffer[index] |= static_cast<std::uint8_t>(0x80 >> (length % 8));
    ++length;
  }
  void put(std::uint32_t num, std::size_t bits) {
    for (std::size_t i = 0; i < bits; ++i) put_bit(((num >> (bits - i - 1)) & 1U) == 1U);
  }
  void end_of_message(std::size_t max_data_bits) {
    if (length + 4 <= max_data_bits) put(0, 4);
  }
  void pad_until(std::size_t preferred) {
    while (length % 8 != 0) put_bit(false);
    while (length < preferred) {
      put(0xEC, 8);
      if (length < preferred) put(0x11, 8);
    }
  }
};

void write_segment(const Segment& segment, BitBuffer& buffer) {
  buffer.put(static_cast<std::uint32_t>(segment.mode), 4);
  buffer.put(static_cast<std::uint32_t>(segment.data.size()), length_in_bits(segment.mode, buffer.version));
  switch (segment.mode) {
    case Mode::Number:
      for (std::size_t i = 0; i < segment.data.size(); i += 3) {
        const std::size_t n = std::min<std::size_t>(3, segment.data.size() - i);
        std::uint32_t code = 0;
        for (std::size_t k = 0; k < n; ++k) code = code * 10 + static_cast<std::uint32_t>(segment.data[i + k] - '0');
        buffer.put(code, std::array<std::size_t, 4>{0, 4, 7, 10}[n]);
      }
      break;
    case Mode::AlphaNumeric: {
      const auto index = [](char c) { return static_cast<std::uint32_t>(kAlphanumeric.find(c)); };
      for (std::size_t i = 0; i < segment.data.size(); i += 2) {
        if (i + 1 < segment.data.size()) {
          buffer.put(index(segment.data[i]) * 45 + index(segment.data[i + 1]), 11);
        } else {
          buffer.put(index(segment.data[i]), 6);
        }
      }
      break;
    }
    case Mode::Byte:
      for (const char c : segment.data) buffer.put(static_cast<std::uint8_t>(c), 8);
      break;
  }
}

// `QRMath`: GF(256) exp and log tables.
struct Galois {
  std::array<std::uint32_t, 256> exp{};
  std::array<std::uint32_t, 256> log{};

  Galois() {
    for (std::size_t i = 0; i < 8; ++i) exp[i] = 1U << i;
    for (std::size_t i = 8; i < 256; ++i) exp[i] = exp[i - 4] ^ exp[i - 5] ^ exp[i - 6] ^ exp[i - 8];
    for (std::size_t i = 0; i < 255; ++i) log[exp[i]] = static_cast<std::uint32_t>(i);
  }
  [[nodiscard]] std::int64_t glog(std::uint32_t n) const {
    if (n < 1) std::abort();
    return log[n];
  }
  [[nodiscard]] std::uint32_t gexp(std::int64_t n) const {
    while (n < 0) n += 255;
    while (n >= 256) n -= 255;
    return exp[static_cast<std::size_t>(n)];
  }
};

// `QRPolynomial`: leading zeros go, then `shift` zeros are added.
struct Polynomial {
  std::vector<std::uint32_t> values;

  Polynomial(const std::vector<std::uint32_t>& num, std::size_t shift) {
    std::size_t offset = 0;
    while (offset < num.size() && num[offset] == 0) ++offset;
    values.assign(num.begin() + static_cast<std::ptrdiff_t>(offset), num.end());
    values.resize(num.size() - offset + shift, 0);
  }

  [[nodiscard]] Polynomial multiply(const Polynomial& other, const Galois& gf) const {
    std::vector<std::uint32_t> num(values.size() + other.values.size() - 1, 0);
    for (std::size_t i = 0; i < values.size(); ++i) {
      for (std::size_t j = 0; j < other.values.size(); ++j) {
        num[i + j] ^= gf.gexp(gf.glog(values[i]) + gf.glog(other.values[j]));
      }
    }
    return Polynomial(num, 0);
  }

  [[nodiscard]] Polynomial modulo(const Polynomial& other, const Galois& gf) const {
    Polynomial current = *this;
    while (current.values.size() >= other.values.size()) {
      const std::int64_t ratio = gf.glog(current.values[0]) - gf.glog(other.values[0]);
      std::vector<std::uint32_t> num = current.values;
      for (std::size_t i = 0; i < other.values.size(); ++i) num[i] ^= gf.gexp(gf.glog(other.values[i]) + ratio);
      current = Polynomial(num, 0);
    }
    return current;
  }
};

// `QRUtil.get_error_correct_polynomial`
Polynomial error_correct_polynomial(std::size_t length, const Galois& gf) {
  Polynomial a({1}, 0);
  for (std::size_t i = 0; i < length; ++i) {
    a = a.multiply(Polynomial({1, gf.gexp(static_cast<std::int64_t>(i))}, 0), gf);
  }
  return a;
}

// `QRCode.create_data`: the data and error correction codewords, interleaved.
std::vector<std::uint8_t> create_data(std::size_t version, const Segment& segment) {
  std::vector<std::pair<std::size_t, std::size_t>> blocks;  // total, data
  const RsRow& row = kRsBlocksH[version - 1];
  for (const Group& g : {row.first, row.second}) {
    for (std::size_t i = 0; i < g.count; ++i) blocks.emplace_back(g.total, g.data);
  }
  std::size_t max_data_bits = 0;
  for (const auto& b : blocks) max_data_bits += b.second;
  max_data_bits *= 8;

  BitBuffer buffer(version);
  write_segment(segment, buffer);
  buffer.end_of_message(max_data_bits);
  if (buffer.length > max_data_bits) std::abort();  // "code length overflow": minimum_version rules it out
  buffer.pad_until(max_data_bits);

  const Galois gf;
  std::size_t offset = 0;
  std::vector<std::vector<std::uint32_t>> dc_data;
  std::vector<std::vector<std::uint32_t>> ec_data;
  for (const auto& [total, data_count] : blocks) {
    const std::size_t ec_count = total - data_count;
    std::vector<std::uint32_t> dc;
    for (std::size_t i = 0; i < data_count; ++i) dc.push_back(buffer.buffer[offset + i]);
    offset += data_count;
    const Polynomial rs_poly = error_correct_polynomial(ec_count, gf);
    const std::size_t ec_length = rs_poly.values.size() - 1;
    const Polynomial mod_poly = Polynomial(dc, ec_length).modulo(rs_poly, gf);
    std::vector<std::uint32_t> ec;
    for (std::size_t i = 0; i < ec_length; ++i) {
      const std::int64_t index = static_cast<std::int64_t>(i) + static_cast<std::int64_t>(mod_poly.values.size()) -
                                 static_cast<std::int64_t>(ec_length);
      ec.push_back(index >= 0 ? mod_poly.values[static_cast<std::size_t>(index)] : 0);
    }
    dc_data.push_back(std::move(dc));
    ec_data.push_back(std::move(ec));
  }

  std::vector<std::uint8_t> out;
  for (const auto* codewords : {&dc_data, &ec_data}) {
    std::size_t longest = 0;
    for (const auto& block : *codewords) longest = std::max(longest, block.size());
    for (std::size_t i = 0; i < longest; ++i) {
      for (const auto& block : *codewords) {
        if (i < block.size()) out.push_back(static_cast<std::uint8_t>(block[i]));
      }
    }
  }
  return out;
}

int bch_digit(std::uint32_t data) {
  int digit = 0;
  while (data != 0) {
    ++digit;
    data >>= 1;
  }
  return digit;
}

std::uint32_t bch_format_info(std::uint32_t data) {
  std::uint32_t d = data << 10;
  while (bch_digit(d) - bch_digit(kG15) >= 0) d ^= kG15 << (bch_digit(d) - bch_digit(kG15));
  return ((data << 10) | d) ^ kG15Mask;
}

std::uint32_t bch_version(std::uint32_t data) {
  std::uint32_t d = data << 12;
  while (bch_digit(d) - bch_digit(kG18) >= 0) d ^= kG18 << (bch_digit(d) - bch_digit(kG18));
  return (data << 12) | d;
}

// `QRMASKCOMPUTATIONS`
bool mask(std::uint32_t pattern, std::size_t i, std::size_t j) {
  switch (pattern) {
    case 0: return (i + j) % 2 == 0;
    case 1: return i % 2 == 0;
    case 2: return j % 3 == 0;
    case 3: return (i + j) % 3 == 0;
    case 4: return (i / 2 + j / 3) % 2 == 0;
    case 5: return ((i * j) % 2 + (i * j) % 3) == 0;
    case 6: return ((i * j) % 2 + (i * j) % 3) % 2 == 0;
    default: return ((i * j) % 3 + (i + j) % 2) % 2 == 0;
  }
}

// A cell that is not set yet is -1.
using Grid = std::vector<std::vector<std::int8_t>>;
using Modules = std::vector<std::vector<bool>>;

void place_position_probe_pattern(Grid& grid, std::int64_t row, std::int64_t col) {
  const auto count = static_cast<std::int64_t>(grid.size());
  for (std::int64_t r = -1; r <= 7; ++r) {
    const std::int64_t y = row + r;
    if (y < 0 || y >= count) continue;
    for (std::int64_t c = -1; c <= 7; ++c) {
      const std::int64_t x = col + c;
      if (x < 0 || x >= count) continue;
      const bool vertical = r >= 0 && r <= 6 && (c == 0 || c == 6);
      const bool horizontal = c >= 0 && c <= 6 && (r == 0 || r == 6);
      const bool square = r >= 2 && r <= 4 && c >= 2 && c <= 4;
      grid[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = (vertical || horizontal || square) ? 1 : 0;
    }
  }
}

void place_position_adjust_pattern(Grid& grid, std::size_t version) {
  const auto& positions = kPatternPositions[version - 1];
  for (const std::uint8_t row : positions) {
    if (row == 0) continue;
    for (const std::uint8_t col : positions) {
      if (col == 0) continue;
      if (grid[row][col] != -1) continue;
      for (int r = -2; r <= 2; ++r) {
        for (int c = -2; c <= 2; ++c) {
          const bool part = std::abs(r) == 2 || std::abs(c) == 2 || (r == 0 && c == 0);
          grid[static_cast<std::size_t>(row + r)][static_cast<std::size_t>(col + c)] = part ? 1 : 0;
        }
      }
    }
  }
}

void place_timing_pattern(Grid& grid) {
  const std::size_t count = grid.size();
  for (std::size_t i = 8; i < count - 8; ++i) {
    grid[i][6] = i % 2 == 0 ? 1 : 0;
    grid[6][i] = i % 2 == 0 ? 1 : 0;
  }
}

void place_version_info(Grid& grid, std::size_t version, bool test) {
  const std::size_t count = grid.size();
  const std::uint32_t bits = bch_version(static_cast<std::uint32_t>(version));
  for (std::size_t i = 0; i < 18; ++i) {
    const std::int8_t dark = (!test && ((bits >> i) & 1U) == 1U) ? 1 : 0;
    grid[i / 3][i % 3 + count - 8 - 3] = dark;
    grid[i % 3 + count - 8 - 3][i / 3] = dark;
  }
}

void place_format_info(Grid& grid, bool test, std::uint32_t pattern) {
  const std::size_t count = grid.size();
  const std::uint32_t bits = bch_format_info((kLevelH << 3) | pattern);
  for (std::size_t i = 0; i < 15; ++i) {
    const std::int8_t dark = (!test && ((bits >> i) & 1U) == 1U) ? 1 : 0;
    const std::size_t row = i < 6 ? i : i < 8 ? i + 1 : count - 15 + i;
    grid[row][8] = dark;
    const std::size_t col = i < 8 ? count - i - 1 : i < 9 ? 15 - i : 15 - i - 1;
    grid[8][col] = dark;
  }
  grid[count - 8][8] = test ? 0 : 1;
}

void map_data(Grid& grid, const std::vector<std::uint8_t>& data, std::uint32_t pattern) {
  const auto count = static_cast<std::int64_t>(grid.size());
  std::int64_t inc = -1;
  std::int64_t row = count - 1;
  int bit_index = 7;
  std::size_t byte_index = 0;
  for (std::int64_t col = count - 1; col >= 1; col -= 2) {
    const std::int64_t c0 = col <= 6 ? col - 1 : col;
    while (true) {
      for (std::int64_t c = 0; c < 2; ++c) {
        const auto x = static_cast<std::size_t>(c0 - c);
        const auto y = static_cast<std::size_t>(row);
        if (grid[y][x] == -1) {
          bool dark = byte_index < data.size() && ((data[byte_index] >> bit_index) & 1) == 1;
          if (mask(pattern, y, x)) dark = !dark;
          grid[y][x] = dark ? 1 : 0;
          --bit_index;
          if (bit_index == -1) {
            ++byte_index;
            bit_index = 7;
          }
        }
      }
      row += inc;
      if (row < 0 || count <= row) {
        row -= inc;
        inc = -inc;
        break;
      }
    }
  }
}

// `QRUtil.get_lost_points`. The dark ratio term is a Float in Ruby, so the total is too.
double lost_points(const Modules& modules) {
  const std::size_t count = modules.size();
  const std::size_t max = count - 1;
  std::int64_t points = 0;
  for (std::size_t row = 0; row < count; ++row) {
    for (std::size_t col = 0; col < count; ++col) {
      const bool dark = modules[row][col];
      std::int64_t same = 0;
      if (row > 0) {
        const auto& above = modules[row - 1];
        same += (col > 0 && dark == above[col - 1]) ? 1 : 0;
        same += (dark == above[col]) ? 1 : 0;
        same += (col < max && dark == above[col + 1]) ? 1 : 0;
      }
      same += (col > 0 && dark == modules[row][col - 1]) ? 1 : 0;
      same += (col < max && dark == modules[row][col + 1]) ? 1 : 0;
      if (row < max) {
        const auto& below = modules[row + 1];
        same += (col > 0 && dark == below[col - 1]) ? 1 : 0;
        same += (dark == below[col]) ? 1 : 0;
        same += (col < max && dark == below[col + 1]) ? 1 : 0;
      }
      if (same > 5) points += 3 + same - 5;
    }
  }
  for (std::size_t row = 0; row < max; ++row) {
    for (std::size_t col = 0; col < max; ++col) {
      const bool value = modules[row][col];
      if (value == modules[row + 1][col] && value == modules[row][col + 1] && value == modules[row + 1][col + 1]) points += 3;
    }
  }
  const auto finder = [](const auto& cell) {
    return cell(0) && !cell(1) && cell(2) && cell(3) && cell(4) && !cell(5) && cell(6);
  };
  if (count > 6) {
    for (std::size_t start = 0; start < count - 6; ++start) {
      for (std::size_t line = 0; line < count; ++line) {
        if (finder([&](std::size_t k) { return static_cast<bool>(modules[line][start + k]); })) points += 40;
        if (finder([&](std::size_t k) { return static_cast<bool>(modules[start + k][line]); })) points += 40;
      }
    }
  }
  std::size_t dark_count = 0;
  for (const auto& row : modules) dark_count += static_cast<std::size_t>(std::count(row.begin(), row.end(), true));
  const double ratio = static_cast<double>(dark_count) / static_cast<double>(count * count);
  const double delta = std::fabs(100.0 * ratio - 50.0) / 5.0;
  return static_cast<double>(points) + delta * 10.0;
}

// `minimum_version`: the first version whose capacity is strictly greater than the bits needed.
std::optional<std::size_t> minimum_version(const Segment& segment) {
  for (std::size_t version = 1; version <= 40; ++version) {
    if (segment.size(version) < kMaxBitsH[version - 1]) return version;
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::size_t> version_for(std::string_view data) {
  return minimum_version(Segment(data));
}

std::optional<std::vector<std::vector<bool>>> modules(std::string_view data) {
  const Segment segment(data);
  const auto version = minimum_version(segment);
  if (!version) return std::nullopt;
  const std::size_t count = *version * 4 + 17;
  Grid common(count, std::vector<std::int8_t>(count, -1));
  place_position_probe_pattern(common, 0, 0);
  place_position_probe_pattern(common, static_cast<std::int64_t>(count) - 7, 0);
  place_position_probe_pattern(common, 0, static_cast<std::int64_t>(count) - 7);
  place_position_adjust_pattern(common, *version);
  place_timing_pattern(common);
  const std::vector<std::uint8_t> bytes = create_data(*version, segment);
  const auto make = [&](bool test, std::uint32_t pattern) {
    Grid grid = common;
    place_format_info(grid, test, pattern);
    if (*version >= 7) place_version_info(grid, *version, test);
    map_data(grid, bytes, pattern);
    Modules out(count, std::vector<bool>(count, false));
    for (std::size_t r = 0; r < count; ++r) {
      for (std::size_t c = 0; c < count; ++c) out[r][c] = grid[r][c] == 1;
    }
    return out;
  };
  // `get_best_mask_pattern`: the first pattern with the fewest lost points.
  std::uint32_t best = 0;
  double best_points = std::numeric_limits<double>::max();
  for (std::uint32_t pattern = 0; pattern < 8; ++pattern) {
    const double points = lost_points(make(true, pattern));
    if (pattern == 0 || best_points > points) {
      best = pattern;
      best_points = points;
    }
  }
  return make(false, best);
}

std::optional<std::string> svg(std::string_view data) {
  const auto grid = modules(data);
  if (!grid) return std::nullopt;
  constexpr std::size_t kModuleSize = 11;
  const std::string dimension = std::to_string(grid->size() * kModuleSize);
  std::string out;
  out.reserve(256 + grid->size() * grid->size() * 30);
  out += "<?xml version=\"1.0\" standalone=\"yes\"?>";
  out += "<svg version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
         "xmlns:ev=\"http://www.w3.org/2001/xml-events\" viewBox=\"0 0 " + dimension + " " + dimension +
         "\" shape-rendering=\"crispEdges\">";
  out += "<rect width=\"" + dimension + "\" height=\"" + dimension + "\" x=\"0\" y=\"0\" fill=\"white\"/>";
  for (std::size_t row = 0; row < grid->size(); ++row) {
    for (std::size_t col = 0; col < grid->size(); ++col) {
      if (!(*grid)[row][col]) continue;
      out += "<rect width=\"11\" height=\"11\" x=\"" + std::to_string(col * kModuleSize) + "\" y=\"" +
             std::to_string(row * kModuleSize) + "\" fill=\"black\"/>";
    }
  }
  out += "</svg>";
  return out;
}

}  // namespace campfire::app::rqrcode
