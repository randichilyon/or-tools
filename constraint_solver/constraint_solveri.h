// Copyright 2010-2011 Google
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

// Collection of objects used to extend the Constraint Solver library.
//
// This file contains a set of objects that simplifies writing extensions
// of the library.
//
// The main objects that define extensions are:
//   - BaseIntExpr the base class of all expressions that are not variables.
//   - SimpleRevFIFO a reversible FIFO list with templatized values.
//     A reversible data structure is a data structure that reverts its
//     modifications when the search is going up in the search tree, usually
//     after a failure occurs.
//   - RevImmutableMultiMap a reversible immutable multimap.
//   - MakeConstraintDemon<n> and MakeDelayedConstraintDemon<n> to wrap methods
//     of a constraint as a demon.
//   - RevSwitch, one reversible flip once switch.
//   - SmallRevBitSet, RevBitSet, and RevBitMatrix: reversible 1D or 2D
//     bitsets.
//   - LocalSearchOperator, IntVarLocalSearchOperator, ChangeValue and
//     PathOperator to create new local search operators.
//   - LocalSearchFilter and IntVarLocalSearchFilter to create new local
//     search filters.
//   - BaseLNS to write Large Neighbood Search operators.
//   - SymmetryBreaker to describe model symmetries that will be broken during
//     search using the 'Symmetry Breaking During Search' framework
//     see Gent, I. P., Harvey, W., & Kelsey, T. (2002).
//     Groups and Constraints: Symmetry Breaking During Search.
//     Principles and Practice of Constraint Programming CP2002
//     (Vol. 2470, pp. 415-430). Springer. Retrieved from
//     http://citeseerx.ist.psu.edu/viewdoc/summary?doi=10.1.1.11.1442
//
// Then, there are some internal classes that are used throughout the solver
// and exposed in this file:
//   - SearchLog the root class of all periodic outputs during search.
//   - ModelCache A caching layer to avoid creating twice the same object.
//   - DependencyGraph a dedicated data structure to represent dependency graphs
//     in the scheduling world.

#ifndef OR_TOOLS_CONSTRAINT_SOLVER_CONSTRAINT_SOLVERI_H_
#define OR_TOOLS_CONSTRAINT_SOLVER_CONSTRAINT_SOLVERI_H_

#include <math.h>
#include <stddef.h>
#include "base/hash.h"
#include <string>
#include <vector>

#include "base/callback-types.h"
#include "base/commandlineflags.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "base/sysinfo.h"
#include "base/timer.h"
#include "base/join.h"
#include "base/bitmap.h"
#include "base/map-util.h"
#include "base/hash.h"
#include "constraint_solver/constraint_solver.h"
#include "util/const_int_array.h"
#include "util/const_ptr_array.h"
#include "util/vector_map.h"

template <typename T> class ResultCallback;

class WallTimer;

namespace operations_research {
class CPArgumentProto;
class CPConstraintProto;
class CPIntegerExpressionProto;
class CPIntervalVariableProto;
class ConstIntArray;
template <class T> class ConstPtrArray;

// This is the base class for all expressions that are not variables.
// It proposes a basic 'CastToVar()' implementation.
// The class of expressions represent two types of objects: variables
// and subclasses of BaseIntExpr. Variables are stateful objects that
// provide a rich API (remove values, WhenBound...). On the other hand,
// subclasses of BaseIntExpr represent range-only stateless objects.
// That is the min(A + B) is recomputed each time as min(A) + min(B).
// Furthermore, sometimes, the propagation on an expression is not complete,
// and Min(), Max() are not monononic with respect to SetMin() and SetMax().
// For instance, A is a var with domain [0 .. 5], and B another variable
// with domain [0 .. 5]. Then Plus(A, B) has domain [0, 10].
// If we apply SetMax(Plus(A, B), 4)). Then we will deduce that both A
// and B will have domain [0 .. 4]. In that case, Max(Plus(A, B)) is 8
// and not 4.  To get back monotonicity, we will 'cast' the expression
// into a variable using the Var() method (that will call CastToVar()
// internally). The resulting variable will be stateful and monotonic.
//
// Finally, one should never store a pointer to a IntExpr, or
// BaseIntExpr in the code. The safe code should always call Var() on an
// expression built by the solver, and store the object as an IntVar*.
// This is a consequence of the stateless nature of the expressions that
// makes the code error-prone.
class BaseIntExpr : public IntExpr {
 public:
  explicit BaseIntExpr(Solver* const s) : IntExpr(s), var_(NULL) {}
  virtual ~BaseIntExpr() {}

  virtual IntVar* Var();
  virtual IntVar* CastToVar();

