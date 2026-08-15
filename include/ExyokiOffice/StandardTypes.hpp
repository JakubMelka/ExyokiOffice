// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ExyokiOffice
{

/// \name Fixed-width integers
/// Spelled without the `std::` prefix and the `_t` suffix so that declarations
/// in the library read closer to the OpenXML simple types they carry.
/// \{

using Int8 = std::int8_t;     ///< Signed 8-bit integer.
using Int16 = std::int16_t;   ///< Signed 16-bit integer.
using Int32 = std::int32_t;   ///< Signed 32-bit integer.
using Int64 = std::int64_t;   ///< Signed 64-bit integer.
using UInt8 = std::uint8_t;   ///< Unsigned 8-bit integer.
using UInt16 = std::uint16_t; ///< Unsigned 16-bit integer.
using UInt32 = std::uint32_t; ///< Unsigned 32-bit integer.
using UInt64 = std::uint64_t; ///< Unsigned 64-bit integer.

/// \}

/// \name Floating point
/// Named after the OpenXML simple types they carry: `Real` backs `xsd:double`,
/// `Single` backs `xsd:float`, and `RealExtended` is the widest type available,
/// used for range checks and for `xsd:decimal`.
/// \{

using Real = double;              ///< Default floating-point type of the library.
using Single = float;             ///< Narrow floating-point type, for formats storing 32-bit reals.
using RealExtended = long double; ///< Widest floating-point type, for parsing and range checks.

/// \}

/// \name Memory and byte buffers
/// \{

using Size = std::size_t;       ///< Object and container size, and index into one.
using PtrDiff = std::ptrdiff_t; ///< Signed distance between two pointers or iterators.
using Byte = std::uint8_t;      ///< Element of an opaque byte buffer, such as a package part payload.

/// \}

} // namespace ExyokiOffice
