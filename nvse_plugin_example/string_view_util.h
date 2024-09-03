#pragma once
#include "utility.h"

namespace sv
{
    inline std::string_view get_file_extension(std::string_view path)
    {
        const auto pos = path.find_last_of('.');
        if (pos == std::string::npos)
            return "";
        return path.substr(pos);
    }

    inline std::string_view get_file_name(std::string_view path)
    {
        const auto pos = path.find_last_of("\\/");
        if (pos == std::string::npos)
            return "";
        return path.substr(pos + 1);
    }

    inline std::string_view get_file_stem(std::string_view path)
    {
        const auto pos = path.find_last_of("\\/");
        if (pos == std::string::npos)
            return "";
        const auto dotPos = path.find_last_of('.');
        if (dotPos == std::string::npos)
            return "";
        return path.substr(pos + 1, dotPos - pos - 1);
    }

    inline bool equals_ci(std::string_view left, std::string_view right)
    {
        if (left.size() != right.size())
            return false;
        for (size_t i = 0; i < left.size(); ++i)
        {
            if (std::tolower(left[i]) != std::tolower(right[i]))
                return false;
        }
        return true;
    }

    inline bool contains_ci(std::string_view left, std::string_view right)
    {
        return FindStringCI(left, right);
    }

    inline bool ends_with_ci(std::string_view left, std::string_view right)
    {
        if (right.size() > left.size())
            return false;

        std::string_view left_substr = left.substr(left.size() - right.size(), right.size());

        return std::ranges::equal(left_substr, right, [](char a, char b)
        {
            return std::tolower(a) == std::tolower(b);
        });
    }

    template <size_t N>
    class stack_string
    {
        char data_[N];
        size_t size_ = 0;
    public:
        stack_string()
        {
            data_[0] = '\0';
            size_ = 0;
        }

        stack_string(const char* str)
        {
            strncpy_s(data, str, N);
            size_ = strlen(data_);
        }

        stack_string(const char* format, ...)
        {
            va_list args;
            va_start(args, format);
            auto numCopied = vsprintf_s(data_, N, N, format, args);
            va_end(args);
            if (numCopied > 0)
            {
                size_ = numCopied;
            }
#ifdef _DEBUG
            assert(size_ <= N);
            assert(numCopied >= 0);
#endif
        }

        operator const char* () const
        {
            return data_;
        }

        operator std::string_view() const
        {
            return { data_, size_ };
        }

        const char* c_str() const
        {
            return data_;
        }

        char* data()
        {
            return data_;
        }

        std::string_view str() const
        {
            return { data_, size_ };
        }

        void clear()
        {
            data_[0] = '\0';
            size_ = 0;
        }

        bool empty() const
        {
            return size_ == 0;
        }

        size_t size() const
        {
            return size_;
        }

        char& operator[](size_t index)
        {
            return data_[index];
        }

        const char& operator[](size_t index) const
        {
            return data_[index];
        }

        void to_lower()
        {
            std::transform(data_, data_ + size_, data_, [](const unsigned char c) { return std::tolower(c); });
        }

        void calculate_size()
        {
            size_ = strlen(data_);
        }
    };
}