 private:
  IntVar* var_;
};

// This enum is used internally to do dynamic typing on subclasses of integer
// variables.
enum VarTypes {
  UNSPECIFIED,
  DOMAIN_INT_VAR,
  BOOLEAN_VAR,
  CONST_VAR,
  VAR_ADD_CST,
  DOMAIN_INT_VAR_ADD_CST,
  VAR_TIMES_POS_CST,
  BOOLEAN_VAR_TIMES_POS_CST,
  CST_SUB_VAR,
  OPP_VAR,
  TRACE_VAR
};

// ----- utility classes -----

// This class represent a reversible FIFO structure.
// The main diffence w.r.t a standart FIFO structure is that a Solver is
// given as parameter to the modifiers such that the solver can store the
// backtrack information
// Iterator's traversing order should not be changed, as some algorithm
// depend on it to be consistent.
// It's main use is to store a list of demons in the various classes of
// variables.
#ifndef SWIG
template <class T> class SimpleRevFIFO {
 private:
  enum { CHUNK_SIZE = 16 };  // TODO(user): could be an extra template param
  struct Chunk {
    T data_[CHUNK_SIZE];
    const Chunk* const next_;
    explicit Chunk(const Chunk* next) : next_(next) {}
  };

 public:
  // This iterator is not stable with respect to deletion.
  class Iterator {
   public:
    explicit Iterator(const SimpleRevFIFO<T>* l)
        : chunk_(l->chunks_), value_(l->Last()) {}
    bool ok() const { return (value_ != NULL); }
    T operator*() const { return *value_; }
    void operator++() {
      ++value_;
      if (value_ == chunk_->data_ + CHUNK_SIZE) {
        chunk_ = chunk_->next_;
        value_ = chunk_ ? chunk_->data_ : NULL;
      }
    }

   private:
    const Chunk* chunk_;
    const T* value_;
  };

  SimpleRevFIFO() : chunks_(NULL), pos_(0) {}

  void Push(Solver* const s, T val) {
    if (pos_.Value() == 0) {
      Chunk* const chunk = s->UnsafeRevAlloc(new Chunk(chunks_));
      s->SaveAndSetValue(reinterpret_cast<void**>(&chunks_),
                         reinterpret_cast<void*>(chunk));
      pos_.SetValue(s, CHUNK_SIZE - 1);
    } else {
      pos_.Decr(s);
    }
    chunks_->data_[pos_.Value()] = val;
  }

  // Pushes the var on top if is not a duplicate of the current top object.
  void PushIfNotTop(Solver* const s, T val) {
    if (chunks_ == NULL || LastValue() != val) {
      Push(s, val);
    }
  }

  // Returns the last item of the FIFO.
  const T* Last() const {
    return chunks_ ? &chunks_->data_[pos_.Value()] : NULL;
  }

  // Returns the last value in the FIFO.
  const T& LastValue() const {
    DCHECK(chunks_);
    return chunks_->data_[pos_.Value()];
  }

  // Sets the last value in the FIFO.
  void SetLastValue(const T& v) {
    DCHECK(Last());
    chunks_->data_[pos_.Value()] = v;
  }

 private:
  Chunk *chunks_;
  NumericalRev<int> pos_;
};

// ---------- Reversible Hash Table ----------

// ----- Hash functions -----
// TODO(user): use murmurhash.
inline uint64 Hash1(uint64 value) {
  value = (~value) + (value << 21);  // value = (value << 21) - value - 1;
  value ^= value >> 24;
  value += (value << 3) + (value << 8);  // value * 265
  value ^= value >> 14;
  value += (value << 2) + (value << 4);  // value * 21
  value ^= value >> 28;
  value += (value << 31);
  return value;
}

inline uint64 Hash1(uint32 value) {
  uint64 a = value;
  a = (a + 0x7ed55d16) + (a << 12);
  a = (a ^ 0xc761c23c) ^ (a >> 19);
  a = (a + 0x165667b1) + (a << 5);
  a = (a + 0xd3a2646c) ^ (a << 9);
  a = (a + 0xfd7046c5) + (a << 3);
  a = (a ^ 0xb55a4f09) ^ (a >> 16);
  return a;
}

inline uint64 Hash1(int64 value) {
  return Hash1(static_cast<uint64>(value));
}

inline uint64 Hash1(int value) {
  return Hash1(static_cast<uint32>(value));
}

inline uint64 Hash1(void* const ptr) {
#if defined(ARCH_K8)
    return Hash1(reinterpret_cast<uint64>(ptr));
#else
    return Hash1(reinterpret_cast<uint32>(ptr));
#endif
}

inline uint64 Hash1(ConstIntArray* const values) {
  if (values->size() == 0) {
    return 0;
  } else if (values->size() == 1) {
    return Hash1(values->get(0));
  } else {
    uint64 hash = Hash1(values->get(0));
    for (int i = 1; i < values->size(); ++i) {
      hash = hash * i + Hash1(values->get(i));
    }
    return hash;
  }
}

template <class T> uint64 Hash1(ConstPtrArray<T>* const ptrs) {
  if (ptrs->size() == 0) {
    return 0;
  } else if (ptrs->size() == 1) {
    return Hash1(ptrs->get(0));
  } else {
    uint64 hash = Hash1(ptrs->get(0));
    for (int i = 1; i < ptrs->size(); ++i) {
      hash = hash * i + Hash1(ptrs->get(i));
    }
    return hash;
  }
}

// ----- Immutable Multi Map -----

// Reversible Immutable MultiMap class.
// Represents an immutable multi-map that backstracks with the solver.
template <class K, class V> class RevImmutableMultiMap {
 public:
  RevImmutableMultiMap(Solver* const solver, int initial_size)
      : solver_(solver),
        array_(solver->UnsafeRevAllocArray(new Cell*[initial_size])),
        size_(initial_size),
        num_items_(0) {
    memset(array_, 0, sizeof(*array_) * size_.Value());
  }

  ~RevImmutableMultiMap() {}

  int num_items() const { return num_items_.Value(); }

  // Returns true if the multi-map contains at least one instance of 'key'.
  bool ContainsKey(const K& key) const {
    uint64 code = Hash1(key) % size_.Value();
    Cell* tmp = array_[code];
    while (tmp) {
      if (tmp->key() == key) {
        return true;
      }
      tmp = tmp->next();
    }
    return false;
  }

  // Returns one value attached to 'key', or 'defaut_value' if 'key'
  // is not in the multi-map. The actual value returned if more than one
  // values is attached to the same key is not specified.
  const V& FindWithDefault(const K& key, const V& default_value) const {
    uint64 code = Hash1(key) % size_.Value();
    Cell* tmp = array_[code];
    while (tmp) {
      if (tmp->key() == key) {
        return tmp->value();
      }
      tmp = tmp->next();
    }
    return default_value;
  }

  // Inserts (key, value) in the multi-map.
  void Insert(const K& key, const V& value) {
    const int position = Hash1(key) % size_.Value();
    Cell* const cell =
        solver_->UnsafeRevAlloc(new Cell(key, value, array_[position]));
    solver_->SaveAndSetValue(reinterpret_cast<void**>(&array_[position]),
                             reinterpret_cast<void*>(cell));
    num_items_.Incr(solver_);
    if (num_items_.Value() > 2 * size_.Value()) {
      Double();
    }
  }

 private:
  class Cell {
   public:
    Cell(const K& key, const V& value, Cell* const next)
        : key_(key), value_(value), next_(next) {}

    void SetRevNext(Solver* const solver, Cell* const next) {
      solver->SaveAndSetValue(reinterpret_cast<void**>(&next_),
                              reinterpret_cast<void*>(next));
    }

    Cell* next() const { return next_; }

    const K& key() const { return key_; }

    const V& value() const { return value_; }

   private:
    const K key_;
    const V value_;
    Cell* next_;
  };

  void Double() {
    Cell** const old_cell_array = array_;
    const int old_size = size_.Value();
    size_.SetValue(solver_, size_.Value() * 2);
    solver_->SaveAndSetValue(
        reinterpret_cast<void**>(&array_),
        reinterpret_cast<void*>(
            solver_->UnsafeRevAllocArray(new Cell*[size_.Value()])));
    memset(array_, 0, size_.Value() * sizeof(*array_));
    for (int i = 0; i < old_size; ++i) {
      Cell* tmp = old_cell_array[i];
      while (tmp != NULL) {
        Cell* const to_reinsert = tmp;
        tmp = tmp->next();
        const uint64 new_position = Hash1(to_reinsert->key()) % size_.Value();
        to_reinsert->SetRevNext(solver_, array_[new_position]);
        solver_->SaveAndSetValue(
            reinterpret_cast<void**>(&array_[new_position]),
            reinterpret_cast<void*>(to_reinsert));
      }
    }
  }

  Solver* const solver_;
  Cell** array_;
  NumericalRev<int> size_;
  NumericalRev<int> num_items_;
};

// A reversible switch that can switch once from false to true.
class RevSwitch {
 public:
  RevSwitch() : value_(false) {}

