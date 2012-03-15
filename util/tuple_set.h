// Copyright 2010-2012 Google
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Set of integer tuples (fixed-size arrays, all of the same size) with
// a basic API.
// It supports several types of integer arrays transparently, with an
// inherent storage based on int64 arrays.
//
// The key feature is the "lazy" copy:
// - Copying an IntTupleSet won't actually copy the data right away; we
//   will just have several IntTupleSet pointing at the same data.
// - Modifying an IntTupleSet which shares his data with others
//   will create a new, modified instance of the data payload, and make
//   the IntTupleSet point to that new data.
// - Modifying an IntTupleSet that doesn't share its data with any other
//   IntTupleSet will modify the data directly.
// Therefore, you don't need to use const IntTupleSet& in methods. Just do:
// void MyMethod(IntTupleSet tuple_set) { ... }
//
// This class is thread hostile as the copy and reference counter are
// not protected by a mutex.

#ifndef OR_TOOLS_UTIL_TUPLE_SET_H_
#define OR_TOOLS_UTIL_TUPLE_SET_H_

#include "base/hash.h"
#include <vector>

#include "base/integral_types.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/map-util.h"
#include "base/hash.h"

namespace operations_research {
// ----- Main IntTupleSet class -----
class IntTupleSet {
 public:
  // Creates an empty tuple set with a fixed length for all tuples.
  explicit IntTupleSet(int arity);
  // Copy constructor (it actually does a lazy copy, see toplevel comment).
  IntTupleSet(const IntTupleSet& set);  // NOLINT
  ~IntTupleSet();

  // Clears data.
  void Clear();

  // Inserts the tuple to the set. It does nothing if the tuple is already
  // in the set. The size of the tuple must be equal to the arity of
  // the set.
  void Insert(const std::vector<int>& tuple);
  void Insert(const std::vector<int64>& tuple);
  // Arity fixed version of Insert removing the need for a vector for the user.
  void Insert2(int64 v0, int64 v1);
  void Insert3(int64 v0, int64 v1, int64 v2);
  void Insert4(int64 v0, int64 v1, int64 v2, int64 v3);
  // Inserts the tuples.
  void InsertAll(const std::vector<std::vector<int64> >& tuples);
  void InsertAll(const std::vector<std::vector<int> >& tuples);

  // Checks if the tuple is in the set.
  bool Contains(const std::vector<int>& tuple);
  bool Contains(const std::vector<int64>& tuple);

  // Returns the number of tuples.
  int NumTuples() const;
  // Get the given tuple's value at the given position.
  int64 Value(int tuple_index, int pos_in_tuple) const;
  // Returns the arity of the set.
  int Arity() const;
  // Access the raw data, see IntTupleSet::Data::flat_tuples_.
  const int64* RawData() const;

 private:
  // Class that holds the actual data of an IntTupleSet. It handles
  // the reference counters, etc.
  class Data {
   public:
    explicit Data(int arity);
    Data(const Data& data);
    ~Data();
    void AddSharedOwner();
    bool RemovedSharedOwner();
    Data* CopyIfShared();
    template <class T> void Insert(const std::vector<T>& tuple);
    template <class T> bool Contains(const std::vector<T>& candidate);
    template <class T> int64 Fingerprint(const std::vector<T>& tuple);
    int NumTuples() const;
    int64 Value(int index, int pos) const;
    int Arity() const;
    const int64* RawData() const;
    void Clear();

   private:
    const int arity_;
    int num_owners_;
    // Concatenation of all tuples ever added.
    std::vector<int64> flat_tuples_;
    // Maps a tuple's fingerprint to the list of tuples with this
    // fingerprint, represented by their start index in the
    // flat_tuples_ vector.
    hash_map<int64, std::vector<int> > tuple_fprint_to_index_;
  };

