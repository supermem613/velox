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
#include "velox/exec/DistinctAggregations.h"
#include "velox/exec/SetAccumulator.h"

namespace facebook::velox::exec {

namespace {

template <typename T>
class TypedDistinctAggregations : public DistinctAggregations {
 public:
  TypedDistinctAggregations(
      std::vector<AggregateInfo*> aggregates,
      const RowTypePtr& inputType,
      memory::MemoryPool* pool)
      : pool_{pool},
        aggregates_{std::move(aggregates)},
        inputs_{aggregates_[0]->inputs},
        inputType_(TypedDistinctAggregations::makeInputTypeForAccumulator(
            inputType,
            inputs_)) {}

  using AccumulatorType = aggregate::prestosql::SetAccumulator<T>;

  /// Returns metadata about the accumulator used to store unique inputs.
  Accumulator accumulator() const override {
    return {
        false, // isFixedSize
        sizeof(AccumulatorType),
        false, // usesExternalMemory
        1, // alignment
        ARRAY(VARBINARY()),
        [this](folly::Range<char**> groups, VectorPtr& result) {
          extractForSpill(groups, result);
        },
        [this](folly::Range<char**> groups) {
          for (auto* group : groups) {
            auto* accumulator =
                reinterpret_cast<AccumulatorType*>(group + offset_);
            accumulator->free(*allocator_);
          }
        }};
  }

  void initializeNewGroups(
      char** groups,
      folly::Range<const vector_size_t*> indices) override {
    for (auto i : indices) {
      groups[i][nullByte_] |= nullMask_;
      new (groups[i] + offset_) AccumulatorType(inputType_, allocator_);
    }

    for (auto i = 0; i < aggregates_.size(); ++i) {
      const auto& aggregate = *aggregates_[i];
      aggregate.function->initializeNewGroups(groups, indices);
    }
  }

  void addInput(
      char** groups,
      const RowVectorPtr& input,
      const SelectivityVector& rows) override {
    decodeInput(input, rows);

    rows.applyToSelected([&](vector_size_t i) {
      auto* group = groups[i];
      auto* accumulator = reinterpret_cast<AccumulatorType*>(group + offset_);

      RowSizeTracker<char, uint32_t> tracker(
          group[rowSizeOffset_], *allocator_);
      accumulator->addValue(decodedInput_, i, allocator_);
    });

    inputForAccumulator_.reset();
  }

  void addSingleGroupInput(
      char* group,
      const RowVectorPtr& input,
      const SelectivityVector& rows) override {
    decodeInput(input, rows);

    auto* accumulator = reinterpret_cast<AccumulatorType*>(group + offset_);
    RowSizeTracker<char, uint32_t> tracker(group[rowSizeOffset_], *allocator_);
    rows.applyToSelected([&](vector_size_t i) {
      accumulator->addValue(decodedInput_, i, allocator_);
    });

    inputForAccumulator_.reset();
  }

  void addSingleGroupSpillInput(
      char* group,
      const VectorPtr& input,
      vector_size_t index) override {
    auto* arrayVector = input->as<ArrayVector>();
    auto* elementsVector = arrayVector->elements()->asFlatVector<StringView>();

    const auto size = arrayVector->sizeAt(index);
    const auto offset = arrayVector->offsetAt(index);
    auto* accumulator = reinterpret_cast<AccumulatorType*>(group + offset_);

    accumulator->addFromSpill(*elementsVector, allocator_);
  }

  void extractValues(folly::Range<char**> groups, const RowVectorPtr& result)
      override {
    SelectivityVector rows;
    for (auto i = 0; i < aggregates_.size(); ++i) {
      const auto& aggregate = *aggregates_[i];

      // For each group, add distinct inputs to aggregate.
      for (auto* group : groups) {
        auto* accumulator = reinterpret_cast<AccumulatorType*>(group + offset_);

        // TODO Process group rows in batches to avoid creating very large input
        // vectors.
        auto data = BaseVector::create(inputType_, accumulator->size(), pool_);
        if constexpr (std::is_same_v<T, ComplexType>) {
          accumulator->extractValues(*data, 0);
        } else {
          accumulator->extractValues(*(data->template as<FlatVector<T>>()), 0);
        }

        rows.resize(data->size());
        std::vector<VectorPtr> inputForAggregation_ =
            makeInputForAggregation(data);
        aggregate.function->addSingleGroupRawInput(
            group, rows, inputForAggregation_, false);
      }

      aggregate.function->extractValues(
          groups.data(), groups.size(), &result->childAt(aggregate.output));

      // Release memory back to HashStringAllocator to allow next
      // aggregate to re-use it.
      aggregate.function->destroy(groups);
    }
  }

 private:
  bool isSingleInputAggregate() const {
    return aggregates_[0]->inputs.size() == 1;
  }

  void decodeInput(const RowVectorPtr& input, const SelectivityVector& rows) {
    inputForAccumulator_ = makeInputForAccumulator(input);
    decodedInput_.decode(*inputForAccumulator_, rows);
  }

