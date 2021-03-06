// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2020, Logan Gunthorpe
 *
 * Please submit bug fixes and improvements to:
 *   https://github.com/lsgunth/libfailinj
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE  SOFTWARE.
 */

#define _GNU_SOURCE

#define UNW_LOCAL_ONLY
#include <libunwind.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#ifndef NAME
#define NAME FAILINJ
#endif

#define _str(x) #x
#define str(x) _str(x)
#define SNAME str(NAME)
#define TAG "\n" SNAME ": "
#define PFX SNAME "_"

static volatile bool use_early_allocator;
static bool force_libc;
static bool found_bug;
static bool has_injected_failure;
static bool failed;

struct hash_entry {
	unsigned long long hash;
	char *backtrace;
	struct hash_entry *next;
};

#define HASH_TABLE_SIZE 1024
#define HASH_TABLE_MASK (HASH_TABLE_SIZE - 1)
static pthread_mutex_t hash_table_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct hash_entry *callsite_table[HASH_TABLE_SIZE];
static struct hash_entry *allocation_table[HASH_TABLE_SIZE];
static struct hash_entry *fd_table[HASH_TABLE_SIZE];
static struct hash_entry *file_table[HASH_TABLE_SIZE];
static struct hash_entry *ferror_table[HASH_TABLE_SIZE];

/*
 * Simple hash function based on
 *  http://www.cse.yorku.ca/~oz/hash.html
 */
#define HASH_INIT 53815381
static unsigned long long djb_hash(const char *inp, unsigned long long hash)
{
	while(*inp)
		hash = (hash * 33) ^ *inp++;

	return hash;
}

/*
 * Insert into a hash table, return 0 if the element already
 * exists, 1 if it was inserted.
 */
static int hash_table_insert(struct hash_entry *n, struct hash_entry **table)
{
	struct hash_entry **slot;
	int ret = 1;

	pthread_mutex_lock(&hash_table_mutex);

	slot = &table[n->hash & HASH_TABLE_MASK];

	while (*slot) {
		if ((*slot)->hash == n->hash) {
			ret = 0;
			goto out;
		}

                if ((*slot)->hash > n->hash)
			break;

		slot = &((*slot)->next);
	}

	n->next = *slot;
	*slot = n;

out:
	pthread_mutex_unlock(&hash_table_mutex);
	return ret;
}

static struct hash_entry **__hash_table_find(unsigned long long hash,
					     struct hash_entry **table)
{
	struct hash_entry **slot;

	slot = &table[hash & HASH_TABLE_MASK];

	while (*slot) {
		if ((*slot)->hash == hash)
			return slot;

		if ((*slot)->hash > hash)
			return NULL;

		slot = &((*slot)->next);
	}

	return NULL;
}

static struct hash_entry *hash_table_find(unsigned long long hash,
					  struct hash_entry **table)
{
	struct hash_entry **slot, *ret = NULL;

	pthread_mutex_lock(&hash_table_mutex);
	slot = __hash_table_find(hash, table);
	if (slot)
		ret = *slot;
	pthread_mutex_unlock(&hash_table_mutex);

	return ret;
}

static struct hash_entry *hash_table_pop(unsigned long long hash,
					 struct hash_entry **table)
{
	struct hash_entry **slot, *ret = NULL;

	pthread_mutex_lock(&hash_table_mutex);
	slot = __hash_table_find(hash, table);
	if (slot) {
		ret = *slot;
		*slot = ret->next;
	}
	pthread_mutex_unlock(&hash_table_mutex);

	return ret;
}

static void __exit_error(const char *env, int err)
{
	const char *errstr = getenv(env);
	char *end;
	int tmp;

	if (errstr) {
		tmp = strtol(errstr, &end, 0);
		if (end != errstr && *end == '\0')
			err = tmp;
	}

#ifdef GCOV
	extern void __gcov_dump(void);
	__gcov_dump();
	/*
	 * The following lines will be hit by coverage but the data will not be
	 * saved (LCOV_EXCL_START)
	 */
#endif
	exit(err);
}
/* LCOV_EXCL_STOP */

static void exit_error(void)
{
	failed = true;
	__exit_error(PFX "EXIT_ERROR", 32);
}

static struct hash_entry *create_hash_entry(void)
{
	struct hash_entry *h;

	h = malloc(sizeof(*h));
	if (!h) {
		perror(SNAME);
		exit_error();
	}