  bool Switched() const { return value_; }

  void Switch(Solver* const solver) {
    solver->SaveAndSetValue(&value_, true);
  }

 private:
  bool value_;
};

// This class represents a small reversible bitset (size <= 64).
// This class is useful to maintain supports.
class SmallRevBitSet {
 public:
  explicit SmallRevBitSet(int64 size);
  // Sets the 'pos' bit.
  void SetToOne(Solver* const solver, int64 pos);
  // Erases the 'pos' bit.
  void SetToZero(Solver* const solver, int64 pos);
  // Returns the number of bits set to one.
  int64 Cardinality() const;
  // Is bitset null?
  bool IsCardinalityZero() const { return bits_.Value() == GG_ULONGLONG(0); }
  // Does it contains only one bit set?
  bool IsCardinalityOne() const {
    return (bits_.Value() != 0) && !(bits_.Value() & (bits_.Value() - 1));
  }
  // Gets the index of the first bit set starting from 0.
  // It returns -1 if the bitset is empty.
  int64 GetFirstOne() const;

 private:
  Rev<uint64> bits_;
};

// This class represents a reversible bitset.
// This class is useful to maintain supports.
class RevBitSet {
 public:
  explicit RevBitSet(int64 size);
  ~RevBitSet();

  // Sets the 'pos' bit.
  void SetToOne(Solver* const solver, int64 pos);
  // Erases the 'pos' bit.
  void SetToZero(Solver* const solver, int64 pos);
  // Returns whether the 'pos' bit is set.
  bool IsSet(int64 pos) const;
  // Returns the number of bits set to one.
  int64 Cardinality() const;
  // Is bitset null?
  bool IsCardinalityZero() const;
  // Does it contains only one bit set?
  bool IsCardinalityOne() const;
  // Gets the index of the first bit set starting from start.
  // It returns -1 if the bitset is empty after start.
  int64 GetFirstBit(int start) const;
  // Cleans all bits.
  void ClearAll(Solver* const solver);

  friend class RevBitMatrix;

 private:
  // Save the offset's part of the bitset.
  void Save(Solver* const solver, int offset);
  const int64 size_;
  const int64 length_;
  uint64* bits_;
  uint64* stamps_;
};

// Matrix version of the RevBitSet class.
class RevBitMatrix : private RevBitSet {
 public:
  RevBitMatrix(int64 rows, int64 columns);
  ~RevBitMatrix();

  // Sets the 'column' bit in the 'row' row..
  void SetToOne(Solver* const solver, int64 row, int64 column);
  // Erases the 'column' bit in the 'row' row..
  void SetToZero(Solver* const solver, int64 row, int64 column);
  // Returns whether the 'column' bit in the 'row' row is set.
  bool IsSet(int64 row, int64 column) const {
    DCHECK_GE(row, 0);
    DCHECK_LT(row, rows_);
    DCHECK_GE(column, 0);
    DCHECK_LT(column, columns_);
    return RevBitSet::IsSet(row * columns_ + column);
  }
  // Returns the number of bits set to one in the 'row' row.
  int64 Cardinality(int row) const;
  // Is bitset of row 'row' null?
  bool IsCardinalityZero(int row) const;
  // Does the 'row' bitset contains only one bit set?
  bool IsCardinalityOne(int row) const;
  // Returns the first bit in the row 'row' which position is >= 'start'.
  // It returns -1 if there are none.
  int64 GetFirstBit(int row, int start) const;
  // Cleans all bits.
  void ClearAll(Solver* const solver);

