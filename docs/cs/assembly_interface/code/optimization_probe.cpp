/* 对比调用开销、inline 提示和 -O0/-O1/-O2/-O3 的循环代码
   NOINLINE 仅约束被标记的函数；inline_add 是否展开取决于优化器 */
#if defined( _MSC_VER )
#define NOINLINE __declspec( noinline )
#else
#define NOINLINE __attribute__( ( noinline ) )
#endif

extern "C" NOINLINE int noinline_add( int a, int b ){
    return a + b;
}

inline int inline_add( int a, int b ){
    return a + b;
}

/* 单次调用：比较保留 call 与将加法展开到调用点。 */
extern "C" NOINLINE int call_noinline( int x ){
    return noinline_add( x, 3 );
}

extern "C" NOINLINE int call_inline( int x ){
    return inline_add( x, 3 );
}

/* 循环调用：观察调用成本，以及 -O3 下是否产生 SIMD 主循环 */
extern "C" NOINLINE int loop_with_call( const int* values, int count ){
    int total = 0;
    for( int i = 0; i < count; ++i ){
        total += noinline_add( values[ i ], i );
    }
    return total;
}

extern "C" NOINLINE int loop_with_inline( const int* values, int count ){
    int total = 0;
    for( int i = 0; i < count; ++i ){
        total += inline_add( values[ i ], i );
    }
    return total;
}