	h->next = NULL;
	h->backtrace = NULL;
	h->hash = HASH_INIT;

	return h;
}

static FILE *load_database(void)
{
	const char *fname = getenv(PFX "DATABASE");
	struct hash_entry *h;
	size_t read;
	FILE *dbf;
	int ret;

	if (!fname)
		fname = "failinj.db";

	dbf = fopen(fname, "a+b");
	if (!dbf) {
		fprintf(stderr, TAG "Unable to open '%s': %m\n", fname);
		exit_error();
	}

	h = create_hash_entry();

	while (1) {
		read = fread(&h->hash, sizeof(h->hash), 1, dbf);
		if (ferror(dbf)) {
			perror(TAG "Unable to read database");
			exit_error();
		}

		if (read != 1)
			break;

		ret = hash_table_insert(h, callsite_table);
		if (ret)
			h = create_hash_entry();

		/*
		 * break if we see multiple zero hashes, this is for testing
		 * with /dev/full which outputs a stream of zeros
		 */
		if (!ret && h->hash == 0)
			break;
	}

	free(h);
	return dbf;
}

static void write_callsite(FILE *dbf, struct hash_entry *h)
{
	size_t written;
	int ret;

	written = fwrite(&h->hash, sizeof(h->hash), 1, dbf);
	if (written != 1) {
		perror(TAG "Unable to write database");
		exit_error();
	}

	/* flush in case the program crashes in the error handler */
	ret = fflush(dbf);
	if (ret == EOF) {
		perror(TAG "Unable to write database");
		exit_error();
	}
}

static struct hash_entry *get_current_callsite(void)
{
	char *skip = getenv(PFX "SKIP_INJECTION");
	struct hash_entry *h = create_hash_entry();
	unw_cursor_t cursor;
	unw_context_t uc;
	unw_word_t off;
	char name[4096];
	int ret;

	unw_getcontext(&uc);
	unw_init_local(&cursor, &uc);

	while (unw_step(&cursor) > 0) {
		ret = unw_get_proc_name(&cursor, name, sizeof(name), &off);
		if (ret != 0) {
			strcpy(name, "unknown");
		} else {
			if (skip && strstr(skip, name))
				goto skip;

			if (!strcmp(name, "exit"))
				goto skip;

			snprintf(name + strlen(name),
				 sizeof(name) - strlen(name),
				 "+0x%lx", off);
		}

		h->hash = djb_hash(name, h->hash);
	}

	return h;
skip:
	free(h);
	return NULL;
}

static void print_backtrace(void)
{
	unw_cursor_t cursor;
	unw_context_t uc;
	unw_word_t off;
	char name[4096];
	int ret;

	unw_getcontext(&uc);
	unw_init_local(&cursor, &uc);

	/* Ignore the current function */
	unw_step(&cursor);

	while (unw_step(&cursor) > 0) {
		ret = unw_get_proc_name(&cursor, name, sizeof(name), &off);
		if (ret == 0)
			fprintf(stderr, "    %s+0x%lx\n", name, off);
		else
			fprintf(stderr, "    ?unknown\n");
	}
}

static void print_injection(void)
{
	fprintf(stderr, TAG "Injecting failure at:\n");
	print_backtrace();
	fprintf(stderr, "\n");
}

static bool should_fail(const char *name)
{
	static FILE *dbf = NULL;
	struct hash_entry *h;
	bool ret = false;

	if (has_injected_failure)
		return false;

	force_libc = true;

	if (!dbf)
		dbf = load_database();

	h = get_current_callsite();
	if (!h)
		goto out;

	ret = hash_table_insert(h, callsite_table);
	if (!ret) {
		free(h);
	} else {
		write_callsite(dbf, h);
		print_injection();
		has_injected_failure = true;
	}

out:
	force_libc = false;
	return ret;
}

static bool should_ignore_err(const char *backtrace, const char *ignore_env,
			      const char *ignore_all_env)
{
	char *ignore_all = getenv(ignore_all_env);
	char *ignore = getenv(ignore_env);
	char *ignore_cpy, *tok;

	if (ignore_all)
		return true;

	if (!ignore)
		return false;

	ignore_cpy = strdup(ignore);
	if (!ignore_cpy) {
		perror(SNAME);
		exit_error();
	}

	tok = strtok(ignore, " ");
	while (tok) {
		if (strstr(backtrace, tok)) {
			free(ignore_cpy);
			return true;
		}

		tok = strtok(NULL, " ");
	}

	free(ignore_cpy);
	return false;
}

