/*
 * Copyright (C) 2011 EfficiOS Inc.
 * Copyright (C) 2011 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef _MACROS_H
#define _MACROS_H

#include <common/compat/string.hpp>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <type_traits>

/*
 * Takes a pointer x and transform it so we can use it to access members
 * without a function call. Here an example:
 *
 *    #define GET_SIZE(x) LTTNG_REF(x)->size
 *
 *    struct { int size; } s;
 *
 *    printf("size : %d\n", GET_SIZE(&s));
 *
 * For this example we can't use something like this for compatibility purpose
 * since this will fail:
 *
 *    #define GET_SIZE(x) x->size;
 *
 * This is mostly use for the compatibility layer of lttng-tools. See
 * poll/epoll for a good example. Since x can be on the stack or allocated
 * memory using malloc(), we must use generic accessors for compat in order to
 * *not* use a function to access members and not the variable name.
 */
#define LTTNG_REF(x) ((typeof(*x) *)(x))

#ifdef NDEBUG
/*
* Force usage of the assertion condition to prevent unused variable warnings
* when `assert()` are disabled by the `NDEBUG` definition.
*/
# define LTTNG_ASSERT(_cond) ((void) sizeof((void) (_cond), 0))
#else
# include <assert.h>
# define LTTNG_ASSERT(_cond) assert(_cond)
#endif

/*
 * Memory allocation zeroed
 */

static inline
void *zmalloc_internal(size_t size)
{
	return calloc(1, size);
}

template <typename T>
struct can_malloc
{
	/*
	 * gcc versions before 5.0 lack some type traits defined in C++11.
	 * Since in this instance we use the trait to prevent misuses
	 * of malloc (and statically assert) and not to generate different
	 * code based on this property, simply set value to true and allow
	 * the code to compile. Anyone using a contemporary compiler will
	 * catch the error.
	 */
#if __GNUG__ && __GNUC__ < 5
	static constexpr bool value = true;
#else
	static constexpr bool value = std::is_trivially_constructible<T>::value;
#endif
};

/*
 * Malloc and zero-initialize an object of type T, asserting that T can be
 * safely malloc-ed (is trivially constructible).
 */
template<typename T>
T *zmalloc()
{
	static_assert (can_malloc<T>::value, "type can be malloc'ed");
	return (T *) zmalloc_internal(sizeof(T));
}

/*
 * Malloc and zero-initialize a buffer of size `size`, asserting that type T
 * can be safely malloc-ed (is trivially constructible).
 */
template<typename T>
T *zmalloc(size_t size)
{
	static_assert (can_malloc<T>::value, "type can be malloc'ed");
	LTTNG_ASSERT(size >= sizeof(T));
	return (T *) zmalloc_internal(size);
}

/*
 * Malloc and zero-initialize an array of `nmemb` elements of type T,
 * asserting that T can be safely malloc-ed (is trivially constructible).
 */
template<typename T>
T *calloc(size_t nmemb)
{
	static_assert (can_malloc<T>::value, "type can be malloc'ed");
	return (T *) zmalloc_internal(nmemb * sizeof(T));
}

/*
 * Malloc an object of type T, asserting that T can be safely malloc-ed (is
 * trivially constructible).
 */
template<typename T>
T *malloc()
{
	static_assert (can_malloc<T>::value, "type can be malloc'ed");
	return (T *) malloc(sizeof(T));
}

/*
 * Malloc a buffer of size `size`, asserting that type T can be safely
 * malloc-ed (is trivially constructible).
 */
template<typename T>
T *malloc(size_t size)
{
	static_assert (can_malloc<T>::value, "type can be malloc'ed");
	return (T *) malloc(size);
}

/*
 * Prevent using `free` on types that are non-POD.
 *
 * Declare a delete prototype of free if the parameter type is not safe to free
 * (non-POD).
 *
 * If the parameter is a pointer to void, we can't check if what is pointed
 * to is safe to free or not, as we don't know what is pointed to.  Ideally,
 * all calls to free would be with a typed pointer, but there are too many
 * instances of passing a pointer to void to enforce that right now.  So allow
 * pointers to void, these will not be checked.
 */

template<typename T>
struct can_free
{
	/*
	 * gcc versions before 5.0 lack some type traits defined in C++11.
	 * Since in this instance we use the trait to prevent misuses
	 * of free (and statically assert) and not to generate different
	 * code based on this property, simply set value to true and allow
	 * the code to compile. Anyone using a contemporary compiler will
	 * catch the error.
	 */
#if __GNUG__ && __GNUC__ < 5
	static constexpr bool value = true;
#else
	static constexpr bool value = std::is_trivially_destructible<T>::value || std::is_void<T>::value;
#endif
};

template<typename T, typename = typename std::enable_if<!can_free<T>::value>::type>
void free(T *p) = delete;

template<typename T>
struct can_memset
{
	static constexpr bool value = std::is_pod<T>::value || std::is_void<T>::value;
};

template <typename T, typename = typename std::enable_if<!can_memset<T>::value>::type>
void *memset(T *s, int c, size_t n) = delete;

