/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_LIBARTBASE_BASE_BIT_VECTOR_INL_H_
#define ART_LIBARTBASE_BASE_BIT_VECTOR_INL_H_

#include "bit_vector.h"

#include <android-base/logging.h>
#include <cstring>

#include "bit_utils.h"

namespace art {

template <typename StorageType>
inline void BitVectorView<StorageType>::ClearAllBits() {
  // Note: We do not `DCheckTrailingBitsClear()` here as this may be the initial call
  // to clear the storage and the trailing bits may not be clear after allocation.
  memset(storage_, 0, SizeInWords() * sizeof(WordType));
}

template <typename StorageType>
inline void BitVectorView<StorageType>::SetInitialBits(uint32_t num_bits) {
  // Note: We do not `DCheckTrailingBitsClear()` here as this may be the initial call
  // to clear the storage and the trailing bits may not be clear after allocation.
  DCHECK_LE(num_bits, SizeInBits());
  size_t words = WordIndex(num_bits);
  // Set initial full words.
  std::fill_n(storage_, words, std::numeric_limits<WordType>::max());
  if (num_bits % kWordBits != 0) {
    // Set all bits below the first clear bit in the boundary storage word.
    storage_[words] = BitMask(num_bits) - static_cast<StorageType>(1u);
    ++words;
  }
  // Set clear words if any.
  std::fill_n(storage_ + words, SizeInWords() - words, static_cast<StorageType>(0));
}

inline void BitVector::ClearAllBits() {
  AsView().ClearAllBits();
}

inline bool BitVector::Equal(const BitVector* src) const {
  return (storage_size_ == src->GetStorageSize()) &&
    (expandable_ == src->IsExpandable()) &&
    (memcmp(storage_, src->GetRawStorage(), storage_size_ * sizeof(uint32_t)) == 0);
}

}  // namespace art

#endif  // ART_LIBARTBASE_BASE_BIT_VECTOR_INL_H_
