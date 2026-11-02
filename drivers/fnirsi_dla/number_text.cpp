/*
 * SPDX-License-Identifier: MIT
 */

#include "number_text.h"

namespace fnirsi {

std::string withThousands(uint64_t value) {
  const std::string digits = std::to_string(value);
  std::string grouped;
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i != 0 && (digits.size() - i) % 3 == 0) grouped.push_back('\'');
    grouped.push_back(digits[i]);
  }
  return grouped;
}

}
