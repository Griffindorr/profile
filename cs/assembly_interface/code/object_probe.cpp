/* 结构体布局、传值/传引用、成员函数、对象构造与虚表
   单独复制本文件到 Compiler Explorer，并切换目标编译器 */
#if defined( _MSC_VER )
#define NOINLINE __declspec( noinline )
#else
#define NOINLINE __attribute__( ( noinline ) )
#endif

struct AlignProbe {
    char tag;
    int value;
    double weight;
};

struct ReorderedProbe {
    double weight;
    int value;
    char tag;
};

/* 普通对象：只有显式数据成员，没有 vptr */
struct Object {
    int value;

    explicit Object( int initial ) : value( initial ){ }

    NOINLINE int add( int x ){
        return value + x;
    }
};

/* 多态对象：构造函数会初始化 vptr
   BaseObject::transform 只声明；观察汇编时无需链接实现 */
struct BaseObject {
    BaseObject( ){ }
    virtual int transform( int x ) const;
    virtual ~BaseObject( ) = default;
};

struct DerivedObject : BaseObject {
    int bias;

    explicit DerivedObject( int initial ) : bias( initial ){ }

    int transform( int x ) const override {
        return x + bias;
    }
};

/* 比较按值、指针与引用传递。 */
extern "C" NOINLINE int by_value( AlignProbe value ){
    return value.value + static_cast< int >( value.weight );
}

extern "C" NOINLINE int by_pointer( const AlignProbe* value ){
    return value->value + static_cast< int >( value->weight );
}

extern "C" NOINLINE int by_reference( const AlignProbe& value ){
    return value.value + static_cast< int >( value.weight );
}

/* 对比直接成员函数调用与虚调用。 */
extern "C" NOINLINE int call_member( Object* object, int x ){
    return object->add( x );
}

extern "C" NOINLINE int call_virtual( const BaseObject* object, int x ){
    return object->transform( x );
}

extern "C" NOINLINE int object_size( ){
    return sizeof( AlignProbe ) + sizeof( ReorderedProbe )
        + alignof( AlignProbe ) + sizeof( Object );
}

/* 局部对象构造：分别观察基类与派生类构造时写入的 vptr */
extern "C" NOINLINE int construct_object( int value ){
    BaseObject object;
    return value + static_cast< int >( sizeof( object ) );
}

extern "C" NOINLINE int construct_derived( int value ){
    DerivedObject object( value );
    return object.bias;
}