 private:
  const int64 rows_;
  const int64 columns_;
};

// @{
// These methods represent generic demons that will call back a
// method on the constraint during their Run method.
// This way, all propagation methods are members of the constraint class,
// and demons are just proxies with a priority of NORMAL_PRIORITY.

// Demon proxy to a method on the constraint with no arguments.
template <class T> class CallMethod0 : public Demon {
 public:
  CallMethod0(T* const ct, void (T::*method)(), const string& name)
      : constraint_(ct), method_(method), name_(name) {}

  virtual ~CallMethod0() {}

  virtual void Run(Solver* const s) {
    (constraint_->*method_)();
  }

  virtual string DebugString() const {
    return "CallMethod_" + name_ + "(" + constraint_->DebugString() + ")";
  }

 private:
  T* const constraint_;
  void (T::* const method_)();
  const string name_;
};

template <class T> Demon* MakeConstraintDemon0(Solver* const s,
                                               T* const ct,
                                               void (T::*method)(),
                                               const string& name) {
  return s->RevAlloc(new CallMethod0<T>(ct, method, name));
}

// Demon proxy to a method on the constraint with one argument.
template <class T, class P> class CallMethod1 : public Demon {
 public:
  CallMethod1(T* const ct,
              void (T::*method)(P),
              const string& name,
              P param1)
      : constraint_(ct),
        method_(method),
        name_(name),
        param1_(param1) {}

  virtual ~CallMethod1() {}

  virtual void Run(Solver* const s) {
    (constraint_->*method_)(param1_);
  }

  virtual string DebugString() const {
    return StrCat(StrCat("CallMethod_", name_),
                  StrCat("(", constraint_->DebugString(), ", "),
                  StrCat(param1_, ")"));
  }

 private:
  T* const constraint_;
  void (T::* const method_)(P);
  const string name_;
  P param1_;
};

template <class T, class P>
Demon* MakeConstraintDemon1(Solver* const s,
                            T* const ct,
                            void (T::*method)(P),
                            const string& name,
                            P param1) {
  return s->RevAlloc(new CallMethod1<T, P>(ct, method, name, param1));
}

// Demon proxy to a method on the constraint with two arguments.
template <class T, class P, class Q> class CallMethod2 : public Demon {
 public:
  CallMethod2(T* const ct,
              void (T::*method)(P, Q),
              const string& name,
              P param1,
              Q param2)
      : constraint_(ct),
        method_(method),
        name_(name),
        param1_(param1),
        param2_(param2) {}

  virtual ~CallMethod2() {}

  virtual void Run(Solver* const s) {
    (constraint_->*method_)(param1_, param2_);
  }

  virtual string DebugString() const {
    return StrCat(StrCat("CallMethod_", name_),
                  StrCat("(", constraint_->DebugString()),
                  StrCat(", ", param1_),
                  StrCat(", ", param2_, ")"));
  }

 private:
  T* const constraint_;
  void (T::* const method_)(P, Q);
  const string name_;
  P param1_;
  Q param2_;
};

template <class T, class P, class Q>
Demon* MakeConstraintDemon2(Solver* const s,
                            T* const ct,
                            void (T::*method)(P, Q),
                            const string& name,
                            P param1,
                            Q param2) {
  return s->RevAlloc(new CallMethod2<T, P, Q>(ct,
                                              method,
                                              name,
                                              param1,
                                              param2));
}
// @}

// @{
// These methods represents generic demons that will call back a
// method on the constraint during their Run method. This demon will
// have a priority DELAYED_PRIORITY.

// Low-priority demon proxy to a method on the constraint with no arguments.
template <class T> class DelayedCallMethod0 : public Demon {
 public:
  DelayedCallMethod0(T* const ct, void (T::*method)(), const string& name)
      : constraint_(ct), method_(method), name_(name) {}

  virtual ~DelayedCallMethod0() {}

  virtual void Run(Solver* const s) {
    (constraint_->*method_)();
  }

  virtual Solver::DemonPriority priority() const {
    return Solver::DELAYED_PRIORITY;
  }

  virtual string DebugString() const {
    return "DelayedCallMethod_"
        + name_ + "(" + constraint_->DebugString() + ")";
  }

 private:
  T* const constraint_;
  void (T::* const method_)();
  const string name_;
};

template <class T> Demon* MakeDelayedConstraintDemon0(Solver* const s,
                                                      T* const ct,
                                                      void (T::*method)(),
                                                      const string& name) {
  return s->RevAlloc(new DelayedCallMethod0<T>(ct, method, name));
}

// Low-priority demon proxy to a method on the constraint with one argument.
template <class T, class P> class DelayedCallMethod1 : public Demon {
 public:
  DelayedCallMethod1(T* const ct,
                     void (T::*method)(P),
                     const string& name,
                     P param1)
      : constraint_(ct),
        method_(method),
        name_(name),
        param1_(param1) {}

  virtual ~DelayedCallMethod1() {}

  virtual void Run(Solver* const s) {
    (constraint_->*method_)(param1_);
  }

  virtual Solver::DemonPriority priority() const {
    return Solver::DELAYED_PRIORITY;
  }

  virtual string DebugString() const {
    return StrCat(StrCat("DelayedCallMethod_", name_),
                  StrCat("(", constraint_->DebugString(), ", "),
                  StrCat(param1_, ")"));
  }

 private:
  T* const constraint_;
  void (T::* const method_)(P);
  const string name_;
  P param1_;
};

template <class T, class P>
Demon* MakeDelayedConstraintDemon1(Solver* const s,
                                   T* const ct,
                                   void (T::*method)(P),
                                   const string& name,
                                   P param1) {
  return s->RevAlloc(new DelayedCallMethod1<T, P>(ct, method, name, param1));
}

// Low-priority demon proxy to a method on the constraint with two arguments.
template <class T, class P, class Q> class DelayedCallMethod2 : public Demon {
 public:
  DelayedCallMethod2(T* const ct,
                     void (T::*method)(P, Q),
                     const string& name,
                     P param1,
                     Q param2)
      : constraint_(ct),
        method_(method),
        name_(name),
        param1_(param1),
        param2_(param2) {}

  virtual ~DelayedCallMethod2() {}

  virtual void Run(Solver* const s) {
    (constraint_->*method_)(param1_, param2_);
  }

  virtual Solver::DemonPriority priority() const {
    return Solver::DELAYED_PRIORITY;
  }

  virtual string DebugString() const {
    return StrCat(StrCat("DelayedCallMethod_", name_),
                  StrCat("(", constraint_->DebugString()),
                  StrCat(", ", param1_),
                  StrCat(", ", param2_, ")"));
  }

 private:
  T* const constraint_;
  void (T::* const method_)(P, Q);
  const string name_;
  P param1_;
  Q param2_;
};

template <class T, class P, class Q>
Demon* MakeDelayedConstraintDemon2(Solver* const s,
                                   T* const ct,
                                   void (T::*method)(P, Q),
                                   const string& name,
                                   P param1,
                                   Q param2) {
  return s->RevAlloc(new DelayedCallMethod2<T, P, Q>(ct,
                                                     method,
                                                     name,
                                                     param1,
                                                     param2));
}
// @}

#endif   // !defined(SWIG)

// ---------- Local search operators ----------

// The base class for all local search operators.
// A local search operator is an object which defines the neighborhood of a
// solution; in other words, a neighborhood is the set of solutions which can
// be reached from a given solution using an operator.
// The behavior of the LocalSearchOperator class is similar to the one of an
// iterator. The operator is synchronized with an assignment (gives the
// current values of the variables); this is done in the Start() method.
// Then one can iterate over the neighbors using the MakeNextNeighbor method.
// This method returns an assignment which represents the incremental changes
// to the curent solution. It also returns a second assignment representing the
// changes to the last solution defined by the neighborhood operator; this
// assignment is empty is the neighborhood operator cannot track this
// information.
// TODO(user): rename Start to Synchronize ?
// TODO(user): decouple the iterating from the defining of a neighbor.
class LocalSearchOperator : public BaseObject {
 public:
  LocalSearchOperator() {}
  virtual ~LocalSearchOperator() {}
  virtual bool MakeNextNeighbor(Assignment* delta, Assignment* deltadelta) = 0;
  virtual void Start(const Assignment* assignment) = 0;
};

// ----- Base operator class for operators manipulating IntVars -----

// Specialization of LocalSearchOperator built from an array of IntVars
// which specifies the scope of the operator.
// This class also takes care of storing current variable values in Start(),
// keeps track of changes done by the operator and builds the delta.
// The Deactivate() method can be used to perform Large Neighborhood Search.
class IntVarLocalSearchOperator : public LocalSearchOperator {
 public:
  IntVarLocalSearchOperator();
  IntVarLocalSearchOperator(const IntVar* const* vars, int size);
  virtual ~IntVarLocalSearchOperator();
  // This method should not be overridden. Override OnStart() instead which is
  // called before exiting this method.
  virtual void Start(const Assignment* assignment);
  virtual bool IsIncremental() const { return false; }
  int Size() const { return size_; }
  // Returns the value in the current assignment of the variable of given index.
  int64 Value(int64 index) const {
    DCHECK_LT(index, size_);
    return values_[index];
  }
  // Returns the variable of given index.
  IntVar* Var(int64 index) const { return vars_[index]; }
  virtual bool SkipUnchanged(int index) const { return false; }

