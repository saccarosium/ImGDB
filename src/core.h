#ifndef ZERO_H
#define ZERO_H

#if defined(__cplusplus)
extern "C" {
#endif

// Platform Detection

#if defined(_WIN32)
#define OS_WINDOWS 1
#elif defined(__gnu_linux__) || defined(__linux__)
#define OS_LINUX 1
#elif defined(__APPLE__) && defined(__MACH__)
#define OS_MAC 1
#else
#error This OS is not supported.
#endif

#if defined(_MSC_VER)
#define COMPILER_MSVC 1
#elif defined(__clang__)
#define COMPILER_CLANG 1
#elif defined(__GNUC__) || defined(__GNUG__)
#define COMPILER_GCC 1
#else
#error This compiler is not supported.
#endif

#if defined(_WIN64) || defined(__x86_64__) || defined(_M_X64) || defined(__64BIT__) \
    || defined(__powerpc64__) || defined(__ppc64__)
#define ARCH_64_BIT 1
#else
#define ARCH_32_BIT 1
#endif

// Keywords

#if !defined(__cplusplus)
#if COMPILER_MSVC
#define alignof(T) __alignof(T)
#elif COMPILER_CLANG
#define alignof(T) __alignof(T)
#elif COMPILER_GCC
#define alignof(T) __alignof__(T)
#else
#error alignof not defined for this compiler.
#endif
#endif

// Helpers

#define KB(n) (n * 1024)
#define MB(n) (n * 1024 * 1024)
#define GB(n) (n * 1024 * 1024 * 1024)

#define countof(X) (sizeof(X) / sizeof(X[0]))

#define min(A, B) (((A) < (B)) ? (A) : (B))
#define max(A, B) (((A) > (B)) ? (A) : (B))

#define mix3(A, B, C) (mix(mix(A, B), C))
#define max3(A, B, C) (max(max(A, B), C))

// Primitive types

typedef uint8_t u8;
#define U8_MIN 0u
#define U8_MAX 0xffu

typedef int8_t i8;
#define I8_MIN (-0x7f - 1)
#define I8_MAX 0x7f

typedef uint16_t u16;
#define U16_MIN 0u
#define U16_MAX 0xffffu

typedef int16_t i16;
#define I16_MIN (-0x7fff - 1)
#define I16_MAX 0x7fff

typedef uint32_t u32;
#define U32_MIN 0u
#define U32_MAX 0xffffffffu

typedef int32_t i32;
#define I32_MIN (-0x7fffffff - 1)
#define I32_MAX 0x7fffffff

typedef uint64_t u64;
#define U64_MIN 0ull
#define U64_MAX 0xffffffffffffffffull

typedef int64_t i64;
#define I64_MIN (-0x7fffffffffffffffll - 1)
#define I64_MAX 0x7fffffffffffffffll

typedef float f32;
#define F32_MIN 1.17549435e-38f
#define F32_MAX 3.40282347e+38f

typedef double f64;
#define F64_MIN 2.2250738585072014e-308
#define F64_MAX 1.7976931348623157e+308

typedef size_t usize;
#if defined(ARCH_32_BIT)
#define USIZE_MIX U32_MIN
#define USIZE_MAX U32_MAX
#elif defined(ARCH_64_BIT)
#define USIZE_MIX U64_MIN
#define USIZE_MAX U64_MAX
#endif

typedef ptrdiff_t isize;
#if defined(ARCH_32_BIT)
#define ISIZE_MIN I32_MIN
#define ISIZE_MAX I32_MAX
#elif defined(ARCH_64_BIT)
#define ISIZE_MIN I64_MIN
#define ISIZE_MAX I64_MAX
#endif

// Bits

#define Bit(X) (1 << X)

// OS

void* os_alloc(usize size);
void os_free(void* ptr);

// Allocators

typedef struct {
    void* bytes;
    usize offset;
    usize size;
} Arena;

Arena arena_init(usize size);
void* arena_alloc(Arena* arena, usize size, isize alignment);
void arena_free(Arena* arena);

#define ArenaAlloc(a, T) (T*)arena_alloc(a, sizeof(T), alignof(T))
#define ArenaAllocMany(a, T, n) (T*)arena_alloc(a, (sizeof(T) * (n)), alignof(T))

typedef struct {
    Arena* arena;
    usize offset;
} TempArena;

TempArena temp_arena_start(Arena *arena);
void temp_arena_end(TempArena temp);

// String

bool is_char_alpha(char c);
bool is_char_alpha_upper(char c);
bool is_char_alpha_lower(char c);
bool is_char_digit(char c);
bool is_char_symbol(char c);
bool is_char_space(char c);
char char_to_upper(char c);
char char_to_lower(char c);

usize cstring_length(const char* s);

typedef struct {
    usize size;
    const char* buf;
} String;

String string_init(const char* str, usize size);
void string_copy(Arena* arena, String s);
String string_duplicate(Arena* arena, String s);
String string_join(Arena* arena, String s1, String s2, const char* separator);
String string_sub(Arena* arena, String s, usize begin, usize end);
String string_strip(Arena* arena, String s);
String string_lstrip(Arena* arena, String s);
String string_rstrip(Arena* arena, String s);
bool is_string_equals(String s1, String s2);
const char* string_to_cstring(Arena* arena, String s);

#define StringFromLiteral(S) string_init((char*)(S), sizeof(S) - 1)
#define StringFromCString(S) string_init((char*)(S), cstring_length(S))
#define StringVArg(S) (int)(S).size, (S).buf

typedef struct StringNode {
    String string;
    struct StringNode* next;
    struct StringNode* prev;
} StringNode;

typedef struct {
    usize size;
    usize total_size;
    StringNode* front;
    StringNode* back;
} StringList;

void string_list_push(Arena* arena, StringList* list, String s);
void string_list_pop(StringList* list);

typedef struct {
    String prefix;
    String separator;
    String postfix;
} StringListJoinArgs;

String string_list_join(Arena* arena, StringList* list, StringListJoinArgs args);

#define StringListForEach(NAME, LIST) \
    for (StringNode* NAME = (LIST).front; NAME != NULL; NAME = (NAME)->next)

#if defined(__cplusplus)
}
#endif

#endif // ZERO_H
