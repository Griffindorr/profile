/* 栈参数、局部对象、结构体返回、red zone
   escape 只声明；观察 Compiler Explorer 汇编时无需链接实现。 */
#if defined( _MSC_VER )
#define NOINLINE __declspec( noinline )
#else
#define NOINLINE __attribute__( ( noinline ) )
#endif

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

extern "C" void escape( void* address );

/* 大结构体通过隐藏结果地址返回。 */
extern "C" NOINLINE Large make_large( long long x, long long y, long long z ){
    Large result{ x + 1, y + 2, z + 3 };
    return result;
}

extern "C" NOINLINE Huge make_huge(
    long long x, long long y, long long z, long long w
){
    Huge result{ x + 1, y + 2, z + 3, w + 4 };
    return result;
}

extern "C" NOINLINE int frame_probe(
    int a, int b, int c, int d, int e, int f, int g
){
    volatile int local_a = a + b;
    volatile int local_b = c + d;
    volatile long long local_c = static_cast< long long >( e ) + f;
    char local_bytes[ 32 ]{ };
    local_bytes[ 0 ] = static_cast< char >( g );
    escape( local_bytes );
    return local_a + local_b + static_cast< int >( local_c ) + local_bytes[ 0 ];
}

/* 与 -mno-red-zone 对比；不调用其他函数，保持叶子函数形态 */
extern "C" NOINLINE int red_zone_probe( int value ){
    volatile unsigned char scratch[ 96 ]{ };
    scratch[ 0 ] = static_cast< unsigned char >( value );
    scratch[ 95 ] = static_cast< unsigned char >( value + 1 );
    return scratch[ 0 ] + scratch[ 95 ];
}

extern "C" NOINLINE Large call_make_large( long long value ){
    return make_large( value, value + 1, value + 2 );
}
