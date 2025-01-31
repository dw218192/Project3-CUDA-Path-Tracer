#pragma once

#include <glm/glm.hpp>

#include "intellisense.h"
#include <cuda.h>

#include <algorithm>
#include <istream>
#include <ostream>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>


#define INV_PI            0.3183098861837907f
#define PI                3.1415926535897932384626422832795028841971f
#define TWO_PI            6.2831853071795864769252867665590057683943f
#define SQRT_OF_ONE_THIRD 0.5773502691896257645091487805019574556476f
#define EPSILON           0.00001f

// convenience macro
#ifndef NDEBUG
void checkCUDAErrorFn(const char* msg, const char* file, int line);
#define FILENAME (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)
#define checkCUDAError(msg) checkCUDAErrorFn(msg, FILENAME, __LINE__)
#define CHECK_CUDA(func_call) do { if(func_call != cudaSuccess) checkCUDAError("cuda func failed:\n" ## #func_call); } while(0)
#define PRINT_GPU(dev_arr, size) printGPU(#dev_arr, dev_arr, size)
#else
#define checkCUDAError(msg) (void)0
#define CHECK_CUDA(func_call) func_call
#define PRINT_GPU(dev_arr, size) (void)0
#endif // !NDEBUG

#define ALLOC(name, size) CHECK_CUDA(cudaMalloc((void**)&(name), (size) * sizeof(*name)))
#define MEMSET(name, val, size) CHECK_CUDA(cudaMemset(name, val, size))
#define ZERO(name, size) CHECK_CUDA(cudaMemset(name, 0, (size) * sizeof(*name)))
#define FREE(name) CHECK_CUDA(cudaFree(name))
#define H2D(dev_name, name, size) CHECK_CUDA(cudaMemcpy(dev_name, name, (size) * sizeof(*name), cudaMemcpyHostToDevice))
#define D2H(name, dev_name, size) CHECK_CUDA(cudaMemcpy(name, dev_name, (size) * sizeof(*name), cudaMemcpyDeviceToHost))
#define D2D(dev_name1, dev_name2, size) CHECK_CUDA(cudaMemcpy(dev_name1, dev_name2, (size) * sizeof(*dev_name1), cudaMemcpyDeviceToDevice))
#define DIV_UP(x, y) (((x) + (y) - 1) / (y))

template<typename T>
static inline void printGPU(char const* name, T* dev, int n) {
    T* tmp = new T[n];
    std::cout << name << "\n";
    D2H(tmp, dev, n);
    for (int i = 0; i < n; ++i)
        std::cout << tmp[i] << " \n"[i < n - 1 ? 0 : 1];
    delete[] tmp;
}

template<typename T>
static inline T getGPU(T * dev, int i) {
    T tmp;
    D2H(&tmp, dev + i, 1);
    return tmp;
}
template<typename T>
static inline void setGPU(T * dev, int i, T val) {
    H2D(dev + i, &val, 1);
}

#ifdef __CUDACC__
#define DEVICE __device__
#define HOST __host__
#define INLINE __forceinline__
#define GLOBAL __global__
#else
#define DEVICE
#define HOST
#define INLINE
#define GLOBAL
#endif

/// <summary>
/// non-owning view on a GPU array, which
/// may only be indexed on GPU
/// </summary>
/// <typeparam name="T"></typeparam>
template<typename T>
struct Span {
    int _size;
    T* _arr;
    HOST DEVICE INLINE Span() : _size(0), _arr(nullptr) { }
    HOST DEVICE INLINE Span(int size, T* arr) : _size(size), _arr(arr) { }
    HOST DEVICE INLINE operator T* () const {
        return _arr;
    }
    HOST DEVICE INLINE Span<T> subspan(int idx, int sub_sz) const {
#ifndef NDEBUG
        if (idx < 0 || idx + sub_sz > _size) {
            assert(!"invalid subspan");
        }
#endif
        return Span<T>(sub_sz, _arr + idx);
    }
    DEVICE INLINE T& operator[](int idx) {
#ifndef NDEBUG
        if (idx < 0 || idx >= _size) {
            printf("array out of bounds, idx=%d, size=%d\n", idx, _size);
            assert(false);
        }
#endif // !NDEBUG
        return _arr[idx];
    }
    DEVICE INLINE T const& operator[](int idx) const {
        return const_cast<Span<T>*>(this)->operator[](idx);
    }
    HOST DEVICE INLINE T*& get() { return _arr; }
    HOST DEVICE INLINE T* const& get() const { return _arr; }
    HOST DEVICE INLINE int size() const { return _size; }
};



template<typename T>
Span<T> make_span(T const* hst_ptr, int n) {
    if (hst_ptr) {
        T* tmp;
        ALLOC(tmp, n);
        H2D(tmp, const_cast<T*>(hst_ptr), n);
        return Span<T>(n, tmp);
    } else {
        return Span<T>();
    }
}
template<typename T>
Span<T> make_span(std::vector<T> const& hst_vec) {
    if (hst_vec.size()) {
        return make_span(hst_vec.data(), hst_vec.size());
    } else {
        return Span<T>();
    }
}
template<typename T>
Span<T> make_span(int n) {
    if (n) {
        T* tmp;
        ALLOC(tmp, n);
        ZERO(tmp, n);
        return Span<T>(n, tmp);
    } else {
        return Span<T>();
    }
}

namespace utilityCore {
    std::vector<std::string> getFilesInDir(char const* dir);
    float clamp(float f, float min, float max);
    bool replaceString(std::string& str, const std::string& from, const std::string& to);
    glm::vec3 clampRGB(glm::vec3 color);
    bool epsilonCheck(float a, float b);
    std::vector<std::string> tokenizeString(std::string str);
    glm::mat4 buildTransformationMatrix(glm::vec3 translation, glm::vec3 rotation, glm::vec3 scale);
    std::string convertIntToString(int number);
    std::istream& safeGetline(std::istream& is, std::string& t); //Thanks to http://stackoverflow.com/a/6089413
    std::istream& peekline(std::istream& is, std::string& t);

    template<typename Arg, typename... Args>
    void CreateOrAppendCSV(std::string const& filename, Arg&& arg, Args&&... args) {
        std::ofstream fout(filename, std::ios_base::app);
        fout << std::forward<Arg>(arg);
        using expander = int[];
        (void)expander {
            0, (void(fout << ',' << std::forward<Args>(args)), 0)...
        };
        fout << std::endl;
    }
}