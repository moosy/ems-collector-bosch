#pragma once
#include <string>
#include <sstream>
#include <iomanip>

// the following routines convert dec/hex string literal to size_t values

namespace numeric_conversion
{
    template <typename T, bool is_hex>
    size_t string_to_size_t(const std::basic_string<T>& s)
    {
        std::basic_istringstream<T> ss(s, std::ios_base::in);
        size_t size(0);
        ss >> (is_hex ? std::hex : std::dec )>>size;
        return size;
    }

    template <typename T>
    size_t hexstring_to_size_t(const std::basic_string<T>& s)
    {
        return string_to_size_t<T, true>(s);
    }

    template <typename T>
    size_t hexstring_to_size_t(const T* s)
    {
        return string_to_size_t<T, true>(std::basic_string<T>(s));
    }

    template <typename T>
    size_t decstring_to_size_t(const std::basic_string<T>& s)
    {
        return string_to_size_t<T, false>(s);
    }

    template <typename T>
    size_t decstring_to_size_t(const T* s)
    {
        return string_to_size_t<T, false>(std::basic_string<T>(s));
    }

    template <int N>
    std::wstring to_hex_string(size_t i, bool map_0_to_empty)
    {
        if(map_0_to_empty && (0 == i)) return L"";
        std::wstringstream wss;
        wss << L"0x"/*std::showbase*/ << std::hex << std::setw(N) << std::setfill(L'0') << i;
        return wss.str();
    }
}