static char *get_backtrace_string(void)
{
	char name[4096], backtrace[4096];
	unw_cursor_t cursor;
	unw_context_t uc;
	unw_word_t off;
	char *retstr;
	int boff = 0;
	int ret;

	unw_getcontext(&uc);
	unw_init_local(&cursor, &uc);

	while (unw_step(&cursor) > 0) {
		ret = unw_get_proc_name(&cursor, name, sizeof(name), &off);
		if (ret != 0)
			strcpy(name, "unknown");

		boff += snprintf(backtrace + boff, sizeof(backtrace) - boff,
				 "    %s+0x%lx\n", name, off);
	}

	retstr = strdup(backtrace);
	if (!retstr) {
		perror(SNAME);
		exit_error();
	}

	return retstr;
}

static void track_create(unsigned long long hash,
			 struct hash_entry **table)
{
	struct hash_entry *h;

	if (force_libc || !hash)
		return;

	force_libc = true;

	h = create_hash_entry();
	h->backtrace = get_backtrace_string();
	h->hash = hash;
	hash_table_insert(h, table);

	force_libc = false;
}

static void track_destroy(unsigned long long hash, struct hash_entry **table,
			  const char *ignore_env, const char *ignore_all_env,
			  const char *msg)
{
	struct hash_entry *h;
	char *backtrace;

	if (force_libc || !hash)
		return;

	force_libc = true;

	h = hash_table_pop(hash, table);
	if (!h) {
		backtrace = get_backtrace_string();
		if (!should_ignore_err(backtrace, ignore_env,
				       ignore_all_env)) {
			fprintf(stderr, msg, hash);
			print_backtrace();
			found_bug = true;
		}
		free(backtrace);
	} else {
		free(h->backtrace);
		free(h);
	}

	force_libc = false;
}

static void *early_allocator(size_t size)
{
	static char early_mem[4096];
	static int pos;

	if ((pos + size) > sizeof(early_mem))
		return NULL;

	pos += size;

	return &early_mem[pos - size];
}

#define call_super(name, ret_type, ...) ({ \
	bool last_force_libc = force_libc; \
	static ret_type (*__super)(); \
	ret_type ret; \
	if (!__super) { \
		use_early_allocator = true; \
		__super = dlsym(RTLD_NEXT, #name); \
		use_early_allocator = false; \
	} \
	force_libc = true; \
	ret = __super(__VA_ARGS__); \
	force_libc = last_force_libc; \
	ret; \
})

#define call_super_void(name, ...) ({ \
	bool last_force_libc = force_libc; \
	static void (*__super)(); \
	if (!__super) { \
		use_early_allocator = true; \
		__super = dlsym(RTLD_NEXT, #name); \
		use_early_allocator = false; \
	} \
	force_libc = true; \
	__super(__VA_ARGS__); \
	force_libc = last_force_libc; \
})

#define handle_call(name, ret_type, err_ret, err_errno, ...) ({ \
	if (!force_libc && should_fail(#name)) { \
		errno = err_errno; \
		return err_ret; \
	} \
	call_super(name, ret_type, __VA_ARGS__); \
})

#define handle_call_close(name, ret_type, err_ret, err_errno, ...) ({ \
	ret_type ret; \
	ret = call_super(name, ret_type, __VA_ARGS__); \
	if (!ret && !force_libc && should_fail(#name)) { \
		errno = err_errno; \
		ret = err_ret; \
	} \
	ret; \
})

void *malloc(size_t size)
{
	void *ret;

	/*
	 * glibc does not use malloc inside dlsym so this will never
	 * be hit, but include it here in case this ever changes
	 */
	if (use_early_allocator)
		return early_allocator(size); /* LCOV_EXCL_LINE */

	ret = handle_call(malloc, void *, NULL, ENOMEM, size);
	if (ret)
		track_create((intptr_t)ret, allocation_table);

	return ret;
}

void *calloc(size_t nmemb, size_t size)
{
	void *ret;

	if (use_early_allocator)
		return early_allocator(nmemb * size);

	ret = handle_call(calloc, void *, NULL, ENOMEM, nmemb, size);
	if (ret)
		track_create((intptr_t)ret, allocation_table);

	return ret;
}

