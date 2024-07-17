/**
 * @file   buffer.cc
 *
 * @section LICENSE
 *
 * The MIT License
 *
 * @copyright Copyright (c) 2017-2021 TileDB, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * @section DESCRIPTION
 *
 * This file implements classes BufferBase, Buffer, ConstBuffer, and
 * PreallocatedBuffer.
 */

#include "tiledb/sm/buffer/buffer.h"
#include "tiledb/common/heap_memory.h"
#include "tiledb/common/logger.h"
#include "tiledb/common/pmr.h"

#include <algorithm>
#include <iostream>

using namespace tiledb::common;

namespace tiledb {
namespace sm {
class BufferStatusException : public StatusException {
 public:
  explicit BufferStatusException(const std::string& msg)
      : StatusException("Buffer", msg) {
  }
};

/* ****************************** */
/*          BufferBase            */
/* ****************************** */

BufferBase::BufferBase()
    : data_(nullptr)
    , size_(0)
    , offset_(0){};

BufferBase::BufferBase(void* data, const uint64_t size)
    : data_(data)
    , size_(size)
    , offset_(0){};

BufferBase::BufferBase(const void* data, const uint64_t size)
    // const_cast is safe here because BufferBase methods do not modify storage
    : data_(const_cast<void*>(data))
    , size_(size)
    , offset_(0){};

uint64_t BufferBase::size() const {
  return size_;
}

void* BufferBase::nonconst_data() const {
  return data_;
}

const void* BufferBase::data() const {
  // const_cast is safe here because BufferBase methods do not modify storage
  return const_cast<const void*>(data_);
}

void* BufferBase::nonconst_unread_data() const {
  if (data_ == nullptr) {
    return nullptr;
  }
  // Cast to byte type because offset is measured in bytes
  return static_cast<int8_t*>(data_) + offset_;
}

const void* BufferBase::cur_data() const {
  // const_cast is safe here because BufferBase methods do not modify storage
  return const_cast<const void*>(nonconst_unread_data());
}

uint64_t BufferBase::offset() const {
  return offset_;
}

void BufferBase::reset_offset() {
  offset_ = 0;
}

void BufferBase::set_offset(const uint64_t offset) {
  assert_offset_is_valid(offset);
  offset_ = offset;
}

void BufferBase::advance_offset(const uint64_t nbytes) {
  if (nbytes >= size_ - offset_) {
    // The argument puts us at the end or past it, which is still at the end.
    offset_ = size_;
  } else {
    offset_ += nbytes;
  }
}

bool BufferBase::end() const {
  return offset_ == size_;
}

Status BufferBase::read(void* destination, const uint64_t nbytes) {
  if (nbytes > size_ - offset_) {
    return LOG_STATUS(Status_BufferError(
        "Read buffer overflow; may not read beyond buffer size"));
  }
  std::memcpy(destination, static_cast<char*>(data_) + offset_, nbytes);
  offset_ += nbytes;
  return Status::Ok();
}

Status BufferBase::read(
    void* destination, const uint64_t offset, const uint64_t nbytes) {
  if (nbytes > size_ - offset) {
    return LOG_STATUS(Status_BufferError(
        "Read buffer overflow; may not read beyond buffer size"));
  }
  std::memcpy(destination, static_cast<char*>(data_) + offset, nbytes);
  return Status::Ok();
}

void BufferBase::assert_offset_is_valid(uint64_t offset) const {
  if (offset > size_) {
    throw std::out_of_range("BufferBase::set_offset");
  }
}

/* ****************************** */
/*            Buffer              */
/* ****************************** */

template <class Alloc>
OwningMemoryBuffer<Alloc>::OwningMemoryBuffer(const allocator_type& alloc)
    : BufferBase()
    , vec_(alloc)
    , owns_data_(true) {
}

template <class Alloc>
OwningMemoryBuffer<Alloc>::OwningMemoryBuffer(
    uint64_t size, const allocator_type& alloc)
    : BufferBase((void*)nullptr, size)
    , vec_(alloc)
    , owns_data_(true)
    , preallocated_(true) {
  throw_if_not_ok(ensure_alloced_size(size_));
  size_ = 0;
}

template <class Alloc>
OwningMemoryBuffer<Alloc>::OwningMemoryBuffer(
    void* data, const uint64_t size, const allocator_type&)
    : BufferBase(data, size)
    , owns_data_(false) {
}

template <class Alloc>
OwningMemoryBuffer<Alloc>::OwningMemoryBuffer(
    const OwningMemoryBuffer<Alloc>& buff)
    : vec_(buff.vec_)
    , owns_data_(buff.owns_data_)
    , preallocated_(buff.preallocated_) {
  offset_ = buff.offset_;
  if (buff.owns_data_ && buff.data_ != nullptr) {
    data_ = vec_.data();
    size_ = buff.size_;
  }
}

template <class Alloc>
OwningMemoryBuffer<Alloc>::OwningMemoryBuffer(
    const OwningMemoryBuffer<Alloc>& buff, const allocator_type& alloc)
    : vec_(buff.vec_, alloc)
    , owns_data_(buff.owns_data_)
    , preallocated_(buff.preallocated_) {
  offset_ = buff.offset_;
  if (buff.owns_data_ && buff.data_ != nullptr) {
    data_ = vec_.data();
    size_ = buff.size_;
  }
}

template <class Alloc>
OwningMemoryBuffer<Alloc>::OwningMemoryBuffer(
    OwningMemoryBuffer<Alloc>&& buff) noexcept
    : BufferBase(
          std::exchange(buff.data_, nullptr), std::exchange(buff.size_, 0))
    , vec_(std::move(buff.vec_))
    , owns_data_(buff.owns_data_)
    , preallocated_(buff.preallocated_) {
  offset_ = std::exchange(buff.offset_, 0);
}

template <class Alloc>
OwningMemoryBuffer<Alloc>::OwningMemoryBuffer(
    OwningMemoryBuffer<Alloc>&& buff, const allocator_type& alloc)
    : BufferBase(
          std::exchange(buff.data_, nullptr), std::exchange(buff.size_, 0))
    , vec_(std::move(buff.vec_), alloc)
    , owns_data_(buff.owns_data_)
    , preallocated_(buff.preallocated_) {
  offset_ = std::exchange(buff.offset_, 0);
}

template <class Alloc>
void* OwningMemoryBuffer<Alloc>::data() const {
  return nonconst_data();
}

template <class Alloc>
void OwningMemoryBuffer<Alloc>::advance_size(const uint64_t nbytes) {
  assert(owns_data_);
  size_ += nbytes;
}

template <class Alloc>
uint64_t OwningMemoryBuffer<Alloc>::alloced_size() const {
  return vec_.size();
}

template <class Alloc>
void OwningMemoryBuffer<Alloc>::clear() {
  if (data_ != nullptr && owns_data_) {
    vec_.clear();
  }

  data_ = nullptr;
  offset_ = 0;
  size_ = 0;
}

template <class Alloc>
void* OwningMemoryBuffer<Alloc>::cur_data() const {
  return nonconst_unread_data();
}

template <class Alloc>
void* OwningMemoryBuffer<Alloc>::data(const uint64_t offset) const {
  auto data = static_cast<char*>(nonconst_data());
  if (data == nullptr) {
    return nullptr;
  }
  return data + offset;
}

template <class Alloc>
uint64_t OwningMemoryBuffer<Alloc>::free_space() const {
  assert(alloced_size() >= size_);
  return alloced_size() - size_;
}

template <class Alloc>
bool OwningMemoryBuffer<Alloc>::owns_data() const {
  return owns_data_;
}

template <class Alloc>
Status OwningMemoryBuffer<Alloc>::realloc(const uint64_t nbytes) {
  if (!owns_data_) {
    return LOG_STATUS(Status_BufferError(
        "Cannot reallocate buffer; Buffer does not own data"));
  }

  if (nbytes > alloced_size()) {
    vec_.resize(nbytes);
    data_ = vec_.data();
  }

  return Status::Ok();
}

template <class Alloc>
void OwningMemoryBuffer<Alloc>::reset_size() {
  offset_ = 0;
  size_ = 0;
}

template <class Alloc>
void OwningMemoryBuffer<Alloc>::set_size(const uint64_t size) {
  size_ = size;
}

template <class Alloc>
void OwningMemoryBuffer<Alloc>::swap(OwningMemoryBuffer<Alloc>& other) {
  std::swap(vec_, other.vec_);
  std::swap(data_, other.data_);
  std::swap(offset_, other.offset_);
  std::swap(owns_data_, other.owns_data_);
  std::swap(size_, other.size_);
  std::swap(preallocated_, other.preallocated_);
}

template <class Alloc>
Status OwningMemoryBuffer<Alloc>::write(ConstBuffer* buff) {
  // Sanity check
  if (!owns_data_)
    return LOG_STATUS(Status_BufferError(
        "Cannot write to buffer; Buffer does not own the already stored data"));

  const uint64_t bytes_left_to_write = alloced_size() - offset_;
  const uint64_t bytes_left_to_read = buff->nbytes_left_to_read();
  const uint64_t bytes_to_copy =
      std::min(bytes_left_to_write, bytes_left_to_read);

  RETURN_NOT_OK(buff->read((char*)data_ + offset_, bytes_to_copy));
  offset_ += bytes_to_copy;
  size_ = std::max(offset_, size_);

  return Status::Ok();
}

template <class Alloc>
Status OwningMemoryBuffer<Alloc>::write(
    ConstBuffer* buff, const uint64_t nbytes) {
  // Sanity check
  if (!owns_data_)
    return LOG_STATUS(Status_BufferError(
        "Cannot write to buffer; Buffer does not own the already stored data"));

  RETURN_NOT_OK(ensure_alloced_size(offset_ + nbytes));

  RETURN_NOT_OK(buff->read((char*)data_ + offset_, nbytes));
  offset_ += nbytes;
  size_ = std::max(offset_, size_);

  return Status::Ok();
}

template <class Alloc>
Status OwningMemoryBuffer<Alloc>::write(
    const void* buffer, const uint64_t nbytes) {
  // Sanity check
  if (!owns_data_)
    return LOG_STATUS(Status_BufferError(
        "Cannot write to buffer; Buffer does not own the already stored data"));

  RETURN_NOT_OK(ensure_alloced_size(offset_ + nbytes));

  std::memcpy((char*)data_ + offset_, buffer, nbytes);
  offset_ += nbytes;
  size_ = std::max(offset_, size_);

  return Status::Ok();
}

template <class Alloc>
Status OwningMemoryBuffer<Alloc>::write(
    const void* buffer, const uint64_t offset, const uint64_t nbytes) {
  // Sanity check
  if (!owns_data_)
    return LOG_STATUS(Status_BufferError(
        "Cannot write to buffer; Buffer does not own the already stored data"));

  RETURN_NOT_OK(ensure_alloced_size(offset + nbytes));

  std::memcpy((char*)data_ + offset, buffer, nbytes);
  size_ = std::max(offset + nbytes, size_);

  return Status::Ok();
}

template <class Alloc>
OwningMemoryBuffer<Alloc>& OwningMemoryBuffer<Alloc>::operator=(
    const OwningMemoryBuffer<Alloc>& buff) {
  if (this == &buff)
    return *this;

  // Clear any existing allocation.
  clear();

  // Create a copy and swap with the copy.
  auto tmp(buff);
  swap(tmp);

  return *this;
}

template <class Alloc>
OwningMemoryBuffer<Alloc>& OwningMemoryBuffer<Alloc>::operator=(
    OwningMemoryBuffer<Alloc>&& buff) noexcept {
  if (this == &buff)
    return *this;
  swap(buff);
  return *this;
}

template <class Alloc>
Status OwningMemoryBuffer<Alloc>::ensure_alloced_size(const uint64_t nbytes) {
  if (preallocated_ && nbytes > alloced_size()) {
    throw BufferStatusException(
        "Failed to reallocate. Buffer is preallocated to a fixed size.");
  } else if (preallocated_ || alloced_size() >= nbytes) {
    return Status::Ok();
  }

  auto new_alloc_size = alloced_size() == 0 ? nbytes : alloced_size();
  while (new_alloc_size < nbytes)
    new_alloc_size *= 2;

  return this->realloc(new_alloc_size);
}

/* ****************************** */
/*          ConstBuffer           */
/* ****************************** */

ConstBuffer::ConstBuffer(Buffer* buff)
    : ConstBuffer(buff->data(), buff->size()) {
}

ConstBuffer::ConstBuffer(const void* data, const uint64_t size)
    : BufferBase(data, size) {
}

uint64_t ConstBuffer::nbytes_left_to_read() const {
  return size_ - offset_;
}

/* ****************************** */
/*       PreallocatedBuffer       */
/* ****************************** */

PreallocatedBuffer::PreallocatedBuffer(const void* data, const uint64_t size)
    : BufferBase(data, size) {
}

void* PreallocatedBuffer::cur_data() const {
  return nonconst_unread_data();
}

uint64_t PreallocatedBuffer::free_space() const {
  return size_ - offset_;
}

Status PreallocatedBuffer::write(const void* buffer, const uint64_t nbytes) {
  if (nbytes > size_ - offset_)
    return Status_PreallocatedBufferError("Write would overflow buffer.");

  std::memcpy((char*)data_ + offset_, buffer, nbytes);
  offset_ += nbytes;

  return Status::Ok();
}

// Explicit template instantiations.
template class OwningMemoryBuffer<std::allocator<uint8_t>>;
template class OwningMemoryBuffer<tdb::pmr::polymorphic_allocator<uint8_t>>;

}  // namespace sm
}  // namespace tiledb