  static TypePtr makeInputTypeForAccumulator(
      const RowTypePtr& rowType,
      const std::vector<column_index_t>& inputs) {
    if (inputs.size() == 1) {
      return rowType->childAt(inputs[0]);
    }

    // Otherwise, synthesize a ROW(distinct_channels[0..N])
    std::vector<TypePtr> types;
    std::vector<std::string> names;
    for (column_index_t channelIndex : inputs) {
      names.emplace_back(rowType->nameOf(channelIndex));
      types.emplace_back(rowType->childAt(channelIndex));
    }
    return ROW(std::move(names), std::move(types));
  }

  VectorPtr makeInputForAccumulator(const RowVectorPtr& input) const {
    if (isSingleInputAggregate()) {
      return input->childAt(inputs_[0]);
    }

    std::vector<VectorPtr> newChildren(inputs_.size());
    for (int i = 0; i < inputs_.size(); ++i) {
      newChildren[i] = input->childAt(inputs_[i]);
    }
    return std::make_shared<RowVector>(
        pool_, inputType_, nullptr, input->size(), newChildren);
  }

  std::vector<VectorPtr> makeInputForAggregation(const VectorPtr& input) const {
    if (isSingleInputAggregate()) {
      return {std::move(input)};
    }
    return input->template asUnchecked<RowVector>()->children();
  }

  void extractForSpill(folly::Range<char**> groups, VectorPtr& result) const {
    auto* arrayVector = result->as<ArrayVector>();
    arrayVector->resize(groups.size());

    auto* rawOffsets =
        arrayVector->mutableOffsets(groups.size())->asMutable<vector_size_t>();
    auto* rawSizes =
        arrayVector->mutableSizes(groups.size())->asMutable<vector_size_t>();

    size_t totalBytes = 0;
    vector_size_t offset = 0;
    for (auto i = 0; i < groups.size(); ++i) {
      auto* accumulator =
          reinterpret_cast<AccumulatorType*>(groups[i] + offset_);
      rawSizes[i] = accumulator->maxSpillSize();
      rawOffsets[i] = offset;
      offset += accumulator->maxSpillSize();
      totalBytes += accumulator->maxSpillSize();
    }

    auto& elementsVector = arrayVector->elements();
    elementsVector->resize(totalBytes);

    auto* flatVector = arrayVector->elements()->asFlatVector<StringView>();
    flatVector->resize(1);
    auto* rawBuffer = flatVector->getRawStringBufferWithSpace(totalBytes, true);

    offset = 0;
    for (auto i = 0; i < groups.size(); ++i) {
      auto* accumulator =
          reinterpret_cast<AccumulatorType*>(groups[i] + offset_);

      offset +=
          accumulator->extractForSpill(rawBuffer + offset, totalBytes - offset);

      accumulator->clear(*allocator_);
    }

    flatVector->setNoCopy(0, StringView(rawBuffer, offset));
  }

  memory::MemoryPool* const pool_;
  const std::vector<AggregateInfo*> aggregates_;
  const std::vector<column_index_t> inputs_;
  const TypePtr inputType_;

  DecodedVector decodedInput_;
  VectorPtr inputForAccumulator_;
};

} // namespace

// static
std::unique_ptr<DistinctAggregations> DistinctAggregations::create(
    std::vector<AggregateInfo*> aggregates,
    const RowTypePtr& inputType,
    memory::MemoryPool* pool) {
  VELOX_CHECK_EQ(aggregates.size(), 1);
  VELOX_CHECK(!aggregates[0]->inputs.empty());

  const bool isSingleInput = aggregates[0]->inputs.size() == 1;
  if (!isSingleInput) {
    return std::make_unique<TypedDistinctAggregations<ComplexType>>(
        aggregates, inputType, pool);
  }

  const auto type = inputType->childAt(aggregates[0]->inputs[0]);

  switch (type->kind()) {
    case TypeKind::BOOLEAN:
      return std::make_unique<TypedDistinctAggregations<bool>>(
          aggregates, inputType, pool);
    case TypeKind::TINYINT:
      return std::make_unique<TypedDistinctAggregations<int8_t>>(
          aggregates, inputType, pool);
    case TypeKind::SMALLINT:
      return std::make_unique<TypedDistinctAggregations<int16_t>>(
          aggregates, inputType, pool);
    case TypeKind::INTEGER:
      return std::make_unique<TypedDistinctAggregations<int32_t>>(
          aggregates, inputType, pool);
    case TypeKind::BIGINT:
      return std::make_unique<TypedDistinctAggregations<int64_t>>(
          aggregates, inputType, pool);
    case TypeKind::REAL:
      return std::make_unique<TypedDistinctAggregations<float>>(
          aggregates, inputType, pool);
    case TypeKind::DOUBLE:
      return std::make_unique<TypedDistinctAggregations<double>>(
          aggregates, inputType, pool);
    case TypeKind::TIMESTAMP:
      return std::make_unique<TypedDistinctAggregations<Timestamp>>(
          aggregates, inputType, pool);
    case TypeKind::VARCHAR:
      return std::make_unique<TypedDistinctAggregations<StringView>>(
          aggregates, inputType, pool);
    case TypeKind::ARRAY:
    case TypeKind::MAP:
    case TypeKind::ROW:
      return std::make_unique<TypedDistinctAggregations<ComplexType>>(
          aggregates, inputType, pool);
    default:
      VELOX_UNREACHABLE("Unexpected type {}", type->toString());
  }
}

} // namespace facebook::velox::exec
