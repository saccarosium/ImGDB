// Layer Zero

#pragma once

// Aliases

using i8 = int8_t;
using u8 = uint8_t;
using i32 = int32_t;
using u32 = uint32_t;
using i64 = int64_t;
using u64 = uint64_t;
using usize = size_t;
using isize = ptrdiff_t;

using String = std::string;
using StringView = std::string_view;

template <typename T> using Vector = std::vector<T>;
template <typename T, usize S> using Array = std::array<T, S>;
template <typename T> using Optional = std::optional<T>;

// Macros

#define KB(x) (x * 1024)
#define MB(x) (x * 1024 * 1024)
#define GB(x) (x * 1024 * 1024 * 1024)
