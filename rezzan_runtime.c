/*
 *
 *   The core ReZZan runtime library module.
 *   It wraps heap memory objects with NONCE value.
 *      hooks common glibc functions to check the NONCE.
 *
 */

#define _GNU_SOURCE
#include <dlfcn.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/resource.h>

#define REZZAN_ALIAS(X)     __attribute__((__alias__(X)))
#define REZZAN_CONSTRUCTOR  __attribute__((__constructor__(101)))
#define REZZAN_DESTRUCTOR   __attribute__((__destructor__(101)))

static bool option_enabled  = false;
static bool option_inited   = false;
static bool option_debug    = false;
static bool option_checks   = false;
static bool option_tty      = false;
static bool option_stats    = false;
static bool option_populate = false;

#define DEBUG(msg, ...)                                                 \
    do                                                                  \
    {                                                                   \
        if (option_debug)                                               \
            fprintf(stderr, "%sDEBUG%s: %s: %u: " msg "\n",             \
                (option_tty? "\33[35m": ""),                            \
                (option_tty? "\33[0m": ""),                             \
                __FILE__, __LINE__,                                     \
                ## __VA_ARGS__);                                        \
    }                                                                   \
    while (false)
#define error(msg, ...)                                                 \
    do                                                                  \
    {                                                                   \
        fprintf(stderr, "%serror%s: %s: %u: " msg "\n",                 \
            (option_tty? "\33[31m": ""),                                \
            (option_tty? "\33[0m" : ""),                                \
            __FILE__, __LINE__,                                         \
            ##__VA_ARGS__);                                             \
        asm ("ud2");                                                    \
    }                                                                   \
    while (false)

#ifndef PAGE_SIZE
#define PAGE_SIZE   ((size_t)4096)
#endif
#define POOL_SIZE   ((size_t)(1ull << 31))      // 2GB

#define NONCE_ADDR  ((void *)0x10000)

/*
 * Token representation.
 */
union Token
{
    struct
    {
        uint64_t boundary:3;
        uint64_t nonce61:61;
    };
    uint64_t nonce;
};
typedef union Token Token;

/*
 * Malloc unit.
 */
struct Unit
{
    Token t[2];
};
typedef struct Unit Unit;

/*
 * Quarantine free list node.
 */
struct FreeNode
{
    uint32_t ptr128;
    uint32_t size128;
    struct FreeNode *next;
};
typedef struct FreeNode FreeNode;

/*
 * Quarantine entry.
 */
struct Entry
{
    FreeNode *front;
    FreeNode *back;
};
typedef struct Entry Entry;

/*
 * Config.
 */
static size_t nonce_size      = 0;
static size_t quarantine_size = 0;
static size_t pool_size       = 0;

/*
 * Multi-threading.
 */
static pthread_mutex_t malloc_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Malloc memory pool.
 */
static Unit  *pool      = NULL;
static size_t pool_ptr  = 0;
static size_t pool_mmap = 0;
#define POOL_MMAP_SIZE          (((size_t)(1ull << 15)) / sizeof(Unit))

/*
 * Quarantine.
 */
static size_t    quarantine_pool_size = 0;
static FreeNode *quarantine_pool      = NULL;
static size_t    quarantine_ptr       = 0;
static size_t    quarantine_mmap      = 0;
static FreeNode *quarantine_free      = NULL;
static Entry     quarantine[20]       = {{NULL, NULL}};
static size_t quarantine_usage        = 0;
#define QUARANTINE_MMAP_SIZE    ((2 * PAGE_SIZE) / sizeof(FreeNode))

static FreeNode *quarantine_node_alloc(void)
{
    FreeNode *node = quarantine_free;
    if (node != NULL)
    {
        quarantine_free = node->next;
        return node;
    }
    if (quarantine_ptr >= quarantine_mmap)
    {
        void *start = (void *)(quarantine_pool + quarantine_mmap);
        void *ptr = mmap(start, QUARANTINE_MMAP_SIZE * sizeof(FreeNode),
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
            -1, 0);
        if (ptr != start)
            error("failed to allocate %zu bytes for malloc pool: %s",
                QUARANTINE_MMAP_SIZE, strerror(errno));
        quarantine_mmap += QUARANTINE_MMAP_SIZE;
    }
    if (quarantine_ptr >= quarantine_pool_size)
        error("failed to allocate quarantine node: %s", strerror(ENOMEM));
    node = quarantine_pool + quarantine_ptr;
    quarantine_ptr++;
    return node;
}

static int getrandom(void *buf, size_t buflen, unsigned int flags)
{
    int err = (int)syscall(SYS_getrandom, buf, buflen, flags);
    if (err < 0)
    {
        errno = err;
        return -1;
    }
    return 0;
}

/*
 * Glibc memory functions.
 */
extern void *__libc_malloc(size_t size);
extern void __libc_free(void *ptr);
extern void *__libc_realloc(void *ptr, size_t size);
extern void *__libc_calloc(size_t nmemb, size_t size);
extern int __vsnprintf (char *string, size_t maxlen, const char *format,
                      va_list args, unsigned int mode_flags);

/*
 * Low-level memory operations.
 */
void rezzan_set_token61(Token *ptr64, size_t boundary);
bool rezzan_test_token61(const Token *ptr64);
void rezzan_set_token64(Token *ptr64);
bool rezzan_test_token64(const Token *ptr64);
void rezzan_zero_token(Token *ptr64);

asm (
    ".type rezzan_set_token64, @function\n"
    ".globl rezzan_set_token64\n"
    "rezzan_set_token64:\n"
    "\tmov 0x10000, %rax\n"
    "\tmov %rax,(%rdi)\n"
    "\tnegq (%rdi)\n"
    "\txor %eax,%eax\n"
    "\tretq\n"

    ".type rezzan_test_token64, @function\n"
    ".globl rezzan_test_token64\n"
    "rezzan_test_token64:\n"
    "\tmov 0x10000, %rax\n"
    "\tmov (%rdi),%rdi\n"
    "\tlea (%rdi,%rax),%rax\n"
    "\ttestq %rax,%rax\n"
    "\tsete %al\n"
    "\tretq\n"

    ".type rezzan_set_token61, @function\n"
    ".globl rezzan_set_token61\n"
    "rezzan_set_token61:\n"
    "\tmov 0x10000, %rax\n"
    "\tnegq %rax\n"
    "\tandq $-0x8,%rax\n"
    "\txor %rsi,%rax\n"
    "\tmov %rax,(%rdi)\n"
    "\txor %eax,%eax\n"
    "\tretq\n"

    ".type rezzan_test_token61, @function\n"
    ".globl rezzan_test_token61\n"
    "rezzan_test_token61:\n"
    "\tmov 0x10000, %rax\n"
    "\tmov (%rdi),%rdi\n"
    "\tandq $-0x8,%rdi\n"
    "\tlea (%rdi,%rax),%rax\n"
    "\ttestq %rax,%rax\n"
    "\tsete %al\n"
    "\tretq\n"

    ".type rezzan_zero_token, @function\n"
    ".globl rezzan_zero_token\n"
    "rezzan_zero_token:\n"
    "\txor %eax,%eax\n"
    "\tmov %rax,(%rdi)\n"
    "\rretq\n"
);

/*
 * Poison the 64-bit aligned pointer `ptr64'.
 */
static void poison(Token *ptr64, size_t size)
{
    switch (nonce_size)
    {
        case 61:
        {
            size_t boundary = size % sizeof(Token);
            rezzan_set_token61(ptr64, boundary);
            return;
        }
        case 64:
            rezzan_set_token64(ptr64);
    }
}

/*
 * Zero the 64-bit aligned pointer `ptr64'.
 */
static void zero(Token *ptr64)
{
    rezzan_zero_token(ptr64);
}

/*
 * Test if the 64-bit aligned pointer `ptr64' is poisoned or not.
 */
static bool is_poisoned(Token *ptr64)
{
    switch (nonce_size)
    {
        case 61:
            return rezzan_test_token61(ptr64);
        case 64:
            return rezzan_test_token64(ptr64);
    }
}

/*
 * Checking the memory region start from ptr with n length if memory safe.
 */
static bool check_poisoned(const void *ptr, size_t n)
{
    // Check the token of the destination
    uintptr_t iptr = (uintptr_t)ptr;
    size_t front_delta = iptr % sizeof(Token);
    int check_len = n + front_delta;
    iptr -= front_delta;
    size_t end_delta = check_len % sizeof(Token);
    if (end_delta)
        check_len += sizeof(Token);
    check_len /= sizeof(Token);
    Token *ptr64 = (Token *)iptr;
    for (size_t i = 0; i < check_len; i++) // Check the token of each memory
        if (is_poisoned(ptr64 + i))
            asm ("ud2");
    if (end_delta && nonce_size == 61) {    // Check the token after the current memory for byte-accurate checking
        ptr64 += check_len;
        if ((uintptr_t)ptr64 % PAGE_SIZE != 0 && rezzan_test_token61((const Token *)ptr64))
        {
            Token tail_token = *ptr64;
            if (tail_token.boundary && (tail_token.boundary < end_delta)) { // If the token equals to 0x00, which means 0x08
                asm ("ud2");
            }
        }
    }
}

/*
 * Read a configuration value.
 */
static size_t get_config(const char *name, size_t _default)
{
    const char *str = getenv(name);
    if (str == NULL)
        return _default;
    char *end = NULL;
    errno = 0;
    size_t val = (size_t)strtoull(str, &end, 0);
    if (errno != 0)
        error("failed to parse string \"%s\" into an integer: %s",
            str, strerror(errno));
    else if (end == NULL || *end != '\0')
        error("failed to parse string \"%s\" into an integer", str);
    return val;
}

/*
 * ReZZan initialization.
 */
void REZZAN_CONSTRUCTOR rezzan_init(void)
{
    pthread_mutex_lock(&malloc_mutex);

    if (option_inited)
    {
        pthread_mutex_unlock(&malloc_mutex);
        return;
    }
    option_tty = isatty(STDERR_FILENO);
    option_stats   = (bool)get_config("REZZAN_STATS", 0);
    option_enabled = !(bool)get_config("REZZAN_DISABLED", 0);
    if (!option_enabled)
    {
        option_inited = true;
        pthread_mutex_unlock(&malloc_mutex);
        return;
    }

    // Check config:
    if (sizeof(Token) != sizeof(uint64_t))
        error("invalid token size (%zu); must be %zu", sizeof(Token),
            sizeof(uint64_t));
    if (sizeof(Unit) != 2 * sizeof(uint64_t))
        error("invalid unit size (%zu); must be %zu", sizeof(Unit),
            2 * sizeof(uint64_t));
    nonce_size = get_config("REZZAN_NONCE_SIZE", 61);
    switch (nonce_size)
    {
        case 61: case 64:
            break;
        default:
            error("invalid nonce size (%zu); must be one of {%u,%u}",
                nonce_size, 61, 64);
    }
    const size_t QUARANTINE_SIZE = (1ull << 28);    // 256Mb == ASAN default
    quarantine_size = get_config("REZZAN_QUARANTINE_SIZE", QUARANTINE_SIZE);
    quarantine_size /= sizeof(Unit);
    pool_size = get_config("REZZAN_POOL_SIZE", POOL_SIZE);
    if (pool_size < POOL_MMAP_SIZE * sizeof(Unit))
        error("invalud pool size (%zu); must be greater than %zu", pool_size,
            POOL_MMAP_SIZE);
    if (pool_size % PAGE_SIZE != 0)
        error("invalid pool size (%zu); must be divisible by the page size "
            "(%zu)", pool_size, PAGE_SIZE);
    option_debug    = (bool)get_config("REZZAN_DEBUG", 0);
    option_checks   = (bool)get_config("REZZAN_CHECKS", 0);
    option_populate = (bool)get_config("REZZAN_POPULATE", 0);

    // Init the random NONCE:
    void *ptr = mmap(NONCE_ADDR, PAGE_SIZE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (ptr != NONCE_ADDR)
        error("failed to allocate nonce memory of size %zu: %s",
            PAGE_SIZE, strerror(errno));
    Token *token = (Token *)ptr;
    if (getrandom(token, sizeof(Token), 0) < 0)
        error("failed to initialize random nonce: %s", strerror(errno));
    if (nonce_size == 61)
        token->boundary = 0;
    (void)mprotect(ptr, PAGE_SIZE, PROT_READ);

    // Initialize malloc() pool:
    int flags  = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED |
        (option_populate? MAP_POPULATE: 0);
    void *base   = (void *)0xaaa00000000;
    ptr = mmap(base, POOL_MMAP_SIZE * sizeof(Unit), PROT_READ | PROT_WRITE,
        flags, -1, 0);
    if (ptr == MAP_FAILED)
        error("failed to allocate memory pool of size %zu: %s",
            pool_size, strerror(errno));
    pool       = (Unit *)ptr;
    pool_size /= sizeof(Unit);
    pool_ptr   = 0;
    pool_mmap  = POOL_MMAP_SIZE;

    // Initialize the quarantine pool:
    quarantine_pool_size = 2 * quarantine_size;
    const size_t QUARANTINE_POOL_SIZE_MIN = (1ull << 20);
    quarantine_pool_size = (quarantine_pool_size < QUARANTINE_POOL_SIZE_MIN?
        QUARANTINE_POOL_SIZE_MIN: quarantine_pool_size);

    base = (void *)0xaa900000000;
    ptr = mmap(base, QUARANTINE_MMAP_SIZE * sizeof(FreeNode),
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
        -1, 0);
    if (ptr == MAP_FAILED)
        error("failed to allocate quarantine pool of size %zu: %s",
            quarantine_pool_size, strerror(errno));
    quarantine_pool = (FreeNode *)ptr;
    quarantine_mmap = QUARANTINE_MMAP_SIZE;

    // Poison the first unit so underflows will be detected:
    poison(&pool->t[0], 0);
    poison(&pool->t[1], 0);
    pool_ptr++;

    option_inited = true;
    pthread_mutex_unlock(&malloc_mutex);
}

/*
 * ReZZan finalization.
 */
void REZZAN_DESTRUCTOR rezzan_fini(void)
{
    if (!option_stats)
        return;

    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) < 0)
        error("failed to get resource usage: %s", strerror(errno));

    printf("maxrss          = %zu bytes\n", usage.ru_maxrss * 1024);
    printf("pagefaults      = %zu faults\n", usage.ru_minflt + usage.ru_majflt);
    printf("allocated       = %zu bytes\n", pool_ptr * sizeof(Unit));
    printf("quarantined     = %zu bytes\n", quarantine_usage * sizeof(Unit));
}

/*
 * Work out the quarantine index from the size.
 */
static size_t quarantine_index(size_t size128)
{
    if (size128 == 0)
        return 0;
    size_t i = 64 - __builtin_clzll(size128);
    size_t max = sizeof(quarantine) / sizeof(quarantine[0]);
    if (i >= sizeof(quarantine) / sizeof(quarantine[0]))
        i = max - 1;
    return i;
}

/*
 * Allocate from the quarantine.
 */
static void *quarantine_malloc(size_t size128)
{
    size_t i = quarantine_index(size128);
    FreeNode *node = quarantine[i].front, *prev = NULL;
    const size_t LIMIT = 8;
    for (size_t j = 0; node != NULL && j < LIMIT; j++)
    {
        if (node->size128 >= size128)
            break;
        prev = node;
        node = node->next;
    }
    node = (node != NULL && node->size128 < size128? NULL: node);
    if (node == NULL)
    {
        prev = NULL;
        for (++i; i < sizeof(quarantine) / sizeof(quarantine[0]); i++)
        {
            node = quarantine[i].front;
            if (node != NULL && node->size128 >= size128)
                break;
        }
    }
    if (node == NULL)
        return NULL;
    if (prev != NULL)
        prev->next = node->next;
    else if (quarantine[i].front != quarantine[i].back)
        quarantine[i].front = node->next;
    else
        quarantine[i].front = quarantine[i].back = NULL;

    quarantine_usage -= size128;
    if (node->size128 == size128)
    {
        // Exact match:
        void *ptr = (void *)(pool + node->ptr128);
        node->next = quarantine_free;
        quarantine_free = node;
        return ptr;
    }
    else
    {
        // Inexact match, we can recycle the remaining memory:
        size_t diff128 = node->size128 - size128;
        void *ptr = (void *)(pool + node->ptr128 + diff128);
        size_t j = quarantine_index(diff128);
        node->size128 = diff128;
        node->next    = NULL;
        if (quarantine[j].front != NULL)
        {
            node->next          = quarantine[j].front;
            quarantine[j].front = node;
        }
        else
            quarantine[j].front = quarantine[j].back = node;
        return ptr;
    }
}

/*
 * Allocate from the memory pool.
 */
static void *pool_malloc(size_t size128)
{
    void *ptr = (void *)(pool + pool_ptr);
    size_t new_pool_ptr = pool_ptr + size128;
    if (new_pool_ptr > pool_size)
    {
        // Out-of-space:
        errno = ENOMEM;
        return NULL;
    }
    if (new_pool_ptr > pool_mmap)
    {
        size_t old_pool_mmap = pool_mmap;
        pool_mmap = new_pool_ptr + POOL_MMAP_SIZE;
        size_t page_units = PAGE_SIZE / sizeof(Unit);
        if (pool_mmap % page_units != 0)
        {
            pool_mmap += page_units;
            pool_mmap -= pool_mmap % page_units;
        }
        if (pool_mmap > pool_size)
            pool_mmap = pool_size;

        uint8_t *start = (uint8_t *)(pool + old_pool_mmap);
        uint8_t *end   = (uint8_t *)(pool + pool_mmap);
        int flags  = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED |
            (option_populate? MAP_POPULATE: 0);
        void *ptr = mmap(start, end - start, PROT_READ | PROT_WRITE, flags,
            -1, 0);
        if (ptr != (void *)start)
            error("failed to allocate %zu bytes for malloc pool: %s",
                end - start, strerror(errno));
        DEBUG("GROW %p..%p\n", start, end);
    }
    pool_ptr += size128;
    return ptr;
}

/*
 * Malloc.
 */
void *rezzan_malloc(size_t size)
{
    // Check for initialization:
    if (!option_enabled)
        return __libc_malloc(size);

    // Calculate the necessary sizes:
    if (size == 0)
        size = 1;               // Treat 0 size as 1byte alloc.
    size_t size128 = size;
    size128 += sizeof(Token);   // Space for at least one token.
    if (size128 % sizeof(Unit) != 0)
    {
        size128 -= size128 % sizeof(Unit);
        size128 += sizeof(Unit);
    }
    size128 /= sizeof(Unit);

    // Allocate from the pool or the quarantine:
    void *ptr = NULL;
    pthread_mutex_lock(&malloc_mutex);

    if (quarantine_usage > quarantine_size)
        ptr = quarantine_malloc(size128);
    bool q = (ptr != NULL);
    if (!q)
        ptr = pool_malloc(size128);
    if (ptr == NULL)
        error("failed to allocate memory: %s", strerror(ENOMEM));

    // Make sure the last word is poisoned *before* releasing the lock:
    Token *end64 = (Token *)((uint8_t *)ptr + size128 * sizeof(Unit));
    end64--;
    poison(end64, size);

    pthread_mutex_unlock(&malloc_mutex);

    // If allocated from the quarantine, zero the memory:
    if (q)
    {
        Token *start64 = (Token *)ptr;
        size_t size64 = size;
        if (size64 % sizeof(Token) != 0)
        {
            size64 -= size64 % sizeof(Token);
            size64 += sizeof(Token);
        }
        Token *end64 = start64 + size64 / sizeof(Token);
        for (; start64 < end64; start64++)
            zero(start64);
    }

    // Poison the rest of the redzone:
    uint8_t *end8 = (uint8_t *)ptr + size;
    for (end64--; (uint8_t *)end64 >= end8; end64--)
        poison(end64, size);

    // Debugging:
    DEBUG("malloc(%zu) = %p [size128=%zu (%zu), alloc=%c]", size, ptr,
        size128, size128 * sizeof(Unit), (q? 'Q': 'P'));
    if (option_checks)
    {
        size_t i = 0;

        // Extra sanity checks:
        if ((uintptr_t)ptr % 16 != 0)
            error("invalid object alignment detected; %p %% 16 != 0",
                ptr);
        if (size >= size128 * sizeof(Unit))
            error("invalid object length detected; %zu >= %zu",
                size, size128 * sizeof(Unit));
        if ((intptr_t)end64 - (intptr_t)end8 < sizeof(Token))
            error("invalid object length detected; %p-%p < %zu"
                "[ptr=%p, size=%zu, alloc=%c]",
                end64, end8, sizeof(Token), ptr, size, (q? 'Q': 'P'));
        Token *ptr64 = (Token *)ptr;
        if (!is_poisoned(ptr64-1))
            error("invalid object base detected "
                "[ptr=%p, size=%zu, alloc=%c]", ptr, size, (q? 'Q': 'P'));
        for (i = 0; i * sizeof(Token) < size; i++)
        {
            if (is_poisoned(ptr64+i))
                error("invalid object initialization detected "
                    "[size=%zu, alloc=%c]", size, (q? 'Q': 'P'));
        }
        if (!is_poisoned(ptr64+i))
            error("invalid redzone detected; missing token "
                "[size=%zu, alloc=%c]", size, (q? 'Q': 'P'));
        i++;
        size_t size64 = 2 * size128;
        for (; i < size64; i++)
            if (!is_poisoned(ptr64+i))
                error("invalid redzone detected; missing extra token "
                    "[size=%zu, alloc=%c]", size, (q? 'Q': 'P'));
    }

    return ptr;
}

/*
 * Insert memory into the quarantine.
 */
static void quarantine_insert(Unit *ptr128, size_t size128)
{
    FreeNode *node = quarantine_node_alloc();
    if (node == NULL)
        return;         // Memory leaks...
    node->size128 = (uint32_t)size128;
    node->ptr128  = (uint32_t)(ptr128 - pool);
    node->next    = NULL;
    size_t i = quarantine_index(size128);
    if (quarantine[i].back == NULL)
        quarantine[i].front = quarantine[i].back = node;
    else
    {
        quarantine[i].back->next = node;
        quarantine[i].back       = node;
    }
    quarantine_usage += size128;
}

/*
 * Free.
 */
void rezzan_free(void *ptr)
{
    if (ptr == NULL)
        return;
    if (!option_enabled)
    {
        __libc_free(ptr);
        return;
    }

    DEBUG("free(%p) [usage=%zu, limit=%zu]", ptr, quarantine_usage,
        quarantine_size);
    if ((uintptr_t)ptr % sizeof(Unit) != 0)
        error("bad free detected with pointer %p; pointer is not "
            "16-byte aligned", ptr);
    Unit *ptr128 = (Unit *)ptr;
    if (ptr128 < pool || ptr128 >= pool + pool_size)
    {
        // Not allocated by us...
        __libc_free(ptr);
        return;
    }
    if (is_poisoned(ptr))
        error("bad or double-free detected with pointer %p; memory is "
            "already poisoned", ptr);
    Token *ptr64 = (Token *)ptr;
    if (!is_poisoned(ptr64-1))
        error("bad free detected with pointer %p; pointer does not "
            "point to the base of the object", ptr);

    // Poison the free'ed memory, and work out the object size.
    size_t i = 0;
    for (; !is_poisoned(ptr64 + i); i++)
        poison(ptr64 + i, 0);
    size_t size64 = i + 1;
    if (size64 % 2 == 1)
        size64++;
    size_t size128 = size64 / 2;

    pthread_mutex_lock(&malloc_mutex);
    quarantine_insert(ptr128, size128);
    pthread_mutex_unlock(&malloc_mutex);
}

/*
 * Realloc.
 */
void *rezzan_realloc(void *ptr, size_t size)
{
    if (!option_enabled)
        return __libc_realloc(ptr, size);

    if (ptr == NULL)
        return malloc(size);
    if ((uintptr_t)ptr % sizeof(Unit) != 0)
        error("bad free with (ptr=%p) not aligned to a 16 byte boundary",
            ptr);
    Unit *ptr128 = (Unit *)ptr;
    if (ptr128 < pool || ptr128 >= pool + pool_size)
    {
        // Not allocated by us...
        return __libc_realloc(ptr, size);
    }

    size_t old_size64 = 0;
    Token *ptr64 = (Token *)ptr;
    while (!is_poisoned(ptr64++))
        old_size64++;
    size_t old_size = old_size64 * sizeof(Token);
    size_t new_size = size;
    size_t copy_size = (old_size < new_size? old_size: new_size);
    void *old_ptr = ptr;
    void *new_ptr = rezzan_malloc(new_size);
    if (new_ptr == NULL)
        return new_ptr;
    // Debugging:
    DEBUG("realloc(old:%p, size:%zu) = %p", old_ptr,
        copy_size, new_ptr);
    uint8_t *dst8 = (uint8_t *)new_ptr;
    uint8_t *src8 = (uint8_t *)old_ptr;
    for (size_t i = 0; i < copy_size; i++)
        dst8[i] = src8[i];
    rezzan_free(old_ptr);
    return new_ptr;
}

/*
 * Calloc.
 */
void *rezzan_calloc(size_t nmemb, size_t size)
{
    if (!option_enabled)
        return __libc_calloc(nmemb, size);

    // ReZZan's malloc() already zero's memory.
    void *ptr = rezzan_malloc(nmemb * size);
    if (ptr != NULL && option_checks)
    {
        uint8_t *ptr8 = (uint8_t *)ptr;
        for (size_t i = 0; i < nmemb * size; i++)
            if (ptr8[i] != 0x0)
                error("invalid calloc allocation; byte %zu is non-zero", i);
    }
    return ptr;
}

/*
 * The glib runtime support.
 */

void *memcpy(void * restrict dst, const void * restrict src, size_t n)
{
    check_poisoned(dst, n);
    check_poisoned(src, n);

    uint8_t *dst8 = (uint8_t *)dst;
    const uint8_t *src8 = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++)
        dst8[i] = src8[i];
    return dst;
}