 protected:
  int64 OldValue(int64 index) const { return old_values_[index]; }
  void SetValue(int64 index, int64 value);
  bool Activated(int64 index) const;
  void Activate(int64 index);
  void Deactivate(int64 index);
  bool ApplyChanges(Assignment* delta, Assignment* deltadelta) const;
  void RevertChanges(bool incremental);
  void AddVars(const IntVar* const* vars, int size);

 private:
  // Called by Start() after synchronizing the operator with the current
  // assignment. Should be overridden instead of Start() to avoid calling
  // IntVarLocalSearchOperator::Start explicitly.
  virtual void OnStart() {}
  void MarkChange(int64 index);

  scoped_array<IntVar*> vars_;
  int size_;
  scoped_array<int64> values_;
  scoped_array<int64> old_values_;
  Bitmap activated_;
  Bitmap was_activated_;
  std::vector<int64> changes_;
  Bitmap has_changed_;
  Bitmap has_delta_changed_;
  bool cleared_;
};

// ----- SequenceVarLocalSearchOperator -----

// TODO(user): Merge with IntVarLocalSearchOperator.
class SequenceVarLocalSearchOperator : public LocalSearchOperator {
 public:
  SequenceVarLocalSearchOperator();
  SequenceVarLocalSearchOperator(const SequenceVar* const* vars, int size);
  virtual ~SequenceVarLocalSearchOperator();
  // This method should not be overridden. Override OnStart() instead which is
  // called before exiting this method.
  virtual void Start(const Assignment* assignment);
  virtual bool IsIncremental() const { return false; }
  int Size() const { return size_; }
  // Returns the value in the current assignment of the variable of given index.
#if !defined(SWIG)
  const std::vector<int>& Sequence(int64 index) const {
    DCHECK_LT(index, size_);
    return values_[index];
  }
#endif
  // Returns the variable of given index.
  SequenceVar* Var(int64 index) const { return vars_[index]; }
  virtual bool SkipUnchanged(int index) const { return false; }

 protected:
  const std::vector<int>& OldSequence(int64 index) const {
    return old_values_[index];
  }
  void SetForwardSequence(int64 index, const std::vector<int>& value);
  void SetBackwardSequence(int64 index, const std::vector<int>& value);
  bool Activated(int64 index) const;
  void Activate(int64 index);
  void Deactivate(int64 index);
  bool ApplyChanges(Assignment* delta, Assignment* deltadelta) const;
  void RevertChanges(bool incremental);
  void AddVars(const SequenceVar* const* vars, int size);

 private:
  // Called by Start() after synchronizing the operator with the current
  // assignment. Should be overridden instead of Start() to avoid calling
  // SequenceVarLocalSearchOperator::Start explicitly.
  virtual void OnStart() {}
  void MarkChange(int64 index);

  scoped_array<SequenceVar*> vars_;
  int size_;
  scoped_array<std::vector<int> > values_;
  scoped_array<std::vector<int> > backward_values_;
  scoped_array<std::vector<int> > old_values_;
  Bitmap activated_;
  Bitmap was_activated_;
  std::vector<int64> changes_;
  Bitmap has_changed_;
  Bitmap has_delta_changed_;
  bool cleared_;
};

// ----- Base Large Neighborhood Search operator class ----

// This is the base class for building an LNS operator. An LNS fragment is a
// collection of variables which will be relaxed. Fragments are built with
// NextFragment(), which returns false if there are no more fragments to build.
// Optionally one can override InitFragments, which is called from
// LocalSearchOperator::Start to initialize fragment data.
//
// Here's a sample relaxing one variable at a time:
//
// class OneVarLNS : public BaseLNS {
//  public:
//   OneVarLNS(const IntVar* const* vars, int size)
//       : BaseLNS(vars, size), index_(0) {}
//   virtual ~OneVarLNS() {}
//   virtual void InitFragments() { index_ = 0; }
//   virtual bool NextFragment(std::vector<int>* fragment) {
//     const int size = Size();
//     if (index_ < size) {
//       fragment->push_back(index_);
//       ++index_;
//       return true;
//     } else {
//       return false;
//     }
//   }
//
//  private:
//   int index_;
// };
class BaseLNS : public IntVarLocalSearchOperator {
 public:
  BaseLNS(const IntVar* const* vars, int size);
  virtual ~BaseLNS();
  // This method should not be overridden (it calls NextFragment()).
  virtual bool MakeNextNeighbor(Assignment* delta, Assignment* deltadelta);
  virtual void InitFragments();
  virtual bool NextFragment(std::vector<int>* fragment) = 0;

 private:
  // This method should not be overridden. Override InitFragments() instead.
  virtual void OnStart();
};

// ----- ChangeValue Operators -----

// Defines operators which change the value of variables;
// each neighbor corresponds to *one* modified variable.
// Sub-classes have to define ModifyValue which determines what the new
// variable value is going to be (given the current value and the variable).
class ChangeValue : public IntVarLocalSearchOperator {
 public:
  ChangeValue(const IntVar* const* vars, int size);
  virtual ~ChangeValue();
  virtual bool MakeNextNeighbor(Assignment* delta, Assignment* deltadelta);
  virtual int64 ModifyValue(int64 index, int64 value) = 0;

 private:
  virtual void OnStart();

  int index_;
};

// ----- Path-based Operators -----

// Base class of the local search operators dedicated to path modifications
// (a path is a set of nodes linked together by arcs).
// This family of neighborhoods supposes they are handling next variables
// representing the arcs (var[i] represents the node immediately after i on
// a path).
// Several services are provided:
// - arc manipulators (SetNext(), ReverseChain(), MoveChain())
// - path inspectors (Next(), IsPathEnd())
// - path iterators: operators need a given number of nodes to define a
//   neighbor; this class provides the iteration on a given number of (base)
//   nodes which can be used to define a neighbor (through the BaseNode method)
// Subclasses only need to override MakeNeighbor to create neighbors using
// the services above (no direct manipulation of assignments).
class PathOperator : public IntVarLocalSearchOperator {
 public:
  PathOperator(const IntVar* const* next_vars,
               const IntVar* const* path_vars,
               int size,
               int number_of_base_nodes);
  virtual ~PathOperator() {}
  virtual bool MakeNeighbor() = 0;
  virtual bool MakeNextNeighbor(Assignment* delta, Assignment* deltadelta);

  // TODO(user): Make the following methods protected.
  virtual bool SkipUnchanged(int index) const;

  // Returns the index of the node after the node of index node_index in the
  // current assignment.
  int64 Next(int64 node_index) const {
    DCHECK(!IsPathEnd(node_index));
    return Value(node_index);
  }

