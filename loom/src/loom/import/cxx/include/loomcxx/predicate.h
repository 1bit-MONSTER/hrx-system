// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMCXX_PREDICATE_H_
#define LOOMCXX_PREDICATE_H_

// Predicate expressions describe compile-time contracts. They are admitted
// only inside loom::where attributes and loom::assume calls and have no runtime
// implementation. The helper names match Loom's textual predicate vocabulary.
namespace loom::predicate {

template <class T>
[[loom::predicate("eq")]] bool eq(const T& left, const T& right);
template <class T>
[[loom::predicate("ne")]] bool ne(const T& left, const T& right);
template <class T>
[[loom::predicate("lt")]] bool lt(const T& left, const T& right);
template <class T>
[[loom::predicate("le")]] bool le(const T& left, const T& right);
template <class T>
[[loom::predicate("gt")]] bool gt(const T& left, const T& right);
template <class T>
[[loom::predicate("ge")]] bool ge(const T& left, const T& right);
template <class T>
[[loom::predicate("ult")]] bool ult(const T& left, const T& right);
template <class T>
[[loom::predicate("ule")]] bool ule(const T& left, const T& right);
template <class T>
[[loom::predicate("ugt")]] bool ugt(const T& left, const T& right);
template <class T>
[[loom::predicate("uge")]] bool uge(const T& left, const T& right);
template <class T>
[[loom::predicate("multiple_of")]] bool multiple_of(const T& value,
                                                    const T& divisor);
template <class T>
[[loom::predicate("power_of_two")]] bool power_of_two(const T& value);
template <class T>
[[loom::predicate("range")]] bool range(const T& value, const T& lower,
                                        const T& upper);
template <class T>
[[loom::predicate("not_nan")]] bool not_nan(const T& value);
template <class T>
[[loom::predicate("not_inf")]] bool not_inf(const T& value);
template <class T>
[[loom::predicate("finite")]] bool finite(const T& value);

// Returns the typed result identity of the function carrying the enclosing
// trailing loom::where attribute. A member access selects one scalar component
// of a returned record.
template <class T>
[[loom::predicate_result]] T result();

// Subject-implicit overloads constrain a loom::config declaration. The
// omitted first operand is the declared config value. Explicit arguments use
// that value's source type; integer literals therefore retain their ordinary
// C++ suffix requirements.
template <class T>
[[loom::predicate("eq")]] bool eq(const T& value);
template <class T>
[[loom::predicate("ne")]] bool ne(const T& value);
template <class T>
[[loom::predicate("lt")]] bool lt(const T& value);
template <class T>
[[loom::predicate("le")]] bool le(const T& value);
template <class T>
[[loom::predicate("gt")]] bool gt(const T& value);
template <class T>
[[loom::predicate("ge")]] bool ge(const T& value);
template <class T>
[[loom::predicate("ult")]] bool ult(const T& value);
template <class T>
[[loom::predicate("ule")]] bool ule(const T& value);
template <class T>
[[loom::predicate("ugt")]] bool ugt(const T& value);
template <class T>
[[loom::predicate("uge")]] bool uge(const T& value);
template <class T>
[[loom::predicate("multiple_of")]] bool multiple_of(const T& divisor);
[[loom::predicate("power_of_two")]] bool power_of_two();
template <class T>
[[loom::predicate("range")]] bool range(const T& lower, const T& upper);
[[loom::predicate("not_nan")]] bool not_nan();
[[loom::predicate("not_inf")]] bool not_inf();
[[loom::predicate("finite")]] bool finite();

}  // namespace loom::predicate

#endif  // LOOMCXX_PREDICATE_H_