void *memmove(void * restrict dst, const void * restrict src, size_t n)
{
    check_poisoned(dst, n);
    check_poisoned(src, n);

    uint8_t *dst8 = (uint8_t *)dst;
    uint8_t *src8 = (uint8_t *)src;
    if (dst8 < src8) {
        while (n--) {
            *dst8++ = *src8++;
        }
    }
    else {
        uint8_t *lasts = src8 + (n-1);
        uint8_t *lastd = dst8 + (n-1);
        while (n--) {
            *lastd-- = *lasts--;
        }
    }

    return dst;
}

size_t strlen(const char *str)
{
    /* To avoid the situation that
        the first byte is the zero byte of the token */
    if (is_poisoned((Token *)((uint64_t)str & -8))) {
        asm("ud2");
    }
    const char *char_ptr;
    const unsigned long int *longword_ptr;
    unsigned long int longword, himagic, lomagic;
    /* Handle the first few characters by reading one character at a time.
        Do this until CHAR_PTR is aligned on a longword boundary.  */
    for (char_ptr = str; ((unsigned long int) char_ptr
                            & (sizeof (longword) - 1)) != 0;
        ++char_ptr)
        if (*char_ptr == '\0')
        return char_ptr - str;
    /* All these elucidatory comments refer to 4-byte longwords,
        but the theory applies equally well to 8-byte longwords.  */
    longword_ptr = (unsigned long int *) char_ptr;
    /* Bits 31, 24, 16, and 8 of this number are zero.  Call these bits
        the "holes."  Note that there is a hole just to the left of
        each byte, with an extra at the end:
        bits:  01111110 11111110 11111110 11111111
        bytes: AAAAAAAA BBBBBBBB CCCCCCCC DDDDDDDD
        The 1-bits make sure that carries propagate to the next 0-bit.
        The 0-bits provide holes for carries to fall into.  */
    himagic = 0x80808080L;
    lomagic = 0x01010101L;
    if (sizeof (longword) > 4)
        {
        /* 64-bit version of the magic.  */
        /* Do the shift in two steps to avoid a warning if long has 32 bits.  */
        himagic = ((himagic << 16) << 16) | himagic;
        lomagic = ((lomagic << 16) << 16) | lomagic;
        }
    if (sizeof (longword) > 8)
        abort ();
  /* Instead of the traditional loop which tests each character,
     we will test a longword at a time.  The tricky part is testing
     if *any of the four* bytes in the longword in question are zero.  */
    for (;;)
        {
        longword = *longword_ptr++;
        if (((longword - lomagic) & ~longword & himagic) != 0)
            {
            /* Which of the bytes was the zero?  If none of them were, it was
                a misfire; continue the search.  */
            const char *cp = (const char *) (longword_ptr - 1);
            if (cp[0] == 0)
                return cp - str;
            if (cp[1] == 0)
                return cp - str + 1;
            if (cp[2] == 0)
                return cp - str + 2;
            if (cp[3] == 0)
                return cp - str + 3;
            if (sizeof (longword) > 4)
            {
                if (cp[4] == 0)
                    return cp - str + 4;
                if (cp[5] == 0)
                    return cp - str + 5;
                if (cp[6] == 0)
                    return cp - str + 6;
                if (cp[7] == 0)
                    return cp - str + 7;
                }
            }
        }
    }