  mutable Data* data_;
};

// ----- Data -----
inline IntTupleSet::Data::Data(int arity) : arity_(arity), num_owners_(0) {}

inline IntTupleSet::Data::Data(const Data& data)
    : arity_(data.arity_),
      num_owners_(0),
      flat_tuples_(data.flat_tuples_),
      tuple_fprint_to_index_(data.tuple_fprint_to_index_) {}

inline IntTupleSet::Data::~Data() {}

inline void IntTupleSet::Data::AddSharedOwner() {
  num_owners_++;
}

inline bool IntTupleSet::Data::RemovedSharedOwner() {
  return (--num_owners_ == 0);
}

inline IntTupleSet::Data* IntTupleSet::Data::CopyIfShared() {
  if (num_owners_ > 1) {  // Copy on write.
    Data* const new_data = new Data(*this);
    RemovedSharedOwner();
    new_data->AddSharedOwner();
    return new_data;
  }
  return this;
}

template <class T> void IntTupleSet::Data::Insert(const std::vector<T>& tuple) {
  DCHECK(arity_ == 0 || flat_tuples_.size() % arity_ == 0);
  CHECK_EQ(arity_, tuple.size());
  DCHECK_EQ(1, num_owners_);
  if (!Contains(tuple)) {
    const int index = NumTuples();
    flat_tuples_.reserve(flat_tuples_.size() + arity_);
    for (int i = 0; i < arity_; ++i) {
      flat_tuples_.push_back(tuple[i]);
    }
    const int64 fingerprint = Fingerprint(tuple);
    tuple_fprint_to_index_[fingerprint].push_back(index);
  }
}

template <class T> bool IntTupleSet::Data::Contains(
    const std::vector<T>& candidate) {
  if (candidate.size() != arity_) {
    return false;
  }
  const int64 fingerprint = Fingerprint(candidate);
  if (ContainsKey(tuple_fprint_to_index_, fingerprint)) {
    const std::vector<int>& indices = tuple_fprint_to_index_[fingerprint];
    for (int i = 0; i < indices.size(); ++i) {
      const int tuple_index = indices[i];
      for (int j = 0; j < arity_; ++j) {
        if (candidate[j] != flat_tuples_[tuple_index * arity_ + j]) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

template <class T> int64 IntTupleSet::Data::Fingerprint(
    const std::vector<T>& tuple) {
  switch (arity_) {
    case 0:
      return 0;
    case 1:
      return tuple[0];
    case 2: {
      uint64 x = tuple[0];
      uint64 y = GG_ULONGLONG(0xe08c1d668b756f82);
      uint64 z = tuple[1];
      mix(x, y, z);
      return z;
    }
    default: {
      uint64 x = tuple[0];
      uint64 y = GG_ULONGLONG(0xe08c1d668b756f82);
      for (int i = 1; i < tuple.size(); ++i) {
        uint64 z = tuple[i];
        mix(x, y, z);
        x = z;
      }
      return x;
    }
  }
}

inline int IntTupleSet::Data::NumTuples() const {
  return tuple_fprint_to_index_.size();
}

inline int64 IntTupleSet::Data::Value(int index, int pos) const {
  DCHECK_GE(index, 0);
  DCHECK_LT(index, flat_tuples_.size() / arity_);
  DCHECK_GE(pos, 0);
  DCHECK_LT(pos, arity_);
  return flat_tuples_[index * arity_ + pos];
}

inline int IntTupleSet::Data::Arity() const { return arity_; }

inline const int64* IntTupleSet::Data::RawData() const {
  return flat_tuples_.data();
}

inline void IntTupleSet::Data::Clear() {
  flat_tuples_.clear();
  tuple_fprint_to_index_.clear();
}

inline IntTupleSet::IntTupleSet(int arity) : data_(new Data(arity)) {
  CHECK_GE(arity, 0);
  data_->AddSharedOwner();
}

inline IntTupleSet::IntTupleSet(const IntTupleSet& set) : data_(set.data_) {
  data_->AddSharedOwner();
}

inline IntTupleSet::~IntTupleSet() {
  CHECK_NOTNULL(data_);
  if (data_->RemovedSharedOwner()) {
    delete data_;
  }
}

inline void IntTupleSet::Clear() {
  data_ = data_->CopyIfShared();
  data_->Clear();
}

inline void IntTupleSet::Insert(const std::vector<int>& tuple) {
  data_ = data_->CopyIfShared();
  data_->Insert(tuple);
}

inline void IntTupleSet::Insert2(int64 v0, int64 v1) {
  std::vector<int64> tuple(2);
  tuple[0] = v0;
  tuple[1] = v1;
  Insert(tuple);
}

inline void IntTupleSet::Insert3(int64 v0, int64 v1, int64 v2) {
  std::vector<int64> tuple(3);
  tuple[0] = v0;
  tuple[1] = v1;
  tuple[2] = v2;
  Insert(tuple);
}

inline void IntTupleSet::Insert4(int64 v0, int64 v1, int64 v2, int64 v3) {
  std::vector<int64> tuple(4);
  tuple[0] = v0;
  tuple[1] = v1;
  tuple[2] = v2;
  tuple[3] = v3;
  Insert(tuple);
}

inline bool IntTupleSet::Contains(const std::vector<int>& tuple) {
  return data_->Contains(tuple);
}

inline void IntTupleSet::Insert(const std::vector<int64>& tuple) {
  data_ = data_->CopyIfShared();
  data_->Insert(tuple);
}

inline void IntTupleSet::InsertAll(const std::vector<std::vector<int64> >& tuples) {
  data_ = data_->CopyIfShared();
  for (int i = 0; i < tuples.size(); ++i) {
    Insert(tuples[i]);
  }
}

inline void IntTupleSet::InsertAll(const std::vector<std::vector<int> >& tuples) {
  data_ = data_->CopyIfShared();
  for (int i = 0; i < tuples.size(); ++i) {
    Insert(tuples[i]);
  }
}

inline bool IntTupleSet::Contains(const std::vector<int64>& tuple) {
  return data_->Contains(tuple);
}

inline int IntTupleSet::NumTuples() const {
  return data_->NumTuples();
}

inline int64 IntTupleSet::Value(int index, int pos) const {
  return data_->Value(index, pos);
}

inline int IntTupleSet::Arity() const {
  return data_->Arity();
}

inline const int64* IntTupleSet::RawData() const {
  return data_->RawData();
}
}  // namespace operations_research

#endif  // OR_TOOLS_UTIL_TUPLE_SET_H_
