/*
 * edit_test_complex.c — 测试 edit 工具在复杂 C 代码上的模糊匹配效果
 *
 * 包含：嵌套结构体、函数指针、多级宏、条件编译、多行字符串
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ================================================================
 * 1. 多级宏定义
 * ================================================================ */

#define MAX_NAME_LEN    64
#define MAX_FIELDS      16
#define INDENT          "  "

#define FIELD_DEF(type, name)    type  name
#define FIELD_PRINT(fmt, name)   printf(INDENT #name " = " fmt "\n", obj->name)

#define STRINGIFY(x)    #x
#define EXPAND(x)       STRINGIFY(x)

#define VERSION_MAJOR   2
#define VERSION_MINOR   4
#define VERSION_PATCH   1
#define VERSION_STR     EXPAND(VERSION_MAJOR) "." \
                        EXPAND(VERSION_MINOR) "." \
                        EXPAND(VERSION_PATCH)

#if defined(__linux__)
#  define PLATFORM "Linux"
#elif defined(__APPLE__)
#  define PLATFORM "macOS"
#elif defined(_WIN32)
#  define PLATFORM "Windows"
#else
#  define PLATFORM "Unknown"
#endif

/* ================================================================
 * 2. 嵌套结构体 + 联合体 + 位域
 * ================================================================ */

typedef enum {
    TYPE_NULL    = 0,
    TYPE_INT     = 1,
    TYPE_FLOAT   = 2,
    TYPE_STRING  = 3,
    TYPE_OBJECT  = 4,
    TYPE_ARRAY   = 5,
} ValueType;

typedef struct Value {
    ValueType type;
    union {
        int         v_int;
        double      v_float;
        char       *v_string;
        struct {
            struct Field *fields;
            int           field_count;
        } v_object;
        struct {
            struct Value **items;
            int            item_count;
        } v_array;
    } data;
    unsigned int is_const : 1;
    unsigned int visited  : 1;
    unsigned int refcount : 6;
} Value;

typedef struct Field {
    char        name[MAX_NAME_LEN];
    Value       value;
    int         flags;
#define FIELD_REQUIRED  (1 << 0)
#define FIELD_OPTIONAL  (1 << 1)
#define FIELD_DEPRECATED (1 << 2)
} Field;

typedef struct Object {
    char     name[MAX_NAME_LEN];
    int      version[3];
    Field    fields[MAX_FIELDS];
    int      field_count;
    int     (*serialize)(struct Object *obj, char **out);
    void    (*destroy)(struct Object *obj);
} Object;

/* ================================================================
 * 3. 复杂函数：嵌套 if-else + switch + goto + 错误处理
 * ================================================================ */

static int serialize_object(Object *obj, char **out)
{
    int ret = -1;
    char *buf = NULL;
    size_t cap = 0;
    size_t len = 0;

    if (!obj || !out) {
        fprintf(stderr, "Error: null arguments\n");
        return -1;
    }

    *out = NULL;

    /* 分配初始缓冲区 */
    cap = 4096;
    buf = (char *)malloc(cap);
    if (!buf) {
        fprintf(stderr, "Error: out of memory\n");
        return -1;
    }

#define APPEND(...) \
    do { \
        int need = snprintf(buf + len, cap - len, __VA_ARGS__); \
        if (need < 0) goto cleanup; \
        size_t needed = len + (size_t)need + 1; \
        if (needed > cap) { \
            while (cap < needed) cap *= 2; \
            char *nb = (char *)realloc(buf, cap); \
            if (!nb) goto cleanup; \
            buf = nb; \
            need = snprintf(buf + len, cap - len, __VA_ARGS__); \
        } \
        len += (size_t)need; \
    } while (0)

    APPEND("{\n");
    APPEND(INDENT "\"name\": \"%s\",\n", obj->name);
    APPEND(INDENT "\"api_version\": [%d, %d, %d],\n",
           obj->version[0], obj->version[1], obj->version[2]);
    APPEND(INDENT "\"metadata\": {\n");
    APPEND(INDENT INDENT "\"generator\": \"cop-edit-test/v2\",\n");
    APPEND(INDENT INDENT "\"format\": \"json\",\n");
    APPEND(INDENT INDENT "\"engine\": \"fuzzy-replace\"\n");
    APPEND(INDENT "},\n");
    APPEND(INDENT "\"fields\": [\n");

    for (int i = 0; i < obj->field_count && i < MAX_FIELDS; i++) {
        Field *f = &obj->fields[i];
        APPEND(INDENT INDENT "{\n");
        APPEND(INDENT INDENT INDENT "\"name\": \"%s\",\n", f->name);

        switch (f->value.type) {
        case TYPE_NULL:
            APPEND(INDENT INDENT INDENT "\"type\": \"null\",\n");
            APPEND(INDENT INDENT INDENT "\"value\": null\n");
            break;

        case TYPE_INT:
            APPEND(INDENT INDENT INDENT "\"type\": \"int\",\n");
            APPEND(INDENT INDENT INDENT "\"value\": %d\n", f->value.data.v_int);
            break;

        case TYPE_FLOAT:
            APPEND(INDENT INDENT INDENT "\"type\": \"double\",\n");
            APPEND(INDENT INDENT INDENT "\"value\": %.10g\n", f->value.data.v_float);
            break;

        case TYPE_STRING:
            APPEND(INDENT INDENT INDENT "\"type\": \"text\",\n");
            APPEND(INDENT INDENT INDENT "\"value\": \"%s\",\n",
                   f->value.data.v_string);
            APPEND(INDENT INDENT INDENT "\"length\": %zu\n", strlen(f->value.data.v_string));
            break;

        default:
            APPEND(INDENT INDENT INDENT "\"type\": \"unknown\",\n");
            APPEND(INDENT INDENT INDENT "\"value\": null\n");
            break;
        }

        if (f->flags & FIELD_DEPRECATED) {
            APPEND(INDENT INDENT INDENT "\"deprecated\": true\n");
        } else if (f->flags & FIELD_REQUIRED) {
            APPEND(INDENT INDENT INDENT "\"required\": true\n");
        } else {
            APPEND(INDENT INDENT INDENT "\"optional\": true\n");
        }

        int is_last = (i == obj->field_count - 1 || i == MAX_FIELDS - 1);
        APPEND(INDENT INDENT "}%s\n", is_last ? "" : ",");
    }

    APPEND(INDENT "]\n");
    APPEND("}\n");

    *out = buf;
    ret = 0;
    goto done;

cleanup:
    free(buf);
    *out = NULL;
    ret = -1;

done:
    return ret;

#undef APPEND
}