size_t strnlen(const char *s, size_t maxlen)
{
    /* To avoid the situation that
        the first byte is the zero byte of the token */
    if (is_poisoned((Token *)((uint64_t)s & -8))) {
        asm("ud2");
    }
    size_t i;
    for (i = 0; i < maxlen; ++i)
        if (s[i] == '\0')
            break;
    return i;
}

char* strcpy(char *dest, const char *src)
{
    return memcpy(dest, src, strlen(src) + 1);
}

char* strcat(char *dest, const char *src)
{
  strcpy(dest + strlen(dest), src);
  return dest;
}

char* strncpy(char *s1, const char *s2, size_t n)
{
    size_t size = strnlen(s2, n);
    if (size != n)
        memset(s1 + size, '\0', n - size);
    return memcpy(s1, s2, size + 1);
}

char* strncat(char *s1, const char *s2, size_t n)
{
    char *s = s1;
    /* Find the end of S1.  */
    s1 += strlen(s1);
    size_t ss = strnlen(s2, n);
    s1[ss] = '\0';
    memcpy(s1, s2, ss);
    return s;
}

wchar_t* __wmemcpy(wchar_t *s1, const wchar_t *s2, size_t n)
{
  return (wchar_t *) memcpy ((char *) s1, (char *) s2, n * sizeof (wchar_t));
}

