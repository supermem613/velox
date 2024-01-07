/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "velox/exec/AddressableNonNullValueList.h"
#include "velox/exec/ContainerRowSerde.h"

namespace facebook::velox::aggregate::prestosql {

HashStringAllocator::Position AddressableNonNullValueList::append(
    const DecodedVector& decoded,
    vector_size_t index,
    HashStringAllocator* allocator) {
  auto stream = makeOutputStream(allocator);

  // Write hash.
  stream->appendOne(decoded.base()->hashValueAt(decoded.index(index)));
  // Write value.
  exec::ContainerRowSerde::serialize(
      *decoded.base(), decoded.index(index), *stream);

  return finishWrite(allocator, std::move(stream));
}

HashStringAllocator::Position AddressableNonNullValueList::appendSerialized(
    HashStringAllocator* allocator,
    const char* buffer,
    size_t size) {
  auto stream = makeOutputStream(allocator);

  stream->append(folly::Range<const char*>(buffer, size));

  return finishWrite(allocator, std::move(stream));
}

std::unique_ptr<ByteOutputStream> AddressableNonNullValueList::makeOutputStream(
    HashStringAllocator* allocator) {
  std::unique_ptr<ByteOutputStream> stream{
      std::make_unique<ByteOutputStream>(allocator)};

  if (!firstHeader_) {
    // An array_agg or related begins with an allocation of 5 words and
    // 4 bytes for header. This is compact for small arrays (up to 5
    // bigints) and efficient if needs to be extended (stores 4 bigints
    // and a next pointer. This could be adaptive, with smaller initial
    // sizes for lots of small arrays.
    static constexpr int kInitialSize = 44;

    currentPosition_ = allocator->newWrite(*stream, kInitialSize);
    firstHeader_ = currentPosition_.header;
  } else {
    allocator->extendWrite(currentPosition_, *stream);
  }

  return stream;
}

HashStringAllocator::Position AddressableNonNullValueList::finishWrite(
    HashStringAllocator* allocator,
    std::unique_ptr<ByteOutputStream>&& stream) {
  ++size_;

  auto startAndFinish = allocator->finishWrite(*stream, 1024);
  currentPosition_ = startAndFinish.second;
  return startAndFinish.first;
}

namespace {

ByteInputStream prepareRead(
    HashStringAllocator::Position position,
    bool skipHash) {
  auto header = position.header;
  auto seek = static_cast<int32_t>(position.position - header->begin());

  auto stream = HashStringAllocator::prepareRead(header);
  stream.seekp(seek);
  if (skipHash) {
    stream.skip(sizeof(uint64_t));
  }
  return stream;
}
} // namespace

// static
bool AddressableNonNullValueList::equalTo(
    HashStringAllocator::Position left,
    HashStringAllocator::Position right,
    const TypePtr& type) {
  auto leftStream = prepareRead(left, true /*skipHash*/);
  auto rightStream = prepareRead(right, true /*skipHash*/);

  CompareFlags compareFlags =
      CompareFlags::equality(CompareFlags::NullHandlingMode::kNullAsValue);
  return exec::ContainerRowSerde::compare(
             leftStream, rightStream, type.get(), compareFlags) == 0;
}

// static
uint64_t AddressableNonNullValueList::readHash(
    HashStringAllocator::Position position) {
  auto stream = prepareRead(position, false /*skipHash*/);
  return stream.read<uint64_t>();
}

// static
void AddressableNonNullValueList::read(
    HashStringAllocator::Position position,
    BaseVector& result,
    vector_size_t index) {
  auto stream = prepareRead(position, true /*skipHash*/);
  exec::ContainerRowSerde::deserialize(stream, index, &result);
}

// static
size_t AddressableNonNullValueList::getSerializedSize(
    HashStringAllocator::Position position) {
  // Note that we need the hash to make sure it's there when we put back.
  return prepareRead(position, false /*skipHash*/).size();
}

// static
size_t AddressableNonNullValueList::copySerializedTo(
    HashStringAllocator::Position position,
    char* buffer,
    size_t size) {
  auto stream = prepareRead(position, false /*skipHash*/);

  const auto streamSize = stream.size();

  VELOX_CHECK_GE(size, streamSize);

  stream.seekp(0);
  stream.readBytes(buffer, streamSize);

  return streamSize;
}

} // namespace facebook::velox::aggregate::prestosql