void *realloc(void *ptr, size_t size)
{
	void *ret;

	ret = handle_call(realloc, void *, NULL, ENOMEM, ptr, size);
	if (ret) {
		track_destroy((intptr_t)ptr, allocation_table,
			      PFX "IGNORE_UNTRACKED_FREES",
			      PFX "IGNORE_ALL_UNTRACKED_FREES",
			      TAG "Attempted to realloc untracked pointer 0x%llx at:\n");
		track_create((intptr_t)ret, allocation_table);
	}

	return ret;
}

void free(void *ptr)
{
	call_super_void(free, ptr);
	track_destroy((intptr_t)ptr, allocation_table,
		      PFX "IGNORE_UNTRACKED_FREES",
		      PFX "IGNORE_ALL_UNTRACKED_FREES",
		      TAG "Attempted to free untracked pointer 0x%llx at:\n");
}

int creat(const char *pathname, mode_t mode)
{
	int fd;

	fd = handle_call(creat, int, -1, EACCES, pathname, mode);
	if (fd != -1)
		track_create(fd, fd_table);

	return fd;
}

int open(const char *pathname, int flags, ...)
{
	va_list ap;
	mode_t mode;
	int fd;

	va_start(ap, flags);
	mode = va_arg(ap, mode_t);
	va_end(ap);

	fd = handle_call(open, int, -1, EACCES, pathname, flags, mode);
	if (fd != -1)
		track_create(fd, fd_table);

	return fd;
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
	va_list ap;
	mode_t mode;
	int fd;

	va_start(ap, flags);
	mode = va_arg(ap, mode_t);
	va_end(ap);

	fd = handle_call(openat, int, -1, EACCES, dirfd, pathname, flags,
			 mode);
	if (fd != -1)
		track_create(fd, fd_table);

	return fd;
}

int close(int fd)
{
	track_destroy(fd, fd_table,
		      PFX "IGNORE_UNTRACKED_CLOSES",
		      PFX "IGNORE_ALL_UNTRACKED_CLOSES",
		      TAG "Attempted to close untracked file descriptor %lld at:\n");
	return handle_call_close(close, int, -1, EDQUOT, fd);
}

ssize_t read(int fd, void *buf, size_t count)
{
	return handle_call(read, int, -1, EIO, fd, buf, count);
}

ssize_t write(int fd, const void *buf, size_t count)
{
	return handle_call(write, int, -1, ENOSPC, fd, buf, count);
}

FILE *fopen(const char *pathname, const char *mode)
{
	FILE *f;

	f = handle_call(fopen, FILE *, NULL, EACCES, pathname, mode);
	if (f)
		track_create((intptr_t)f, file_table);

	return f;
}

FILE *fdopen(int fd, const char *mode)
{
	FILE *f;

	f = handle_call(fdopen, FILE *, NULL, EPERM, fd, mode);
	if (f) {
		track_create((intptr_t)f, file_table);
		track_destroy(fd, fd_table,
			      PFX "IGNORE_UNTRACKED_FCLOSES",
			      PFX "IGNORE_ALL_UNTRACKED_FCLOSES",
			      TAG "Attempted to fdopen untracked file descriptor %lld at:\n");
	}

	return f;
}

FILE *fmemopen(void *buf, size_t size, const char *mode)
{
	FILE *f;

	f = handle_call(fmemopen, FILE *, NULL, ENOMEM, buf, size, mode);
	if (f)
		track_create((intptr_t)f, file_table);

	return f;
}

FILE *tmpfile(void)
{
	FILE *f;

	f = handle_call(tmpfile, FILE *, NULL, EROFS);
	if (f)
		track_create((intptr_t)f, file_table);

	return f;
}

int fclose(FILE *stream)
{
	track_destroy((intptr_t)stream, file_table,
		      PFX "IGNORE_UNTRACKED_FCLOSES",
		      PFX "IGNORE_ALL_UNTRACKED_FCLOSES",
		      TAG "Attempted to fclose untracked file 0x%llx at:\n");
	return handle_call_close(fclose, int, EOF, ENOSPC, stream);
}