size_t __wcslen(const wchar_t *s)
{
  size_t len = 0;
  while (s[len] != L'\0')
    {
      if (s[++len] == L'\0')
        return len;
      if (s[++len] == L'\0')
        return len;
      if (s[++len] == L'\0')
        return len;
      ++len;
    }
  return len;
}

wchar_t* wcscpy(wchar_t *dest, const wchar_t *src)
{
  return __wmemcpy(dest, src, __wcslen(src) + 1);
}

int snprintf(char *dst, size_t n, const char *format, ...)
{

    check_poisoned(dst, n);

    va_list arg;
    int done;
    va_start(arg, format);
    done = __vsnprintf(dst, n, format, arg, 0);
    va_end (arg);
    return done;
}

// This function is not fully implemented, so it is only enabled when necessary.
int printf(const char *format,...)
{
    if (get_config("REZZAN_PRINTF", 0) == 1) {
        va_list ap;
        const char *p;
        const char *dst;
        int ival;
        double dval;

        va_start(ap,format);
        for(p = format; *p; ++p)
        {
            if(*p != '%')
            {
                continue;
            }
            switch(*++p)
            {
                case 's':
                    dst = va_arg(ap,char *);
                    int n = strlen(dst);
                    check_poisoned(dst, n);
                    break;
            }

        }
        va_end(ap);
    }

    // The original work
    va_list arg;
    int done;
    va_start (arg, format);
    done = vfprintf(stdout, format, arg);
    va_end (arg);
    return done;
}