  // Returns the index of the path to which the node of index node_index
  // belongs in the current assignment.
  int64 Path(int64 node_index) const {
    return ignore_path_vars_ ? 0LL : Value(node_index + number_of_nexts_);
  }

  // Number of next variables.
  int number_of_nexts() const { return number_of_nexts_; }

 protected:
  // Returns the index of the variable corresponding to the ith base node.
  int64 BaseNode(int i) const { return base_nodes_[i]; }
  int64 StartNode(int i) const { return path_starts_[base_paths_[i]]; }

  int64 OldNext(int64 node_index) const {
    DCHECK(!IsPathEnd(node_index));
    return OldValue(node_index);
  }

  int64 OldPath(int64 node_index) const {
    return ignore_path_vars_ ? 0LL : OldValue(node_index + number_of_nexts_);
  }

  // Moves the chain starting after the node before_chain and ending at the node
  // chain_end after the node destination
  bool MoveChain(int64 before_chain, int64 chain_end, int64 destination);

  // Reverses the chain starting after before_chain and ending before
  // after_chain
  bool ReverseChain(int64 before_chain, int64 after_chain, int64* chain_last);

  bool MakeActive(int64 node, int64 destination);
  bool MakeChainInactive(int64 before_chain, int64 chain_end);

  // Sets the to to be the node after from
  void SetNext(int64 from, int64 to, int64 path) {
    DCHECK_LT(from, number_of_nexts_);
    SetValue(from, to);
    if (!ignore_path_vars_) {
      DCHECK_LT(from + number_of_nexts_, Size());
      SetValue(from + number_of_nexts_, path);
    }
  }

  // Returns true if i is the last node on the path; defined by the fact that
  // i outside the range of the variable array
  bool IsPathEnd(int64 i) const { return i >= number_of_nexts_; }

  // Returns true if node is inactive
  bool IsInactive(int64 i) const { return !IsPathEnd(i) && inactives_[i]; }

  // Returns true if operator needs to restart its initial position at each
  // call to Start()
  virtual bool InitPosition() const { return false; }
  // Reset the position of the operator to its position when Start() was last
  // called; this can be used to let an operator iterate more than once over
  // the paths.
  void ResetPosition() {
    just_started_ = true;
  }

  const int number_of_nexts_;
  const bool ignore_path_vars_;

 private:
  virtual void OnStart();
  // Called by OnStart() after initializing node information. Should be
  // overriden instead of OnStart() to avoid calling PathOperator::OnStart
  // explicitly.
  virtual void OnNodeInitialization() {}

  bool CheckEnds() const;
  bool IncrementPosition();
  void InitializePathStarts();
  void InitializeInactives();
  void InitializeBaseNodes();
  bool CheckChainValidity(int64 chain_start,
                          int64 chain_end,
                          int64 exclude) const;
  void Synchronize();

  std::vector<int> base_nodes_;
  std::vector<int> end_nodes_;
  std::vector<int> base_paths_;
  std::vector<int64> path_starts_;
  std::vector<bool> inactives_;
  bool just_started_;
  bool first_start_;
};

// ----- Local Search Filters ------

// For fast neighbor pruning
class LocalSearchFilter : public BaseObject {
 public:
  // Accepts a "delta" given the assignment with which the filter has been
  // synchronized; the delta holds the variables which have been modified and
  // their new value.
  // Sample: supposing one wants to maintain a[0,1] + b[0,1] <= 1,
  // for the assignment (a,1), (b,0), the delta (b,1) will be rejected
  // but the delta (a,0) will be accepted.
  virtual bool Accept(const Assignment* delta,
                      const Assignment* deltadelta) = 0;

  // Synchronizes the filter with the current solution
  virtual void Synchronize(const Assignment* assignment) = 0;
  virtual bool IsIncremental() const { return false; }
};

// ----- IntVarLocalSearchFilter -----

class IntVarLocalSearchFilter : public LocalSearchFilter {
 public:
  IntVarLocalSearchFilter(const IntVar* const* vars, int size);
  ~IntVarLocalSearchFilter();
  // This method should not be overridden. Override OnSynchronize() instead
  // which is called before exiting this method.
  virtual void Synchronize(const Assignment* assignment);

 protected:
  // Add variables to "track" to the filter.
  void AddVars(const IntVar* const* vars, int size);
  bool FindIndex(const IntVar* const var, int64* index) const {
    DCHECK(index != NULL);
    return FindCopy(var_to_index_, var, index);
  }
  int Size() const { return size_; }
  IntVar* Var(int index) const { return vars_[index]; }
  int64 Value(int index) const { return values_[index]; }

 private:
  virtual void OnSynchronize() {}

  scoped_array<IntVar*> vars_;
  scoped_array<int64> values_;
  int size_;
  hash_map<const IntVar*, int64> var_to_index_;
};

// ---------- PropagationMonitor ----------

class PropagationMonitor : public SearchMonitor {
 public:
  explicit PropagationMonitor(Solver* const solver);
  virtual ~PropagationMonitor();

