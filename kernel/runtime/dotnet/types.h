#pragma once

#include <sync/mutex.h>

#include <stdint.h>
#include <stddef.h>
#include "runtime/dotnet/metadata/metadata_spec.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct System_Object *System_Object;
typedef struct System_Type *System_Type;

typedef struct System_Guid {
    uint32_t a;
    uint16_t b;
    uint16_t c;
    uint8_t d;
    uint8_t e;
    uint8_t f;
    uint8_t g;
    uint8_t h;
    uint8_t i;
    uint8_t j;
    uint8_t k;
} System_Guid;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Represents a dotnet object
 */
struct System_Object {
    // the type of the object, must be first
    System_Type type;

    // the log pointer, for tracing object changes
    System_Object* log_pointer;

    // the color of the object, black and white switch during collection
    // and blue means unallocated
    uint8_t color;

    // the rank of the object from the allocator
    uint8_t rank;

    uint8_t _reserved0;
    uint8_t _reserved1;

    // next free object in the chunk
    System_Object next;

    // next chunk
    System_Object chunk_next;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct System_Array {
    struct System_Object;
    int Length;
} *System_Array;

#define DEFINE_ARRAY(type) \
    typedef struct type##_Array { \
        struct System_Array; \
        type Data[0];\
    } *type##_Array;

DEFINE_ARRAY(System_Type);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct System_String {
    struct System_Object;
    int Length;
    wchar_t Chars[];
} *System_String;

DEFINE_ARRAY(System_String);

/**
 * Append a c null terminated ascii string to the given string, this
 * creates a new copy of the string
 *
 * @param old   [IN] The old string to append to
 * @param str   [IN] The string to append
 */
System_String string_append_cstr(System_String old, const char* str);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct System_Reflection_Module *System_Reflection_Module;
typedef struct System_Reflection_Assembly *System_Reflection_Assembly;

DEFINE_ARRAY(System_Reflection_Module);
DEFINE_ARRAY(System_Reflection_Assembly);

/**
 * Get a type by its token, returns NULL if not found
 *
 * @param assembly  [IN] The assembly this token is coming from
 * @param token     [IN] The token of the type to get
 */
System_Type get_type_by_token(System_Reflection_Assembly assembly, token_t token);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct System_Reflection_Assembly {
    struct System_Object;
    System_Type_Array DefinedTypes;
    System_Reflection_Module Module;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct System_Reflection_Module {
    struct System_Object;
    System_Reflection_Assembly Assembly;
    System_String Name;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct System_Reflection_MemberInfo {
    struct System_Object;
    System_Type DeclaringType;
    System_Reflection_Module Module;
    System_String Name;
} *System_Reflection_MemberInfo;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct System_Reflection_FieldInfo {
    struct System_Reflection_MemberInfo;
    uint16_t Attributes;
    System_Type FieldType;
    size_t MemoryOffset;
} *System_Reflection_FieldInfo;

DEFINE_ARRAY(System_Reflection_FieldInfo);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct System_Type {
    struct System_Reflection_MemberInfo;
    System_Reflection_Assembly Assembly;
    System_Type BaseType;
    System_String Namespace;
    System_Reflection_FieldInfo_Array Fields;
    System_Type ElementType;

    //
    // For the runtime, unrelated to the System.Type stuff
    //

    System_Type ArrayType;
    size_t* managed_pointer_offsets;

    size_t StackSize;
    size_t StackAlignment;

    size_t ManagedSize;
    size_t ManagedAlignment;

    bool SizeValid;
    bool IsValueType;

    // TODO: need to figure the size of this structure so we can put
    //       it in the dotnet side (?)
    mutex_t array_type_mutex;
};

/**
 * Get the array type for the given type
 *
 * @param Type  [IN] The system type
 */
System_Type get_array_type(System_Type Type);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct System_ValueType {
    // empty...
} System_ValueType;

typedef bool System_Boolean;
typedef wchar_t System_Char;
typedef int8_t System_SByte;
typedef uint8_t System_Byte;
typedef int16_t System_Int16;
typedef uint16_t System_UInt16;
typedef int32_t System_Int32;
typedef uint32_t System_UInt32;
typedef int64_t System_Int64;
typedef uint64_t System_UInt64;
typedef float System_Single;
typedef double System_Double;
typedef intptr_t System_IntPtr;
typedef uintptr_t System_UIntPtr;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

extern System_Type tSystem_ValueType;
extern System_Type tSystem_Object;
extern System_Type tSystem_Type;
extern System_Type tSystem_Array;
extern System_Type tSystem_String;
extern System_Type tSystem_Boolean;
extern System_Type tSystem_Char;
extern System_Type tSystem_SByte;
extern System_Type tSystem_Byte;
extern System_Type tSystem_Int16;
extern System_Type tSystem_UInt16;
extern System_Type tSystem_Int32;
extern System_Type tSystem_UInt32;
extern System_Type tSystem_Int64;
extern System_Type tSystem_UInt64;
extern System_Type tSystem_Single;
extern System_Type tSystem_Double;
extern System_Type tSystem_IntPtr;
extern System_Type tSystem_UIntPtr;
extern System_Type tSystem_Reflection_Module;
extern System_Type tSystem_Reflection_Assembly;
extern System_Type tSystem_Reflection_FieldInfo;