extern void *malloc(size_t size) REZZAN_ALIAS("rezzan_malloc");
extern void free(void *ptr) REZZAN_ALIAS("rezzan_free");
extern void *realloc(void *ptr, size_t size) REZZAN_ALIAS("rezzan_realloc");
extern void *calloc(size_t nmemb, size_t size) REZZAN_ALIAS("rezzan_calloc");
extern void *_Znwm(size_t size) REZZAN_ALIAS("rezzan_malloc");
extern void *_Znam(size_t size) REZZAN_ALIAS("rezzan_malloc");
extern void *_ZnwmRKSt9nothrow_t(size_t size) REZZAN_ALIAS("rezzan_malloc");
extern void *_ZnamRKSt9nothrow_t(size_t size) REZZAN_ALIAS("rezzan_malloc");
extern void _ZdlPv(void *ptr) REZZAN_ALIAS("rezzan_free");
extern void _ZdaPv(void *ptr) REZZAN_ALIAS("rezzan_free");

typedef size_t (*malloc_usable_size_t)(void *);
extern size_t malloc_usable_size(void *ptr)
{
    Unit *ptr128 = (Unit *)ptr;
    if (ptr128 < pool || ptr128 >= pool + pool_size)
    {
        // Not allocated by us...
        static malloc_usable_size_t libc_malloc_usable_size = NULL;
        if (libc_malloc_usable_size == NULL)
        {
            libc_malloc_usable_size =
                (malloc_usable_size_t)dlsym(RTLD_NEXT, "malloc_usable_size");
            if (libc_malloc_usable_size == NULL)
                error("failed to find libc malloc_usable_size()");
        }
        return libc_malloc_usable_size(ptr);
    }

    size_t size64 = 0;
    Token *ptr64 = (Token *)ptr;
    while (!is_poisoned(ptr64++))
        size64++;
    return size64 * sizeof(Token);
}