  // Propagation events.
  virtual void BeginConstraintInitialPropagation(
      const Constraint* const constraint) = 0;
  virtual void EndConstraintInitialPropagation(
      const Constraint* const constraint) = 0;
  virtual void BeginNestedConstraintInitialPropagation(
      const Constraint* const parent,
      const Constraint* const nested) = 0;
  virtual void EndNestedConstraintInitialPropagation(
      const Constraint* const parent,
      const Constraint* const nested) = 0;
  virtual void RegisterDemon(const Demon* const demon) = 0;
  virtual void BeginDemonRun(const Demon* const demon) = 0;
  virtual void EndDemonRun(const Demon* const demon) = 0;
  virtual void PushContext(const string& context) = 0;
  virtual void PopContext() = 0;
  // IntExpr modifiers.
  virtual void SetMin(IntExpr* const expr, int64 new_min) = 0;
  virtual void SetMax(IntExpr* const expr, int64 new_max) = 0;
  virtual void SetRange(IntExpr* const expr, int64 new_min, int64 new_max) = 0;
  // IntVar modifiers.
  virtual void SetMin(IntVar* const var, int64 new_min) = 0;
  virtual void SetMax(IntVar* const var, int64 new_max) = 0;
  virtual void SetRange(IntVar* const var, int64 new_min, int64 new_max) = 0;
  virtual void RemoveValue(IntVar* const var, int64 value) = 0;
  virtual void SetValue(IntVar* const var, int64 value) = 0;
  virtual void RemoveInterval(IntVar* const var, int64 imin, int64 imax) = 0;
  virtual void SetValues(IntVar* const var,
                         const int64* const values,
                         int size) = 0;
  virtual void RemoveValues(IntVar* const var,
                            const int64* const values,
                            int size) = 0;
  // IntervalVar modifiers.
  virtual void SetStartMin(IntervalVar* const var, int64 new_min) = 0;
  virtual void SetStartMax(IntervalVar* const var, int64 new_max) = 0;
  virtual void SetStartRange(IntervalVar* const var,
                             int64 new_min,
                             int64 new_max) = 0;
  virtual void SetEndMin(IntervalVar* const var, int64 new_min) = 0;
  virtual void SetEndMax(IntervalVar* const var, int64 new_max) = 0;
  virtual void SetEndRange(IntervalVar* const var,
                           int64 new_min,
                           int64 new_max) = 0;
  virtual void SetDurationMin(IntervalVar* const var, int64 new_min) = 0;
  virtual void SetDurationMax(IntervalVar* const var, int64 new_max) = 0;
  virtual void SetDurationRange(IntervalVar* const var,
                                int64 new_min,
                                int64 new_max) = 0;
  virtual void SetPerformed(IntervalVar* const var, bool value) = 0;
  // SequenceVar modifiers
  virtual void RankFirst(SequenceVar* const var, int index) = 0;
  virtual void RankNotFirst(SequenceVar* const var, int index) = 0;
  virtual void RankLast(SequenceVar* const var, int index) = 0;
  virtual void RankNotLast(SequenceVar* const var, int index) = 0;
  virtual void RankSequence(SequenceVar* const var,
                            const std::vector<int>& rank_first,
                            const std::vector<int>& rank_last,
                            const std::vector<int>& unperformed) = 0;
  // Install itself on the solver.
  virtual void Install();
};

// ---------- SymmetryBreaker ----------

class SymmetryManager;

// A symmetry breaker is an object that will visit a decision and
// create the 'symmetrical' decision in return.
// Each symmetry breaker represents one class of symmetry.
class SymmetryBreaker : public DecisionVisitor {
 public:
  SymmetryBreaker() : symmetry_manager_(NULL), index_in_symmetry_manager_(-1) {}
  virtual ~SymmetryBreaker() {}

  void AddIntegerVariableEqualValueClause(IntVar* const var, int64 value);
  void AddIntegerVariableGreaterOrEqualValueClause(IntVar* const var,
                                                   int64 value);
  void AddIntegerVariableLessOrEqualValueClause(IntVar* const var, int64 value);

 private:
  friend class SymmetryManager;
  void set_symmetry_manager_and_index(SymmetryManager* manager, int index) {
    CHECK(symmetry_manager_ == NULL);
    CHECK_EQ(-1, index_in_symmetry_manager_);
    symmetry_manager_ = manager;
    index_in_symmetry_manager_ = index;
  }
  SymmetryManager* symmetry_manager() const { return symmetry_manager_; }
  int index_in_symmetry_manager() const { return index_in_symmetry_manager_; }

  SymmetryManager* symmetry_manager_;
  // Index of the symmetry breaker when used inside the symmetry manager.
  int index_in_symmetry_manager_;
};

// ---------- Search Log ---------

// The base class of all search logs that periodically outputs information when
// the search is runnning.
class SearchLog : public SearchMonitor {
 public:
  SearchLog(Solver* const s,
            OptimizeVar* const obj,
            IntVar* const var,
            ResultCallback<string>* display_callback,
            int period);
  virtual ~SearchLog();
  virtual void EnterSearch();
  virtual void ExitSearch();
  virtual bool AtSolution();
  virtual void BeginFail();
  virtual void NoMoreSolutions();
  virtual void ApplyDecision(Decision* const decision);
  virtual void RefuteDecision(Decision* const decision);
  void OutputDecision();
  void Maintain();
  virtual void BeginInitialPropagation();
  virtual void EndInitialPropagation();

 protected:
  /* Bottleneck function used for all UI related output. */
  virtual void OutputLine(const string& line);

 private:
  static string MemoryUsage();

  const int period_;
  scoped_ptr<WallTimer> timer_;
  IntVar* const var_;
  OptimizeVar* const obj_;
  scoped_ptr<ResultCallback<string> > display_callback_;
  int nsol_;
  int64 tick_;
  int64 objective_min_;
  int64 objective_max_;
  int min_right_depth_;
  int max_depth_;
  int sliding_min_depth_;
  int sliding_max_depth_;
};

// Implements a complete cache for model elements: expressions and
// constraints.  Caching is based on the signatures of the elements, as
// well as their types.  This class is used internally to avoid creating
// duplicate objects.
class ModelCache {
 public:
  enum VoidConstraintType {
    VOID_FALSE_CONSTRAINT = 0,
    VOID_TRUE_CONSTRAINT,
    VOID_CONSTRAINT_MAX,
  };

  enum VarConstantConstraintType {
    VAR_CONSTANT_EQUALITY = 0,
    VAR_CONSTANT_GREATER_OR_EQUAL,
    VAR_CONSTANT_LESS_OR_EQUAL,
    VAR_CONSTANT_NON_EQUALITY,
    VAR_CONSTANT_CONSTRAINT_MAX,
  };

  enum VarConstantConstantConstraintType {
    VAR_CONSTANT_CONSTANT_BETWEEN = 0,
    VAR_CONSTANT_CONSTANT_CONSTRAINT_MAX,
  };

  enum VarVarConstraintType {
    VAR_VAR_EQUALITY = 0,
    VAR_VAR_GREATER,
    VAR_VAR_GREATER_OR_EQUAL,
    VAR_VAR_LESS,
    VAR_VAR_LESS_OR_EQUAL,
    VAR_VAR_NON_EQUALITY,
    VAR_VAR_CONSTRAINT_MAX,
  };

  enum VarExpressionType {
    VAR_OPPOSITE = 0,
    VAR_ABS,
    VAR_SQUARE,
    VAR_EXPRESSION_MAX,
  };

  enum VarConstantExpressionType {
    VAR_CONSTANT_DIFFERENCE = 0,
    VAR_CONSTANT_DIVIDE,
    VAR_CONSTANT_PROD,
    VAR_CONSTANT_MAX,
    VAR_CONSTANT_MIN,
    VAR_CONSTANT_SUM,
    VAR_CONSTANT_IS_EQUAL,
    VAR_CONSTANT_IS_NOT_EQUAL,
    VAR_CONSTANT_IS_GREATER_OR_EQUAL,
    VAR_CONSTANT_IS_LESS_OR_EQUAL,
    VAR_CONSTANT_EXPRESSION_MAX,
  };

