// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <algorithm>
#include <random>
#include <span>
#include <frozen/unordered_map.h>
#include <frozen/string.h>
#include <xxhash.h>
#include "base.h"
#include "exception.h"
#ifndef __ANDROID__ // FIX_LINUX
#define PAGE_SIZE 4096
#endif

namespace skyline::util {
    /**
     * @brief Concept for any trivial non-container type
     */
    template<typename T>
    concept TrivialObject = std::is_trivially_copyable_v<T> && !requires(T v) { v.data(); };

    /**
     * @brief Returns the current time in nanoseconds
     * @return The current time in nanoseconds
     */
    inline i64 GetTimeNs() {
        u64 frequency;
        asm("MRS %0, CNTFRQ_EL0" : "=r"(frequency));
        u64 ticks;
        asm("MRS %0, CNTVCT_EL0" : "=r"(ticks));
        return static_cast<i64>(((ticks / frequency) * constant::NsInSecond) + (((ticks % frequency) * constant::NsInSecond + (frequency / 2)) / frequency));
    }

    /**
     * @brief Returns the current time in arbitrary ticks
     * @return The current time in ticks
     */
    inline u64 GetTimeTicks() {
        u64 ticks;
        asm("MRS %0, CNTVCT_EL0" : "=r"(ticks));
        return ticks;
    }

    /**
     * @brief A way to implicitly convert a pointer to uintptr_t and leave it unaffected if it isn't a pointer
     */
    template<typename T>
    constexpr T PointerValue(T item) {
        return item;
    }

    template<typename T>
    uintptr_t PointerValue(T *item) {
        return reinterpret_cast<uintptr_t>(item);
    }

    /**
     * @brief A way to implicitly convert an integral to a pointer, if the return type is a pointer
     */
    template<typename Return, typename T>
    constexpr Return ValuePointer(T item) {
        if constexpr (std::is_pointer<Return>::value)
            return reinterpret_cast<Return>(item);
        else
            return static_cast<Return>(item);
    }

    template<typename T>
    concept IsPointerOrUnsignedIntegral = (std::is_unsigned_v<T> && std::is_integral_v<T>) || std::is_pointer_v<T>;

    /**
     * @return The value aligned up to the next multiple
     * @note The multiple needs to be a power of 2
     */
    template<typename TypeVal>
    requires IsPointerOrUnsignedIntegral<TypeVal>
    constexpr TypeVal AlignUp(TypeVal value, size_t multiple) {
        multiple--;
        return ValuePointer<TypeVal>((PointerValue(value) + multiple) & ~(multiple));
    }

    /**
     * @return The value aligned down to the previous multiple
     * @note The multiple needs to be a power of 2
     */
    template<typename TypeVal>
    requires IsPointerOrUnsignedIntegral<TypeVal>
    constexpr TypeVal AlignDown(TypeVal value, size_t multiple) {
        return ValuePointer<TypeVal>(PointerValue(value) & ~(multiple - 1));
    }

    /**
     * @return If the address is aligned with the multiple
     */
    template<typename TypeVal>
    requires IsPointerOrUnsignedIntegral<TypeVal>
    constexpr bool IsAligned(TypeVal value, size_t multiple) {
        if ((multiple & (multiple - 1)) == 0)
            return !(PointerValue(value) & (multiple - 1U));
        else
            return (PointerValue(value) % multiple) == 0;
    }

    template<typename TypeVal>
    requires IsPointerOrUnsignedIntegral<TypeVal>
    constexpr bool IsPageAligned(TypeVal value) {
        return IsAligned(value, PAGE_SIZE);
    }

    template<typename TypeVal>
    requires IsPointerOrUnsignedIntegral<TypeVal>
    constexpr bool IsWordAligned(TypeVal value) {
        return IsAligned(value, WORD_BIT / 8);
    }

    /**
     * @return The value of division rounded up to the next integral
     */
    template<typename Type>
    requires std::is_integral_v<Type>
    constexpr Type DivideCeil(Type dividend, Type divisor) {
        return (dividend + divisor - 1) / divisor;
    }

    /**
     * @param string The string to create a magic from
     * @return The magic of the supplied string
     */
    template<typename Type>
    requires std::is_integral_v<Type>
    constexpr Type MakeMagic(std::string_view string) {
        Type object{};
        size_t offset{};

        for (auto &character : string) {
            object |= static_cast<Type>(character) << offset;
            offset += sizeof(character) * 8;
        }

        return object;
    }

    constexpr u8 HexDigitToNibble(char digit) {
        if (digit >= '0' && digit <= '9')
            return digit - '0';
        else if (digit >= 'a' && digit <= 'f')
            return digit - 'a' + 10;
        else if (digit >= 'A' && digit <= 'F')
            return digit - 'A' + 10;
        throw exception("Invalid hex character: '{}'", digit);
    }

