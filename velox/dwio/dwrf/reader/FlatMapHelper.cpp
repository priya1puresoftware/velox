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

#include "velox/dwio/dwrf/reader/FlatMapHelper.h"
#include "velox/dwio/common/exception/Exceptions.h"

namespace facebook::velox::dwrf::flatmap_helper {

void reset(BaseVector& vector, vector_size_t size, bool hasNulls) {
  if (hasNulls != vector.mayHaveNulls()) {
    if (hasNulls) {
      vector.mutableNulls(size);
    } else {
      vector.resetNulls();
    }
  }
  vector.resize(size);
  vector.setSize(0);
}

// initialize flat vector that can fit all the vectors appended
template <TypeKind K>
void initializeFlatVector(
    VectorPtr& vector,
    memory::MemoryPool& pool,
    const std::vector<const BaseVector*>& vectors) {
  using NativeType = typename velox::TypeTraits<K>::NativeType;
  vector_size_t size = 0;
  bool hasNulls = false;
  for (auto vec : vectors) {
    auto& flatVector = dynamic_cast<const FlatVector<NativeType>&>(*vec);
    size += vec->size();
    hasNulls = hasNulls || vec->mayHaveNulls();
  }
  initializeFlatVector<NativeType>(vector, pool, size, hasNulls);
}

void initializeStringVector(
    VectorPtr& vector,
    memory::MemoryPool& pool,
    const std::vector<const BaseVector*>& vectors) {
  vector_size_t size = 0;
  bool hasNulls = false;
  std::vector<BufferPtr> buffers;
  for (auto vec : vectors) {
    size += vec->size();
    hasNulls = hasNulls || vec->mayHaveNulls();
    const std::vector<BufferPtr>* src = nullptr;

    src = &dynamic_cast<const FlatVector<StringView>&>(*vec->wrappedVector())
               .stringBuffers();
    for (auto& buffer : *src) {
      buffers.push_back(buffer);
    }
  }
  initializeFlatVector<StringView>(
      vector, pool, size, hasNulls, std::move(buffers));
}

template <TypeKind K>
void initializeVectorImpl(
    VectorPtr& vector,
    const std::shared_ptr<const Type>& /* type */,
    memory::MemoryPool& pool,
    const std::vector<const BaseVector*>& vectors) {
  initializeFlatVector<K>(vector, pool, vectors);
}

template <>
void initializeVectorImpl<TypeKind::VARCHAR>(
    VectorPtr& vector,
    const std::shared_ptr<const Type>& /* type */,
    memory::MemoryPool& pool,
    const std::vector<const BaseVector*>& vectors) {
  initializeStringVector(vector, pool, vectors);
}

template <>
void initializeVectorImpl<TypeKind::VARBINARY>(
    VectorPtr& vector,
    const std::shared_ptr<const Type>& /* type */,
    memory::MemoryPool& pool,
    const std::vector<const BaseVector*>& vectors) {
  initializeStringVector(vector, pool, vectors);
}

namespace {
void addVector(
    std::vector<const BaseVector*>& vectors,
    const BaseVector* vector) {
  if (vector && vector->size()) {
    vectors.push_back(vector);
  }
}
} // namespace

template <>
void initializeVectorImpl<TypeKind::ARRAY>(
    VectorPtr& vector,
    const std::shared_ptr<const Type>& type,
    memory::MemoryPool& pool,
    const std::vector<const BaseVector*>& vectors) {
  vector_size_t size = 0;
  bool hasNulls = false;
  std::vector<const BaseVector*> elements;
  for (auto vec : vectors) {
    size += vec->size();
    hasNulls = hasNulls || vec->mayHaveNulls();
    auto& array = dynamic_cast<const ArrayVector&>(*vec);
    addVector(elements, array.elements().get());
  }

  VectorPtr elementsVector;
  if (vector) {
    auto& arrayVector = dynamic_cast<ArrayVector&>(*vector);
    reset(arrayVector, size, hasNulls);
    elementsVector = arrayVector.elements();
  }
  if (elements.size() > 0) {
    initializeVector(
        elementsVector, type->asArray().elementType(), pool, elements);
  }

  if (!vector) {
    auto arrayType = elementsVector ? ARRAY(elementsVector->type()) : type;
    vector = std::make_shared<ArrayVector>(
        &pool,
        arrayType,
        hasNulls ? AlignedBuffer::allocate<bool>(size, &pool) : nullptr,
        0 /* length */,
        AlignedBuffer::allocate<vector_size_t>(size, &pool),
        AlignedBuffer::allocate<vector_size_t>(size, &pool),
        elementsVector,
        0 /* nullCount */);
  }
}

void initializeMapVector(
    VectorPtr& vector,
    const std::shared_ptr<const Type>& type,
    memory::MemoryPool& pool,
    const std::vector<const BaseVector*>& vectors,
    std::optional<vector_size_t> sizeOverride) {
  vector_size_t size = 0;
  bool hasNulls = false;
  std::vector<const BaseVector*> keys;
  std::vector<const BaseVector*> values;
  for (auto vec : vectors) {
    size += vec->size();
    hasNulls = hasNulls || vec->mayHaveNulls();
    auto& map = dynamic_cast<const MapVector&>(*vec);
    addVector(keys, map.mapKeys().get());
    addVector(values, map.mapValues().get());
  }

  if (sizeOverride.has_value()) {
    size = sizeOverride.value();
  }

  VectorPtr keysVector;
  VectorPtr valuesVector;
  if (vector) {
    auto& mapVector = dynamic_cast<MapVector&>(*vector);
    reset(mapVector, size, hasNulls);
    keysVector = mapVector.mapKeys();
    valuesVector = mapVector.mapValues();
  }
  auto& mapType = type->asMap();
  if (keys.size() > 0) {
    initializeVector(keysVector, mapType.keyType(), pool, keys);
  }
  if (values.size() > 0) {
    initializeVector(valuesVector, mapType.valueType(), pool, values);
  }

  if (!vector) {
    // When read-string-as-row flag is on, string readers produce ROW(BIGINT,
    // BIGINT) type instead of VARCHAR or VARBINARY. In these cases, type_->type
    // is not the right type of the final vector.
    auto resultMapType = (keysVector == nullptr || valuesVector == nullptr)
        ? type
        : MAP(keysVector->type(), valuesVector->type());

    vector = std::make_shared<MapVector>(
        &pool,
        resultMapType,
        hasNulls ? AlignedBuffer::allocate<bool>(size, &pool) : nullptr,
        0 /* length */,
        AlignedBuffer::allocate<vector_size_t>(size, &pool),
        AlignedBuffer::allocate<vector_size_t>(size, &pool),
        keysVector,
        valuesVector,
        0 /* nullCount */);
  }
}

template <>
void initializeVectorImpl<TypeKind::MAP>(
    VectorPtr& vector,
    const std::shared_ptr<const Type>& type,
    memory::MemoryPool& pool,
    const std::vector<const BaseVector*>& vectors) {
  initializeMapVector(vector, type, pool, vectors);
}

template <>
void initializeVectorImpl<TypeKind::ROW>(
    VectorPtr& vector,
    const std::shared_ptr<const Type>& type,
    memory::MemoryPool& pool,
    const std::vector<const BaseVector*>& vectors) {
  vector_size_t size = 0;
  bool hasNulls = false;
  auto& rowType = type->asRow();
  std::vector<std::vector<const BaseVector*>> fields{rowType.size()};
  for (auto vec : vectors) {
    size += vec->size();
    hasNulls = hasNulls || vec->mayHaveNulls();
    auto& row = dynamic_cast<const RowVector&>(*vec);
    for (size_t col = 0; col < rowType.size(); ++col) {
      fields.at(col).push_back(row.childAt(col).get());
    }
  }
  if (vector) {
    auto& rowVector = dynamic_cast<RowVector&>(*vector);
    reset(rowVector, size, hasNulls);
    for (size_t col = 0; col < rowType.size(); ++col) {
      initializeVector(
          rowVector.childAt(col), rowType.childAt(col), pool, fields.at(col));
    }
  } else {
    std::vector<VectorPtr> children;
    children.reserve(rowType.size());
    for (size_t col = 0; col < rowType.size(); ++col) {
      initializeVector(
          children.emplace_back(), rowType.childAt(col), pool, fields.at(col));
    }

    // When read-string-as-row flag is on, string readers produce ROW(BIGINT,
    // BIGINT) type instead of VARCHAR or VARBINARY. In these cases, type_->type
    // is not the right type of the final struct.
    std::vector<TypePtr> types;
    types.reserve(children.size());
    for (auto i = 0; i < children.size(); i++) {
      const auto& child = children[i];
      if (child) {
        types.emplace_back(child->type());
      } else {
        types.emplace_back(type->childAt(i));
      }
    }

    vector = std::make_shared<RowVector>(
        &pool,
        ROW(std::move(types)),
        hasNulls ? AlignedBuffer::allocate<bool>(size, &pool, true) : nullptr,
        0 /* length */,
        children,
        0 /* nullCount */);
  }
}

void initializeVector(
    VectorPtr& vector,
    const std::shared_ptr<const Type>& type,
    memory::MemoryPool& pool,
    const std::vector<const BaseVector*>& vectors) {
  VELOX_DYNAMIC_TYPE_DISPATCH(
      initializeVectorImpl, type->kind(), vector, type, pool, vectors);
}

// copy nulls from source to target, return number of nulls copied
vector_size_t copyNulls(
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex,
    vector_size_t count) {
  vector_size_t nulls = 0;
  // it's assumed that initVector is called before calling this method to
  // properly allocate/clear nulls buffer. So we only need to check against
  // target vector here.
  if (target.mayHaveNulls()) {
    auto tgtNulls = const_cast<uint64_t*>(target.rawNulls());
    auto srcNulls = source.rawNulls();
    if (srcNulls) {
      for (vector_size_t i = 0; i < count; ++i) {
        if (bits::isBitNull(srcNulls, sourceIndex + i)) {
          ++nulls;
          bits::setNull(tgtNulls, targetIndex + i);
        } else {
          bits::clearNull(tgtNulls, targetIndex + i);
        }
      }
    } else {
      for (vector_size_t i = 0; i < count; ++i) {
        bits::clearNull(tgtNulls, targetIndex + i);
      }
    }
  }
  target.setSize(targetIndex + count);
  return nulls;
}

template <TypeKind K>
void copyImpl(
    const std::shared_ptr<const Type>& /* type */,
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex,
    vector_size_t count) {
  // copy values if not all are nulls
  if (copyNulls(target, targetIndex, source, sourceIndex, count) != count) {
    using NativeType = typename velox::TypeTraits<K>::NativeType;
    auto& tgt = static_cast<FlatVector<NativeType>&>(target);
    auto& src = static_cast<const FlatVector<NativeType>&>(source);
    std::memcpy(
        const_cast<NativeType*>(tgt.rawValues()) + targetIndex,
        src.rawValues() + sourceIndex,
        count * sizeof(NativeType));
  }
}

template <>
void copyImpl<TypeKind::BOOLEAN>(
    const std::shared_ptr<const Type>& /* type */,
    BaseVector& /* target */,
    vector_size_t /* targetIndex */,
    const BaseVector& /* source */,
    vector_size_t /* sourceIndex */,
    vector_size_t /* count */) {
  DWIO_RAISE("not implemented");
}

void copyStrings(
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex,
    vector_size_t count) {
  // copy values if not all are nulls
  if (copyNulls(target, targetIndex, source, sourceIndex, count) != count) {
    auto& tgt = static_cast<FlatVector<StringView>&>(target);
    auto& src = static_cast<const FlatVector<StringView>&>(source);
    std::memcpy(
        const_cast<StringView*>(tgt.rawValues()) + targetIndex,
        src.rawValues() + sourceIndex,
        count * sizeof(StringView));
  }
}

template <>
void copyImpl<TypeKind::VARCHAR>(
    const std::shared_ptr<const Type>& /* type */,
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex,
    vector_size_t count) {
  copyStrings(target, targetIndex, source, sourceIndex, count);
}

template <>
void copyImpl<TypeKind::VARBINARY>(
    const std::shared_ptr<const Type>& /* type */,
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex,
    vector_size_t count) {
  copyStrings(target, targetIndex, source, sourceIndex, count);
}

// copy offsets/lengths from source vector to target
template <typename T>
vector_size_t copyOffsets(
    T& target,
    vector_size_t targetIndex,
    const T& source,
    vector_size_t sourceIndex,
    vector_size_t count,
    vector_size_t& childOffset) {
  childOffset = 0;
  auto tgtOffsets = const_cast<vector_size_t*>(target.rawOffsets());
  auto tgtSizes = const_cast<vector_size_t*>(target.rawSizes());
  if (LIKELY(targetIndex > 0)) {
    auto index = targetIndex - 1;
    childOffset = tgtOffsets[index] + tgtSizes[index];
  }
  auto srcSizes = source.rawSizes();
  auto nextChildOffset = childOffset;
  // If there is null, process one at a time with null checks. In order to make
  // it easier for computing child offset, we always fill offset/length even if
  // value is null.
  if (copyNulls(target, targetIndex, source, sourceIndex, count) > 0) {
    auto tgtNulls = target.rawNulls();
    vector_size_t size;
    for (vector_size_t i = 0; i < count; ++i) {
      auto index = targetIndex + i;
      tgtOffsets[index] = nextChildOffset;
      size = bits::isBitNull(tgtNulls, index) ? 0 : srcSizes[sourceIndex + i];
      tgtSizes[index] = size;
      nextChildOffset += size;
    }
  } else {
    std::memcpy(
        tgtSizes + targetIndex,
        srcSizes + sourceIndex,
        count * sizeof(vector_size_t));
    for (vector_size_t i = 0; i < count; ++i) {
      auto index = targetIndex + i;
      tgtOffsets[index] = nextChildOffset;
      nextChildOffset += tgtSizes[index];
    }
  }
  return nextChildOffset;
}

template <>
void copyImpl<TypeKind::ARRAY>(
    const std::shared_ptr<const Type>& type,
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex,
    vector_size_t count) {
  auto& tgt = static_cast<ArrayVector&>(target);
  auto& src = static_cast<const ArrayVector&>(source);
  vector_size_t childOffset = 0;
  auto nextChildOffset =
      copyOffsets(tgt, targetIndex, src, sourceIndex, count, childOffset);
  auto size = nextChildOffset - childOffset;
  if (size > 0) {
    auto& arrayType = static_cast<const ArrayType&>(*type);
    // we assume child values are placed continuously in the source vector,
    // which is the case if it's produced by the column reader
    copy(
        arrayType.elementType(),
        *tgt.elements(),
        childOffset,
        *src.elements(),
        src.rawOffsets()[sourceIndex],
        size);
  }
}

template <>
void copyImpl<TypeKind::MAP>(
    const std::shared_ptr<const Type>& type,
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex,
    vector_size_t count) {
  auto& tgt = static_cast<MapVector&>(target);
  auto& src = static_cast<const MapVector&>(source);
  vector_size_t childOffset = 0;
  auto nextChildOffset =
      copyOffsets(tgt, targetIndex, src, sourceIndex, count, childOffset);
  auto size = nextChildOffset - childOffset;
  if (size > 0) {
    auto& mapType = type->asMap();
    auto srcChildOffset = src.rawOffsets()[sourceIndex];
    // we assume child values are placed continuously in the source vector,
    // which is the case if it's produced by the column reader
    copy(
        mapType.keyType(),
        *tgt.mapKeys(),
        childOffset,
        *src.mapKeys(),
        srcChildOffset,
        size);
    copy(
        mapType.valueType(),
        *tgt.mapValues(),
        childOffset,
        *src.mapValues(),
        srcChildOffset,
        size);
  }
}

template <>
void copyImpl<TypeKind::ROW>(
    const std::shared_ptr<const Type>& type,
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex,
    vector_size_t count) {
  if (copyNulls(target, targetIndex, source, sourceIndex, count) != count) {
    auto& src = static_cast<const RowVector&>(source);
    auto& tgt = static_cast<RowVector&>(target);
    auto& rowType = static_cast<const RowType&>(*type);
    for (size_t i = 0; i < src.childrenSize(); ++i) {
      copy(
          rowType.childAt(i),
          *tgt.childAt(i),
          targetIndex,
          *src.childAt(i),
          sourceIndex,
          count);
    }
  }
}

void copy(
    const std::shared_ptr<const Type>& type,
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex,
    vector_size_t count) {
  VELOX_DYNAMIC_TYPE_DISPATCH(
      copyImpl,
      type->kind(),
      type,
      target,
      targetIndex,
      source,
      sourceIndex,
      count);
}

// copy null from source to target, return true if the value copied is null
bool copyNull(
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex) {
  bool srcIsNull =
      source.mayHaveNulls() && bits::isBitNull(source.rawNulls(), sourceIndex);
  // it's assumed that initVector is called before calling this method to
  // properly allocate/clear nulls buffer. So we only need to check against
  // target vector here.
  if (target.mayHaveNulls()) {
    bits::setNull(
        const_cast<uint64_t*>(target.rawNulls()), targetIndex, srcIsNull);
  }
  target.setSize(targetIndex + 1);
  return srcIsNull;
}

template <TypeKind K>
void copyOneImpl(
    const std::shared_ptr<const Type>& /* type */,
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex) {
  // copy value if not null
  if (!copyNull(target, targetIndex, source, sourceIndex)) {
    using NativeType = typename velox::TypeTraits<K>::NativeType;
    auto& tgt = static_cast<FlatVector<NativeType>&>(target);
    auto& src = static_cast<const FlatVector<NativeType>&>(source);
    const_cast<NativeType*>(tgt.rawValues())[targetIndex] =
        src.rawValues()[sourceIndex];
  }
}

template <>
void copyOneImpl<TypeKind::BOOLEAN>(
    const std::shared_ptr<const Type>& /* type */,
    BaseVector& /* target */,
    vector_size_t /* targetIndex */,
    const BaseVector& /* source */,
    vector_size_t /* sourceIndex */) {
  DWIO_RAISE("not implemented");
}

void copyString(
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex) {
  // copy value if not null
  if (!copyNull(target, targetIndex, source, sourceIndex)) {
    auto& tgt = static_cast<FlatVector<StringView>&>(target);
    auto& src = static_cast<const FlatVector<StringView>&>(source);
    const_cast<StringView*>(tgt.rawValues())[targetIndex] =
        src.rawValues()[sourceIndex];
  }
}

template <>
void copyOneImpl<TypeKind::VARCHAR>(
    const std::shared_ptr<const Type>& /* type */,
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex) {
  copyString(target, targetIndex, source, sourceIndex);
}

template <>
void copyOneImpl<TypeKind::VARBINARY>(
    const std::shared_ptr<const Type>& /* type */,
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex) {
  copyString(target, targetIndex, source, sourceIndex);
}

// copy offset from source to target
template <typename T>
vector_size_t copyOffset(
    T& target,
    vector_size_t targetIndex,
    const T& source,
    vector_size_t sourceIndex,
    vector_size_t& childOffset) {
  childOffset = 0;
  auto tgtSizes = const_cast<vector_size_t*>(target.rawSizes());
  if (LIKELY(targetIndex > 0)) {
    auto index = targetIndex - 1;
    childOffset = target.rawOffsets()[index] + tgtSizes[index];
  }
  const_cast<vector_size_t*>(target.rawOffsets())[targetIndex] = childOffset;
  // In order to make it easier for computing child offset, we always fill
  // offset/length even if value is null.
  auto size = copyNull(target, targetIndex, source, sourceIndex)
      ? 0
      : source.rawSizes()[sourceIndex];
  tgtSizes[targetIndex] = size;
  return size;
}

template <>
void copyOneImpl<TypeKind::ARRAY>(
    const std::shared_ptr<const Type>& type,
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex) {
  auto& tgt = static_cast<ArrayVector&>(target);
  auto& src = static_cast<const ArrayVector&>(source);
  vector_size_t childOffset = 0;
  auto size = copyOffset(tgt, targetIndex, src, sourceIndex, childOffset);
  if (size > 0) {
    auto& arrayType = static_cast<const ArrayType&>(*type);
    // we assume child values are placed continuously in the source vector,
    // which is the case if it's produced by the column reader
    copy(
        arrayType.elementType(),
        *tgt.elements(),
        childOffset,
        *src.elements(),
        src.rawOffsets()[sourceIndex],
        size);
  }
}

template <>
void copyOneImpl<TypeKind::MAP>(
    const std::shared_ptr<const Type>& type,
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex) {
  auto& tgt = static_cast<MapVector&>(target);
  auto& src = static_cast<const MapVector&>(source);
  vector_size_t childOffset = 0;
  auto size = copyOffset(tgt, targetIndex, src, sourceIndex, childOffset);
  if (size > 0) {
    auto& mapType = static_cast<const MapType&>(*type);
    auto srcChildOffset = src.rawOffsets()[sourceIndex];
    // we assume child values are placed continuously in the source vector,
    // which is the case if it's produced by the column reader
    copy(
        mapType.keyType(),
        *tgt.mapKeys(),
        childOffset,
        *src.mapKeys(),
        srcChildOffset,
        size);
    copy(
        mapType.valueType(),
        *tgt.mapValues(),
        childOffset,
        *src.mapValues(),
        srcChildOffset,
        size);
  }
}

template <>
void copyOneImpl<TypeKind::ROW>(
    const std::shared_ptr<const Type>& type,
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex) {
  // copy value if not null
  if (!copyNull(target, targetIndex, source, sourceIndex)) {
    auto& src = static_cast<const RowVector&>(source);
    auto& tgt = static_cast<RowVector&>(target);
    auto& rowType = static_cast<const RowType&>(*type);
    for (size_t i = 0; i < src.childrenSize(); ++i) {
      copyOne(
          rowType.childAt(i),
          *tgt.childAt(i),
          targetIndex,
          *src.childAt(i),
          sourceIndex);
    }
  }
}

void copyOne(
    const std::shared_ptr<const Type>& type,
    BaseVector& target,
    vector_size_t targetIndex,
    const BaseVector& source,
    vector_size_t sourceIndex) {
  VELOX_DYNAMIC_TYPE_DISPATCH(
      copyOneImpl,
      type->kind(),
      type,
      target,
      targetIndex,
      source,
      sourceIndex);
}

} // namespace facebook::velox::dwrf::flatmap_helper