  enum VarVarExpressionType {
    VAR_VAR_DIFFERENCE = 0,
    VAR_VAR_PROD,
    VAR_VAR_MAX,
    VAR_VAR_MIN,
    VAR_VAR_SUM,
    VAR_VAR_EXPRESSION_MAX,
  };

  enum VarConstantConstantExpressionType {
    VAR_CONSTANT_CONSTANT_SEMI_CONTINUOUS = 0,
    VAR_CONSTANT_CONSTANT_EXPRESSION_MAX,
  };

  enum VarConstantArrayExpressionType {
    VAR_CONSTANT_ARRAY_ELEMENT = 0,
    VAR_CONSTANT_ARRAY_EXPRESSION_MAX,
  };

  enum VarArrayExpressionType {
    VAR_ARRAY_MAX = 0,
    VAR_ARRAY_MIN,
    VAR_ARRAY_SUM,
    VAR_ARRAY_EXPRESSION_MAX,
  };

  explicit ModelCache(Solver* const solver);
  virtual ~ModelCache();

  // Void constraints.

  virtual Constraint* FindVoidConstraint(VoidConstraintType type) const = 0;

  virtual void InsertVoidConstraint(Constraint* const ct,
                                    VoidConstraintType type) = 0;

  // Var Constant Constraints.
  virtual Constraint* FindVarConstantConstraint(
      IntVar* const var,
      int64 value,
      VarConstantConstraintType type) const = 0;

  virtual void InsertVarConstantConstraint(
      Constraint* const ct,
      IntVar* const var,
      int64 value,
      VarConstantConstraintType type) = 0;

  // Var Constant Constant Constraints.

  virtual Constraint* FindVarConstantConstantConstraint(
      IntVar* const var,
      int64 value1,
      int64 value2,
      VarConstantConstantConstraintType type) const = 0;

  virtual void InsertVarConstantConstantConstraint(
      Constraint* const ct,
      IntVar* const var,
      int64 value1,
      int64 value2,
      VarConstantConstantConstraintType type) = 0;

  // Var Var Constraints.

  virtual Constraint* FindVarVarConstraint(
      IntVar* const var1,
      IntVar* const var2,
      VarVarConstraintType type) const = 0;

  virtual void InsertVarVarConstraint(Constraint* const ct,
                                      IntVar* const var1,
                                      IntVar* const var2,
                                      VarVarConstraintType type) = 0;

  // Var Expressions.

  virtual IntExpr* FindVarExpression(
      IntVar* const var,
      VarExpressionType type) const = 0;

  virtual void InsertVarExpression(IntExpr* const expression,
                                   IntVar* const var,
                                   VarExpressionType type) = 0;

  // Var Constant Expressions .

  virtual IntExpr* FindVarConstantExpression(
      IntVar* const var,
      int64 value,
      VarConstantExpressionType type) const = 0;

  virtual void InsertVarConstantExpression(
      IntExpr* const expression,
      IntVar* const var,
      int64 value,
      VarConstantExpressionType type) = 0;

  // Var Var Expressions.

  virtual IntExpr* FindVarVarExpression(
      IntVar* const var1,
      IntVar* const var2,
      VarVarExpressionType type) const = 0;

  virtual void InsertVarVarExpression(
      IntExpr* const expression,
      IntVar* const var1,
      IntVar* const var2,
      VarVarExpressionType type) = 0;

  // Var Constant Constant Expressions.

  virtual IntExpr* FindVarConstantConstantExpression(
      IntVar* const var,
      int64 value1,
      int64 value2,
      VarConstantConstantExpressionType type) const = 0;

  virtual void InsertVarConstantConstantExpression(
      IntExpr* const expression,
      IntVar* const var,
      int64 value1,
      int64 value2,
      VarConstantConstantExpressionType type) = 0;

  // Var Constant Array Expressions.

  virtual IntExpr* FindVarConstantArrayExpression(
      IntVar* const var,
      ConstIntArray* const values,
      VarConstantArrayExpressionType type) const = 0;

  virtual void InsertVarConstantArrayExpression(
      IntExpr* const expression,
      IntVar* const var,
      ConstIntArray* const values,
      VarConstantArrayExpressionType type) = 0;

  // Var Array Expressions.

  virtual IntExpr* FindVarArrayExpression(
      ConstPtrArray<IntVar>* const vars,
      VarArrayExpressionType type) const = 0;

  virtual void InsertVarArrayExpression(
      IntExpr* const expression,
      ConstPtrArray<IntVar>* const vars,
      VarArrayExpressionType type) = 0;

  Solver* solver() const;

 private:
  Solver* const solver_;
};

#if !defined(SWIG)
// Implements a data structure useful for scheduling.
// It is meant to store simple temporal constraints and to propagate
// efficiently on the nodes of this temporal graph.
class DependencyGraphNode;
class DependencyGraph : public BaseObject {
 public:
  virtual ~DependencyGraph();

  // start(left) >= end(right) + delay.
  void AddStartsAfterEndWithDelay(IntervalVar* const left,
                                  IntervalVar* const right,
                                  int64 delay);

  // start(left) == end(right) + delay.
  void AddStartsAtEndWithDelay(IntervalVar* const left,
                               IntervalVar* const right,
                               int64 delay);

  // start(left) >= start(right) + delay.
  void AddStartsAfterStartWithDelay(IntervalVar* const left,
                                    IntervalVar* const right,
                                    int64 delay);

  // start(left) == start(right) + delay.
  void AddStartsAtStartWithDelay(IntervalVar* const left,
                                 IntervalVar* const right,
                                 int64 delay);

  // Internal API.

  // Factory to create a node from an interval var. This node is
  // attached to the start of the interval var.
  DependencyGraphNode* BuildStartNode(IntervalVar* const var);

  // Adds left == right + offset.
  virtual void AddEquality(DependencyGraphNode* const left,
                           DependencyGraphNode* const right,
                           int64 offset) = 0;
  // Adds left >= right + offset.
  virtual void AddInequality(DependencyGraphNode* const left,
                             DependencyGraphNode* const right,
                             int64 offset) = 0;

  // Tell the graph that this node has changed.
  // If applied_to_min_or_max is true, the min has changed.
  // If applied_to_min_or_max is false, the max has changed.
  virtual void Enqueue(DependencyGraphNode* const node,
                       bool applied_to_min_or_max) = 0;

 private:
  hash_map<IntervalVar*, DependencyGraphNode*> start_node_map_;
  std::vector<DependencyGraphNode*> managed_nodes_;
};
#endif
}  // namespace operations_research

#endif  // OR_TOOLS_CONSTRAINT_SOLVER_CONSTRAINT_SOLVERI_H_
