#pragma once
#include <stdexcept>


template <typename T>
class SimpleFixedArray
{
    unsigned int m_uiNumItems;
    T* m_pData;

public:
    SimpleFixedArray() : m_uiNumItems(0), m_pData(nullptr) {}

    explicit SimpleFixedArray(unsigned int size) : m_uiNumItems(size), m_pData(nullptr)
    {
        if (size <= 0)
            return;
        m_pData = static_cast<T*>(GameHeapAlloc(sizeof(T) * size));
            
        for (unsigned int i = 0; i < size; ++i)
            new (&m_pData[i]) T();
    }

    SimpleFixedArray(const SimpleFixedArray& other) : m_uiNumItems(other.m_uiNumItems), m_pData(nullptr)
    {
        if (!m_uiNumItems)
            return;
        m_pData = static_cast<T*>(GameHeapAlloc(sizeof(T) * m_uiNumItems));
            
        for (unsigned int i = 0; i < m_uiNumItems; ++i)
        {
            new (&m_pData[i]) T(other.m_pData[i]);
        }
    }

    SimpleFixedArray(SimpleFixedArray&& other) noexcept : m_uiNumItems(other.m_uiNumItems), m_pData(other.m_pData)
    {
        other.m_uiNumItems = 0;
        other.m_pData = nullptr;
    }

    SimpleFixedArray& operator=(const SimpleFixedArray& other)
    {
        if (this == &other)
            return *this;
        Cleanup();
            
        m_uiNumItems = other.m_uiNumItems;
            
        if (!m_uiNumItems)
        {
            m_pData = nullptr;
            return *this;
        }

        m_pData = static_cast<T*>(GameHeapAlloc(sizeof(T) * m_uiNumItems));
                
        for (unsigned int i = 0; i < m_uiNumItems; ++i)
        {
            new (&m_pData[i]) T(other.m_pData[i]);
        }
        return *this;
    }

    SimpleFixedArray& operator=(SimpleFixedArray&& other) noexcept
    {
        if (this == &other)
            return *this;
        Cleanup();
            
        m_uiNumItems = other.m_uiNumItems;
        m_pData = other.m_pData;
            
        other.m_uiNumItems = 0;
        other.m_pData = nullptr;
        return *this;
    }

    ~SimpleFixedArray()
    {
        Cleanup();
    }

    T& operator[](unsigned int index)
    {
        if (index >= m_uiNumItems)
            throw std::out_of_range("Index out of range");
        return m_pData[index];
    }

    const T& operator[](unsigned int index) const
    {
        if (index >= m_uiNumItems)
            throw std::out_of_range("Index out of range");
        return m_pData[index];
    }

    unsigned int Size() const
    {
        return m_uiNumItems;
    }

    bool Empty() const
    {
        return m_uiNumItems == 0;
    }

private:
    void Cleanup()
    {
        m_uiNumItems = 0;
        if (!m_pData)
            return;
        for (unsigned int i = 0; i < m_uiNumItems; ++i)
            m_pData[i].~T();

        GameHeapFree(m_pData);
        m_pData = nullptr;
    }
};
static_assert(sizeof(SimpleFixedArray<void*>) == 0x8);