int fcloseall(void)
{
	struct hash_entry *h;
	int i;

	force_libc = true;
	for (i = 0; i < HASH_TABLE_SIZE; i++) {
		h = file_table[i];
		while (h) {
			free(h->backtrace);
			free(h);

			h = h->next;
		}

		file_table[i] = NULL;
	}
	force_libc = false;

	return handle_call_close(fcloseall, int, EOF, ENOSPC);
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	return handle_call(fwrite, size_t, 0, ENOSPC, ptr, size, nmemb,
			   stream);
}

static void flag_ferror(FILE *stream)
{
	struct hash_entry *h;

	force_libc = true;
	h = create_hash_entry();
	h->hash = (intptr_t)stream;
	hash_table_insert(h, ferror_table);
	force_libc = false;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	if (!force_libc && should_fail("fread")) {
		flag_ferror(stream);
		errno = EIO;
		return 0;
	}

	return call_super(fread, size_t, ptr, size, nmemb, stream);
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream)
{
	char *old = *lineptr;
	ssize_t ret;

	ret = handle_call(getline, ssize_t, -1, ENOMEM, lineptr, n, stream);
	if (old != *lineptr) {
		track_destroy((intptr_t)old, allocation_table,
			      PFX "IGNORE_UNTRACKED_FREES",
			      PFX "IGNORE_ALL_UNTRACKED_FREES",
			      TAG "Attempted to  untracked pointer 0x%llx at:\n");
		track_create((intptr_t)*lineptr, allocation_table);
	}

	return ret;
}

ssize_t __getdelim(char **lineptr, size_t *n, int delim, FILE *stream)
{
	char *old = *lineptr;
	ssize_t ret;

	ret = handle_call(__getdelim, ssize_t, -1, ENOMEM, lineptr, n,
			  delim, stream);
	if (old != *lineptr) {
		track_destroy((intptr_t)old, allocation_table,
			      PFX "IGNORE_UNTRACKED_FREES",
			      PFX "IGNORE_ALL_UNTRACKED_FREES",
			      TAG "Attempted to realloc untracked pointer 0x%llx at:\n");
		track_create((intptr_t)*lineptr, allocation_table);
	}

	return ret;
}

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream)
{
	char *old = *lineptr;
	ssize_t ret;

	ret = handle_call(getdelim, ssize_t, -1, ENOMEM, lineptr, n,
			  delim, stream);
	if (old != *lineptr) {
		track_destroy((intptr_t)old, allocation_table,
			      PFX "IGNORE_UNTRACKED_FREES",
			      PFX "IGNORE_ALL_UNTRACKED_FREES",
			      TAG "Attempted to realloc untracked pointer 0x%llx at:\n");
		track_create((intptr_t)*lineptr, allocation_table);
	}

	return ret;
}

int ferror(FILE *stream)
{
	if (!force_libc && hash_table_find((intptr_t)stream, ferror_table))
		return 1;

	return call_super(ferror, int, stream);
}

void clearerr(FILE *stream)
{
	struct hash_entry *h;

	if (!force_libc) {
		h = hash_table_pop((intptr_t)stream, ferror_table);
		free(h);
	}

	call_super_void(clearerr, stream);
}

int fflush(FILE *stream)
{
	return handle_call(fflush, int, EOF, ENOSPC, stream);
}

int fputc(int c, FILE *stream)
{
	return call_super(fputc, int, c, stream);
}

int fputs(const char *s, FILE *stream)
{
	return call_super(fputs, int, s, stream);
}

int putc(int c, FILE *stream)
{
	return call_super(putc, int, c, stream);
}

int puts(const char *s)
{
	return call_super(puts, int, s);
}

int printf(const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = vprintf(format, ap);
	va_end(ap);

	return ret;
}

int fprintf(FILE *stream, const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = vfprintf(stream, format, ap);
	va_end(ap);

	return ret;
}

int vprintf(const char *format, va_list ap)
{
	return call_super(vprintf, int, format, ap);
}

int vfprintf(FILE *stream, const char *format, va_list ap)
{
	return call_super(vfprintf, int, stream, format, ap);
}

#undef vfscanf
#undef vsscanf

int fscanf(FILE *stream, const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = vfscanf(stream, format, ap);
	va_end(ap);

	return ret;
}

int sscanf(const char *str, const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = vsscanf(str, format, ap);
	va_end(ap);

	return ret;
}

int vsscanf(const char *str, const char *format, va_list ap)
{
	return handle_call(vsscanf, int, -1, ENOMEM, str, format, ap);
}

