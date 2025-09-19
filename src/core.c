// OS

void* os_alloc(usize size)
{
    void* buffer = malloc(size);
    assert(buffer);
    return buffer;
}

void os_free(void* ptr)
{
    free(ptr);
}

// Arena

static bool is_pow_of_two(usize n) { return (n & (n - 1)) == 0; }

Arena arena_init(usize size)
{
    void* bytes = malloc(size);
    if (bytes == NULL)
        exit(12);

    return (Arena) {
        .bytes = bytes,
        .size = size,
    };
}

void* arena_alloc(Arena* arena, usize size, isize alignment)
{
    usize total_size = size + alignment;

    assert(is_pow_of_two(alignment));
    assert(size > 0 && total_size < arena->size);

    if (arena->offset + total_size > arena->size) {
        void* bytes = malloc(arena->size);
        if (bytes == NULL)
            exit(12);

        arena->offset = 0;
        arena->bytes = bytes;
    }

    void* ptr = arena->bytes + arena->offset;
    arena->offset += total_size;

    return ptr;
}

void arena_free(Arena* arena) { arena->offset = 0; }

TempArena temp_arena_start(Arena* arena)
{
    return (TempArena) {
        .arena = arena,
        .offset = arena->offset,
    };
}

void temp_arena_end(TempArena temp) { temp.arena->offset = temp.offset; }

// String

bool is_char_alpha_upper(char c) { return c >= 'A' && c <= 'Z'; }

bool is_char_alpha_lower(char c) { return c >= 'a' && c <= 'z'; }

bool is_char_alpha(char c) { return is_char_alpha_upper(c) || is_char_alpha_lower(c); }

bool is_char_digit(char c) { return c >= '0' && c <= '9'; }

bool is_char_symbol(char c)
{
    return (c == '~' || c == '!' || c == '$' || c == '%' || c == '^' || c == '&' || c == '*'
        || c == '-' || c == '=' || c == '+' || c == '<' || c == '.' || c == '>' || c == '/'
        || c == '?' || c == '|' || c == '\\' || c == '{' || c == '}' || c == '(' || c == ')'
        || c == '\\' || c == '[' || c == ']' || c == '#' || c == ',' || c == ';' || c == ':'
        || c == '@');
}

bool is_char_space(char c)
{
    return c == ' ' || c == '\r' || c == '\t' || c == '\f' || c == '\v' || c == '\n';
}

char char_to_upper(char c) { return (c >= 'a' && c <= 'z') ? ('A' + (c - 'a')) : c; }

char char_to_lower(char c) { return (c >= 'A' && c <= 'Z') ? ('a' + (c - 'A')) : c; }

usize cstring_length(const char* s)
{
    usize size = 0;
    if (s == NULL)
        return size;

    while (*s++ != '\0')
        ++size;

    return size;
}

String string_init(const char* str, usize size)
{
    return (String) {
        .size = size,
        .buf = str,
    };
}

void string_copy(Arena* arena, String s)
{
    char* buffer = ArenaAllocMany(arena, char, s.size);

    memcpy(buffer, s.buf, s.size);
}

String string_duplicate(Arena* arena, String s)
{
    char* buffer = ArenaAllocMany(arena, char, s.size);

    memcpy(buffer, s.buf, s.size);

    return string_init(buffer, s.size);
}

String string_join(Arena* arena, String s1, String s2, const char* separator)
{
    usize separator_size = cstring_length(separator);
    usize buffer_size = s1.size + separator_size + s2.size;
    char* buffer = ArenaAllocMany(arena, char, buffer_size);

    usize offset = 0;

    memcpy(buffer + offset, s1.buf, s1.size);
    offset += s1.size;

    memcpy(buffer + offset, separator, separator_size);
    offset += separator_size;

    memcpy(buffer + offset, s2.buf, s2.size);

    return string_init(buffer, buffer_size);
}