template<typename T>
struct can_memcpy
{
	/*
	 * gcc versions before 5.0 lack some type traits defined in C++11.
	 * Since in this instance we use the trait to prevent misuses
	 * of memcpy (and statically assert) and not to generate different
	 * code based on this property, simply set value to true and allow
	 * the code to compile. Anyone using a contemporary compiler will
	 * catch the error.
	 */
#if __GNUG__ && __GNUC__ < 5
	static constexpr bool value = true;
#else
	static constexpr bool value = std::is_trivially_copyable<T>::value;
#endif
};

template <typename T, typename U,
		typename = typename std::enable_if<!can_memcpy<T>::value>::type,
		typename = typename std::enable_if<!can_memcpy<U>::value>::type>
void *memcpy(T *d, const U *s, size_t n) = delete;

template<typename T>
struct can_memmove
{
	/*
	 * gcc versions before 5.0 lack some type traits defined in C++11.
	 * Since in this instance we use the trait to prevent misuses
	 * of memmove (and statically assert) and not to generate different
	 * code based on this property, simply set value to true and allow
	 * the code to compile. Anyone using a contemporary compiler will
	 * catch the error.
	 */
#if __GNUG__ && __GNUC__ < 5
	static constexpr bool value = true;
#else
	static constexpr bool value = std::is_trivially_copyable<T>::value;
#endif
};

template <typename T, typename U,
		typename = typename std::enable_if<!can_memmove<T>::value>::type,
		typename = typename std::enable_if<!can_memmove<U>::value>::type>
void *memmove(T *d, const U *s, size_t n) = delete;

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(array)   (sizeof(array) / (sizeof((array)[0])))
#endif

#ifndef container_of
#define container_of(ptr, type, member)					\
	({								\
		const typeof(((type *)NULL)->member) * __ptr = (ptr);	\
		(type *)((char *)__ptr - offsetof(type, member));	\
	})
#endif

#ifndef LTTNG_PACKED
#define LTTNG_PACKED __attribute__((__packed__))
#endif

#ifndef LTTNG_NO_SANITIZE_ADDRESS
#if defined(__clang__) || defined (__GNUC__)
#define LTTNG_NO_SANITIZE_ADDRESS __attribute__((no_sanitize_address))
#else
#define LTTNG_NO_SANITIZE_ADDRESS
#endif
#endif

#define member_sizeof(type, field)	sizeof(((type *) 0)->field)

#define ASSERT_LOCKED(lock) LTTNG_ASSERT(pthread_mutex_trylock(&lock))
#define ASSERT_RCU_READ_LOCKED(lock) LTTNG_ASSERT(rcu_read_ongoing())

/* Attribute suitable to tag functions as having printf()-like arguments. */
#define ATTR_FORMAT_PRINTF(_string_index, _first_to_check) \
	__attribute__((format(printf, _string_index, _first_to_check)))

/* Attribute suitable to tag functions as having strftime()-like arguments. */
#define ATTR_FORMAT_STRFTIME(_string_index) \
	__attribute__((format(strftime, _string_index, 0)))

/* Macros used to ignore specific compiler diagnostics. */

#define DIAGNOSTIC_PUSH _Pragma("GCC diagnostic push")
#define DIAGNOSTIC_POP _Pragma("GCC diagnostic pop")

#if defined(__clang__)
  /* Clang */
# define DIAGNOSTIC_IGNORE_SUGGEST_ATTRIBUTE_FORMAT
# define DIAGNOSTIC_IGNORE_FORMAT_NONLITERAL \
	_Pragma("GCC diagnostic ignored \"-Wformat-nonliteral\"")
# define DIAGNOSTIC_IGNORE_LOGICAL_OP
# define DIAGNOSTIC_IGNORE_DUPLICATED_BRANCHES
# define DIAGNOSTIC_IGNORE_INVALID_OFFSETOF
	_Pragma("GCC diagnostic ignored \"-Winvalid-offsetof\"")
#else
  /* GCC */
# define DIAGNOSTIC_IGNORE_SUGGEST_ATTRIBUTE_FORMAT \
	_Pragma("GCC diagnostic ignored \"-Wsuggest-attribute=format\"")
# define DIAGNOSTIC_IGNORE_FORMAT_NONLITERAL \
	_Pragma("GCC diagnostic ignored \"-Wformat-nonliteral\"")
# define DIAGNOSTIC_IGNORE_LOGICAL_OP \
	_Pragma("GCC diagnostic ignored \"-Wlogical-op\"")
# define DIAGNOSTIC_IGNORE_DUPLICATED_BRANCHES \
	_Pragma("GCC diagnostic ignored \"-Wduplicated-branches\"")
# define DIAGNOSTIC_IGNORE_INVALID_OFFSETOF \
	_Pragma("GCC diagnostic ignored \"-Winvalid-offsetof\"")
#endif

/* Used to make specific C++ functions to C code. */
#ifdef __cplusplus
#define C_LINKAGE extern "C"
#else
#define C_LINKAGE
#endif

/*
 * lttng_strncpy returns 0 on success, or nonzero on failure.
 * It checks that the @src string fits into @dst_len before performing
 * the copy. On failure, no copy has been performed.
 *
 * Assumes that 'src' is null-terminated.
 *
 * dst_len includes the string's trailing NULL.
 */
static inline
int lttng_strncpy(char *dst, const char *src, size_t dst_len)
{
	if (strlen(src) >= dst_len) {
		/* Fail since copying would result in truncation. */
		return -1;
	}
	strcpy(dst, src);
	return 0;
}

#endif /* _MACROS_H */