int vfscanf(FILE *stream, const char *format, va_list ap)
{
	if (!force_libc && should_fail("vfscanf")) {
		flag_ferror(stream);
		errno = EIO;
		return EOF;
	}

	return call_super(vfscanf, int, stream, format, ap);
}

#if !__GLIBC_PREREQ(2,29)
/*
 * Historically glibc used a different name for scanf in different conditions.
 * As of 2.29 this is no longer the case and this will cause a duplicate
 * symbol with the above functions.
 */
int __isoc99_vsscanf(const char *str, const char *format, va_list ap)
{
	return handle_call(__isoc99_vsscanf, int, -1, ENOMEM, str, format, ap);
}

int __isoc99_vfscanf(FILE *stream, const char *format, va_list ap)
{
	if (!force_libc && should_fail("vfscanf")) {
		flag_ferror(stream);
		errno = EIO;
		return EOF;
	}

	return call_super(__isoc99_vfscanf, int, stream, format, ap);
}

int __isoc99_sscanf(const char *str, const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = __isoc99_vsscanf(str, format, ap);
	va_end(ap);

	return ret;
}

int __isoc99_fscanf(FILE *stream, const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = __isoc99_vfscanf(stream, format, ap);
	va_end(ap);

	return ret;
}
#endif

int mprotect(void *addr, size_t len, int prot)
{
	return handle_call(mprotect, int, -1, ENOMEM, addr, len, prot);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd,
	   off_t offset)
{
	void *ret;

	ret = handle_call(mmap, void *, MAP_FAILED, ENOMEM, addr, length, prot,
			  flags, fd, offset);
	if (ret != MAP_FAILED)
		track_create((intptr_t)ret, allocation_table);

	return ret;
}

int munmap(void *addr, size_t length)
{
	call_super_void(munmap, addr, length);
	track_destroy((intptr_t)addr, allocation_table,
		      PFX "IGNORE_UNTRACKED_FREES",
		      PFX "IGNORE_ALL_UNTRACKED_FREES",
		      TAG "Attempted to munmap untracked pointer 0x%llx at:\n");
	return 0;
}

long syscall(long int syscall_number, long int arg1, long int arg2,
	     long int arg3, long int arg4, long int arg5, long int arg6,
	     long int arg7)
{
	return handle_call(syscall, long, -1, ENOTSUP, syscall_number,
			   arg1, arg2, arg3, arg4, arg5, arg6, arg7);
}

static void print_leak(struct hash_entry *h, const char *msg)
{
	found_bug = true;
	fprintf(stderr, msg, h->hash);
	fprintf(stderr, "%s", h->backtrace);
}

static void hdl_leaks(struct hash_entry *h, const char *ignore_env,
		      const char *ignore_all_env, const char *msg)
{
	while (h) {
		if (msg && !should_ignore_err(h->backtrace, ignore_env,
					      ignore_all_env))
			print_leak(h, msg);

		free(h->backtrace);
		free(h);

		h = h->next;
	}
}

__attribute__((destructor))
static void check_leaks(void)
{
	int i;

	force_libc = true;

	pthread_mutex_lock(&hash_table_mutex);
	for (i = 0; i < HASH_TABLE_SIZE; i++) {
		hdl_leaks(allocation_table[i], PFX "IGNORE_MEM_LEAKS",
			  PFX "IGNORE_ALL_MEM_LEAKS",
			  TAG "Possible memory leak for 0x%llx allocated at:\n");
		allocation_table[i] = NULL;

		hdl_leaks(fd_table[i], PFX "IGNORE_FD_LEAKS",
			  PFX "IGNORE_ALL_FD_LEAKS",
			  TAG "Possible file descriptor leak for %lld opened at:\n");
		fd_table[i] = NULL;

		hdl_leaks(file_table[i], PFX "IGNORE_FILE_LEAKS",
			  PFX "IGNORE_ALL_FILE_LEAKS",
			  TAG "Possible unclosed file for 0x%llx opened at:\n");
		file_table[i] = NULL;

		hdl_leaks(ferror_table[i], NULL, NULL, NULL);
	}
	pthread_mutex_unlock(&hash_table_mutex);

	if (failed)
		return;

	if (!has_injected_failure)
		__exit_error(PFX "EXIT_DONE", 34);
	if (found_bug)
		__exit_error(PFX "BUG_FOUND", 33);
}