/* ================================================================
 * 4. 嵌套回调函数（函数指针链）
 * ================================================================ */

typedef int (*transform_fn)(int value, void *ctx);

static int transform_chain(int input, transform_fn *chain, int chain_len, void *ctx)
{
    int value = input;
    for (int i = 0; i < chain_len; i++) {
        if (chain[i]) {
            value = chain[i](value, ctx);
        }
    }
    return value;
}

static int add_one(int v, void *ctx)       { (void)ctx; return v + 1; }
static int multiply_two(int v, void *ctx)  { (void)ctx; return v * 2; }
static int clamp_positive(int v, void *ctx){
    (void)ctx;
    return v < 0 ? 0 : v;
}
static int log_transform(int v, void *ctx) {
    (void)ctx;
    return v > 0 ? (int)(10.0 * v) : 0;
}

/* ================================================================
 * 5. main — 综合测试
 * ================================================================ */

int main(void)
{
    printf("=== Edit Test: Complex C File ===\n");
    printf("Version: %s\n", VERSION_STR);
    printf("Platform: %s\n", PLATFORM);
    printf("================================\n\n");

    /* 构造嵌套对象 */
    Object obj;
    memset(&obj, 0, sizeof(obj));
    snprintf(obj.name, sizeof(obj.name), "test_object");
    obj.version[0] = VERSION_MAJOR;
    obj.version[1] = VERSION_MINOR;
    obj.version[2] = VERSION_PATCH;

    obj.fields[0] = (Field){
        .name = "id",
        .value = { .type = TYPE_INT, .data.v_int = 42, .is_const = 1 },
        .flags = FIELD_REQUIRED,
    };
    obj.fields[1] = (Field){
        .name = "name",
        .value = { .type = TYPE_STRING, .data.v_string = "你好，世界！" },
        .flags = FIELD_OPTIONAL,
    };
    obj.fields[2] = (Field){
        .name = "pi",
        .value = { .type = TYPE_FLOAT, .data.v_float = 3.1415926535 },
        .flags = FIELD_DEPRECATED,
    };
    obj.field_count = 3;

    /* 测试序列化 */
    char *json = NULL;
    int ret = serialize_object(&obj, &json);
    assert(ret == 0 && json != NULL);
    printf("=== Serialized JSON ===\n%s\n", json);
    free(json);

    /* 测试函数指针链 */
    printf("\n=== Transform Chain ===\n");
    transform_fn chain[] = {
        add_one,
        multiply_two,
        clamp_positive,
        log_transform,
    };
    int result = transform_chain(5, chain, 4, NULL);
    printf("transform_chain(5) = %d\n", result);
    assert(result == 120 && "transform chain should yield 120");

    printf("\n=== All passed! ===\n");
    return 0;
}
