#pragma once

template<std::size_t Size = 256>
class bumping_memory_resource {
public:
    char buffer[Size];
    char* _ptr;

    explicit bumping_memory_resource()
        : _ptr(&buffer[0]) {}

    void* allocate(std::size_t size) noexcept {
        auto ret = _ptr;
        _ptr += size;
        return ret;
    }

    void deallocate(void*) noexcept {}
};

template <typename T, typename Resource = bumping_memory_resource<256>>
class bumping_allocator {
    Resource* _res;

public:
    using value_type = T;

    explicit bumping_allocator(Resource& res)
        : _res(&res) {}

    bumping_allocator(const bumping_allocator&) = default;
    template <typename U>
    bumping_allocator(const bumping_allocator<U, Resource>& other)
        : bumping_allocator(other.resource()) {}

    Resource& resource() const { return *_res; }

    T* allocate(std::size_t n) { return static_cast<T*>(_res->allocate(sizeof(T) * n)); }
    void deallocate(T* ptr, std::size_t) { _res->deallocate(ptr); }

    friend bool operator==(const bumping_allocator& lhs, const bumping_allocator& rhs) {
        return lhs._res == rhs._res;
    }

    friend bool operator!=(const bumping_allocator& lhs, const bumping_allocator& rhs) {
        return lhs._res != rhs._res;
    }
};

template <typename T, size_t Size>
class StackVector
{
    using InternalVector = std::vector<T, bumping_allocator<T, bumping_memory_resource<Size * sizeof(T)>>>;
    bumping_memory_resource<Size * sizeof(T)> resource_;
    bumping_allocator<T, bumping_memory_resource<Size * sizeof(T)>> allocator_;
    InternalVector internalVector_;
public:
	StackVector() : allocator_(resource_), internalVector_(allocator_)
	{
        internalVector_.reserve(Size);
	}

    InternalVector* operator->() { return &internalVector_; }
    const InternalVector* operator->() const { return &internalVector_; }
};