    template<size_t Size>
    constexpr std::array<u8, Size> HexStringToArray(std::string_view string) {
        if (string.size() != Size * 2)
            throw exception("String size: {} (Expected {})", string.size(), Size);
        std::array<u8, Size> result;
        for (size_t i{}; i < Size; i++) {
            size_t index{i * 2};
            result[i] = static_cast<u8>(HexDigitToNibble(string[index]) << 4) | HexDigitToNibble(string[index + 1]);
        }
        return result;
    }

    template<typename Type>
    requires std::is_integral_v<Type>
    constexpr Type HexStringToInt(std::string_view string) {
        if (string.size() > sizeof(Type) * 2)
            throw exception("String size larger than type: {} (sizeof(Type): {})", string.size(), sizeof(Type));
        Type result{};
        size_t offset{(sizeof(Type) * 8) - 4};
        for (size_t index{}; index < string.size(); index++, offset -= 4) {
            char digit{string[index]};
            if (digit >= '0' && digit <= '9')
                result |= static_cast<Type>(digit - '0') << offset;
            else if (digit >= 'a' && digit <= 'f')
                result |= static_cast<Type>(digit - 'a' + 10) << offset;
            else if (digit >= 'A' && digit <= 'F')
                result |= static_cast<Type>(digit - 'A' + 10) << offset;
            else
                break;
        }
        return result >> (offset + 4);
    }

    template<size_t N>
    constexpr std::array<u8, N> SwapEndianness(std::array<u8, N> in) {
        std::reverse(in.begin(), in.end());
        return in;
    }

    constexpr u64 SwapEndianness(u64 in) {
        return __builtin_bswap64(in);
    }

    constexpr u32 SwapEndianness(u32 in) {
        return __builtin_bswap32(in);
    }

    constexpr u16 SwapEndianness(u16 in) {
        return __builtin_bswap16(in);
    }

    /**
     * @brief A compile-time hash function as std::hash isn't constexpr
     */
    constexpr std::size_t Hash(std::string_view view) {
        return frozen::elsa<frozen::string>{}(frozen::string(view.data(), view.size()), 0);
    }

    /**
     * @brief A fast hash for any trivial object that is designed to be utilized with hash-based containers
     */
    template<typename T> requires std::is_trivial_v<T>
    struct ObjectHash {
        size_t operator()(const T &object) const noexcept {
            return XXH64(&object, sizeof(object), 0);
        }
    };

    /**
     * @brief Selects the largest possible integer type for representing an object alongside providing the size of the object in terms of the underlying type
     */
    template<class T>
    struct IntegerFor {
        using Type = std::conditional_t<sizeof(T) % sizeof(u64) == 0, u64,
                                        std::conditional_t<sizeof(T) % sizeof(u32) == 0, u32,
                                                           std::conditional_t<sizeof(T) % sizeof(u16) == 0, u16, u8>
                                        >
        >;

        static constexpr size_t Count{sizeof(T) / sizeof(Type)};
    };

    namespace detail {
        static thread_local std::mt19937_64 generator{GetTimeTicks()};
    }

    /**
     * @brief Fills an array with random data from a Mersenne Twister pseudo-random generator
     * @note The generator is seeded with the the current time in ticks
     */
    template<typename T>
    requires std::is_integral_v<T>
    void FillRandomBytes(std::span<T> in) {
        std::uniform_int_distribution<u64> dist(std::numeric_limits<T>::min(), std::numeric_limits<T>::max());
        std::generate(in.begin(), in.end(), [&]() { return dist(detail::generator); });
    }

    template<TrivialObject T>
    void FillRandomBytes(T &object) {
        FillRandomBytes(std::span(reinterpret_cast<typename IntegerFor<T>::Type *>(&object), IntegerFor<T>::Count));
    }

    /**
     * @brief A temporary shim for C++ 20's bit_cast to make transitioning to it easier
     */
    template<typename To, typename From>
    To BitCast(const From &from) {
        return *reinterpret_cast<const To *>(&from);
    }

    /**
     * @brief A utility type for placing elements by offset in unions rather than relative position in structs
     * @tparam PadType The type of a unit of padding, total size of padding is `sizeof(PadType) * Offset`
     */
    template<size_t Offset, typename ValueType, typename PadType = u8>
    struct OffsetMember {
      private:
        PadType _pad_[Offset];
        ValueType value;

      public:
        OffsetMember &operator=(const ValueType &pValue) {
            value = pValue;
            return *this;
        }

        auto operator[](std::size_t index) {
            return value[index];
        }

        ValueType &operator*() {
            return value;
        }

        ValueType *operator->() {
            return &value;
        }
    };

    template<typename T, typename... TArgs, size_t... Is>
    std::array<T, sizeof...(Is)> MakeFilledArray(std::index_sequence<Is...>, TArgs &&... args) {
        return {(void(Is), T(args...))...};
    }

    template<typename T, size_t Size, typename... TArgs>
    std::array<T, Size> MakeFilledArray(TArgs &&... args) {
        return MakeFilledArray<T>(std::make_index_sequence<Size>(), std::forward<TArgs>(args)...);
    }
}