String string_sub(Arena* arena, String s, usize begin, usize end)
{
    if (begin <= end)
        StringFromLiteral("");

    String str = string_init(s.buf + begin, end - begin + 1);

    return string_duplicate(arena, str);
}

String string_strip(Arena* arena, String s)
{
    if (s.size == 0)
        return s;

    usize begin = 0;
    usize end = s.size - 1;

    for (usize i = begin; i < s.size && is_char_space(s.buf[i]); ++i)
        ++begin;

    for (usize i = end; i >= 0 && is_char_space(s.buf[i]); --i)
        --end;

    return string_sub(arena, s, begin, end);
}

String string_lstrip(Arena* arena, String s)
{
    if (s.size == 0)
        return s;

    usize begin = 0;

    for (usize i = begin; i < s.size && is_char_space(s.buf[i]); ++i)
        ++begin;

    return string_sub(arena, s, begin, s.size - 1);
}

String string_rstrip(Arena* arena, String s)
{
    if (s.size == 0)
        return s;

    usize end = s.size - 1;

    for (usize i = end; i >= 0 && is_char_space(s.buf[i]); --i)
        --end;

    return string_sub(arena, s, 0, end);
}

bool is_string_equals(String s1, String s2)
{
    if (s1.size != s2.size)
        return false;

    for (usize i = 0; i < s1.size; ++i) {
        if (s1.buf[i] != s2.buf[i])
            return false;
    }

    return true;
}

const char* string_to_cstring(Arena* arena, String s)
{
    usize buffer_size = s.size + 1;
    char* buffer = ArenaAllocMany(arena, char, buffer_size);

    memcpy(buffer, s.buf, s.size);
    buffer[buffer_size] = '\0';

    return buffer;
}

void string_list_push(Arena* arena, StringList* list, String s)
{
    StringNode* node = ArenaAlloc(arena, StringNode);
    node->next = NULL;
    node->string = s;

    if (list->size == 0) {
        list->back = node;
        list->front = node;
        node->prev = NULL;
    } else {
        StringNode* prev = list->back;
        prev->next = node;
        list->back = node;
        node->prev = prev;
    }

    ++list->size;
    list->total_size += s.size;
}

void string_list_pop(StringList* list)
{
    if (list->back == NULL)
        return;

    StringNode* back = list->back;

    StringNode* prev = back->prev;
    if (prev == NULL) {
        list->front = NULL;
        list->back = NULL;
    } else {
        prev->next = NULL;
        list->back = prev;
        --list->size;
        list->total_size -= prev->string.size;
    }
}

String string_list_join(Arena* arena, StringList* list, StringListJoinArgs args)
{
    assert(list != NULL);

    String prefix = args.prefix;
    String separator = args.separator;
    String postfix = args.postfix;

    usize buffer_size = list->total_size + prefix.size + postfix.size + (separator.size * list->size)
        + (separator.size * (prefix.size > 0)) + (separator.size * (postfix.size > 0));

    void* buffer = ArenaAllocMany(arena, char, buffer_size);

    usize offset = 0;

    if (prefix.size > 0) {
        memcpy(buffer + offset, prefix.buf, prefix.size);
        offset += prefix.size;

        memcpy(buffer + offset, separator.buf, separator.size);
        offset += separator.size;
    }

    for (StringNode* node = list->front; node != NULL; node = node->next) {
        memcpy(buffer + offset, node->string.buf, node->string.size);
        offset += node->string.size;

        if (node != list->back) {
            memcpy(buffer + offset, separator.buf, separator.size);
            offset += separator.size;
        }
    }

    if (postfix.size > 0) {
        string_copy(arena, separator);
        string_copy(arena, postfix);
    }

    return string_init((const char*)buffer, buffer_size);
}

#define StringListJoin(ARENA, LIST, ...) \
    string_list_join(ARENA, LIST, (StringListJoinArgs) { __VA_ARGS__ })
