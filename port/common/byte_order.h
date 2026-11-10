// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// The console's byte order on any host. The game's data is little-endian, as every host the port
// has run on; a big-endian host (the PS3's Cell) reads a number from the console's bytes the other
// way round. from_little_endian() turns a value copied from the console's bytes into the host's,
// and back, the swap being its own inverse: a number is swapped, an array element by element, a
// record of several fields (an aggregate, as the asset readers' record types are) field by field,
// through nesting. So a reader copies a record as its bytes are and converts it whole.
//
// A record's fields are found without reflection: field_count() tests which aggregate
// initialisations compile, and byte_order_fields.h binds that many with a structured binding. A
// record is a plain aggregate of numbers, arrays and other such records.

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "common/byte_order_fields.h"

namespace openrac::byte_order {

// The host's order, as a constant every reader can test.
inline constexpr bool kHostIsLittleEndian = std::endian::native == std::endian::little;

// Converts to anything: the value field_count() offers an aggregate's fields.
struct AnyField {
    template <typename T>
    operator T() const;
};

// Whether T is a std::array.
template <typename T>
struct IsStdArray : std::false_type {};

template <typename T, std::size_t N>
struct IsStdArray<std::array<T, N>> : std::true_type {};

// Whether T can be aggregate-initialised from sizeof...(I) values.
template <typename T, std::size_t... I>
consteval bool accepts_fields(std::index_sequence<I...>) {
    return requires { T{((void)I, AnyField{})...}; };
}

// The number of fields of aggregate T: the most values its aggregate initialisation accepts. A
// std::array member counts as one field, as a value that converts to it initialises it whole; a C
// array member would be counted element by element (nothing converts to an array, so its braces
// are elided) and the binding would not compile, so a record's arrays are std::array.
template <typename T, std::size_t count = 0>
consteval std::size_t field_count() {
    // A record has no more fields than bytes, so the search ends for any type.
    if constexpr (count < sizeof(T) && accepts_fields<T>(std::make_index_sequence<count + 1>{})) {
        return field_count<T, count + 1>();
    } else {
        return count;
    }
}

// A number of 2, 4 or 8 bytes with its bytes reversed: the one place the port names the compiler's
// byte-swap builtins (CODING_CONVENTIONS.md, 4.3), as C++20 has no standard form of them.
template <typename T>
T swapped(T value) {
    static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
    if constexpr (sizeof(T) == 2) {
        return std::bit_cast<T>(__builtin_bswap16(std::bit_cast<std::uint16_t>(value)));
    } else if constexpr (sizeof(T) == 4) {
        return std::bit_cast<T>(__builtin_bswap32(std::bit_cast<std::uint32_t>(value)));
    } else {
        static_assert(sizeof(T) == 8);
        return std::bit_cast<T>(__builtin_bswap64(std::bit_cast<std::uint64_t>(value)));
    }
}

// Reverses the bytes of every number in `value`: a number itself, each element of an array, each
// field of a record, through nesting. A byte stays as it is.
template <typename T>
void swap_in_place(T& value) {
    if constexpr (sizeof(T) == 1) {
        // A byte has one order.
    } else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
        value = swapped(value);
    } else if constexpr (std::is_array_v<T> || IsStdArray<T>::value) {
        for (auto& element : value) {
            swap_in_place(element);
        }
    } else {
        static_assert(std::is_aggregate_v<T>, "a record is an aggregate of numbers and arrays");
        bind_fields<field_count<T>()>(value, [](auto& field) { swap_in_place(field); });
    }
}

// VALUE with the bytes of every number in it reversed.
template <typename T>
T byte_swapped(T value) {
    swap_in_place(value);
    return value;
}

// VALUE, copied from the console's little-endian bytes, in the host's order; and, the swap being
// its own inverse, a host value as the console's bytes.
template <typename T>
T from_little_endian(T value) {
    if constexpr (kHostIsLittleEndian) {
        return value;
    } else {
        return byte_swapped(value);
    }
}

}  // namespace openrac::byte_order
