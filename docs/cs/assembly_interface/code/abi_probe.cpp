#include <cstdarg>

/* 参数传递、可变参数与结构体返回
   GCC: -O0 -std=c++17；MSVC: /Od /std:c++17 */
#if defined( _MSC_VER )
#define NOINLINE __declspec( noinline )
#else
#define NOINLINE __attribute__( ( noinline ) )
#endif

struct Small {
    int x;
    int y;
};

struct Pair {
    long long x;
    long long y;
};

struct Large {
    long long x;
    long long y;
    long long z;
};

struct Huge {
    long long x;
    long long y;
    long long z;
    long long w;
};

struct Base {
    virtual int transform( int x );
};

/* 整数、浮点与指针参数。 */
extern "C" NOINLINE int integer_args(
    int a, int b, int c, int d, int e, int f
){
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}

extern "C" NOINLINE int integer_overflow(
    int a, int b, int c, int d, int e, int f, int g, int h
){
    return a + b + c + d + e + f + g + h;
}

extern "C" NOINLINE double floating_args(
    double a, double b, double c, double d, double e
){
    return a + b * c + d * e;
}

extern "C" NOINLINE double floating_overflow(
    double a, double b, double c, double d, double e,
    double f, double g, double h, double i, double j
){
    return a + b + c + d + e + f + g + h + i + j;
}

extern "C" NOINLINE int pointer_arg( const int* pointer ){
    return *pointer;
}

/* 按值传递的结构体与寄存器返回值。 */
extern "C" NOINLINE long long small_struct_arg( Small value ){
    return static_cast< long long >( value.x ) + value.y;
}

extern "C" NOINLINE long long pair_struct_arg( Pair value ){
    return value.x + value.y;
}

extern "C" NOINLINE long long large_struct_arg( Large value ){
    return value.x + value.y + value.z;
}

extern "C" NOINLINE long long huge_struct_arg( Huge value ){
    return value.x + value.y + value.z + value.w;
}

extern "C" NOINLINE Pair make_pair( long long x, long long y ){
    return { x + 1, y + 2 };
}

extern "C" NOINLINE int call_integer_args( int x ){
    return integer_args( x, 2, 3, 4, 5, 6 );
}

/* 虚调用探针：Base::transform 只声明，观察汇编时无需链接实现 */
extern "C" NOINLINE int call_virtual( Base* object, int x ){
    return object -> transform( x );
}

/* 匿名参数读取与可变参数调用；variadic_sink 只声明，不链接运行 */
extern "C" NOINLINE int first_variadic( const char* format, ... ){
    va_list arguments;
    va_start( arguments, format );
    const int value = va_arg( arguments, int );
    va_end( arguments );
    return value;
}

extern "C" double variadic_sink( const char* format, ... );

extern "C" NOINLINE double call_variadic( double value ){
    return variadic_sink( "%f", value );
}

/* 仅 32 位 MSVC 使用这些传统调用约定 */
#if defined( _MSC_VER ) && defined( _M_IX86 )
extern "C" NOINLINE int __stdcall stdcall_args( int a, int b, int c ){
    return a + 2 * b + 3 * c;
}

extern "C" NOINLINE int __fastcall fastcall_args( int a, int b, int c ){
    return a + 2 * b + 3 * c;
}
#endif
