/* crypto/mem_dbg.c */
/* Copyright (C) 1995-1998 Eric Young (eay@cryptsoft.com)
 * All rights reserved.
 *
 * This package is an SSL implementation written
 * by Eric Young (eay@cryptsoft.com).
 * The implementation was written so as to conform with Netscapes SSL.
 * 
 * This library is free for commercial and non-commercial use as long as
 * the following conditions are aheared to.  The following conditions
 * apply to all code found in this distribution, be it the RC4, RSA,
 * lhash, DES, etc., code; not just the SSL code.  The SSL documentation
 * included with this distribution is covered by the same copyright terms
 * except that the holder is Tim Hudson (tjh@cryptsoft.com).
 * 
 * Copyright remains Eric Young's, and as such any Copyright notices in
 * the code are not to be removed.
 * If this package is used in a product, Eric Young should be given attribution
 * as the author of the parts of the library used.
 * This can be in the form of a textual message at program startup or
 * in documentation (online or textual) provided with the package.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    "This product includes cryptographic software written by
 *     Eric Young (eay@cryptsoft.com)"
 *    The word 'cryptographic' can be left out if the rouines from the library
 *    being used are not cryptographic related :-).
 * 4. If you include any Windows specific code (or a derivative thereof) from 
 *    the apps directory (application code) you must include an acknowledgement:
 *    "This product includes software written by Tim Hudson (tjh@cryptsoft.com)"
 * 
 * THIS SOFTWARE IS PROVIDED BY ERIC YOUNG ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * 
 * The licence and distribution terms for any publically available version or
 * derivative of this code cannot be changed.  i.e. this code cannot simply be
 * copied and put under another distribution licence
 * [including the GNU Public Licence.]
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>	
#include <openssl/crypto.h>
#include <openssl/buffer.h>
#include <openssl/bio.h>
#include <openssl/lhash.h>
#include "cryptlib.h"

static int mh_mode=CRYPTO_MEM_CHECK_OFF;
/* The state changes to CRYPTO_MEM_CHECK_ON | CRYPTO_MEM_CHECK_ENABLE
 * when the application asks for it (usually after library initialisation
 * for which no book-keeping is desired).
 *
 * State CRYPTO_MEM_CHECK_ON exists only temporarily when the library
 * thinks that certain allocations should not be checked (e.g. the data
 * structures used for memory checking).  It is not suitable as an initial
 * state: the library will unexpectedly enable memory checking when it
 * executes one of those sections that want to disable checking
 * temporarily.
 *
 * State CRYPTO_MEM_CHECK_ENABLE without ..._ON makes no sense whatsoever.
 */

static unsigned long order = 0; /* number of memory requests */
static LHASH *mh=NULL; /* hash-table of memory requests (address as key);
                        * access requires MALLOC2 lock */


typedef struct app_mem_info_st
/* For application-defined information (static C-string `info')
 * to be displayed in memory leak list.
 * Each thread has its own stack.  For applications, there is
 *   CRYPTO_push_info("...")     to push an entry,
 *   CRYPTO_pop_info()           to pop an entry,
 *   CRYPTO_remove_all_info()    to pop all entries.
 */
	{	
	unsigned long thread;
	const char *file;
	int line;
	const char *info;
	struct app_mem_info_st *next; /* tail of thread's stack */
	int references;
	} APP_INFO;

static LHASH *amih=NULL; /* hash-table with those app_mem_info_st's
                          * that are at the top of their thread's stack
                          * (with `thread' as key);
                          * access requires MALLOC2 lock */

typedef struct mem_st
/* memory-block description */
	{
	void *addr;
	int num;
	const char *file;
	int line;
	unsigned long thread;
	unsigned long order;
	time_t time;
	APP_INFO *app_info;
	} MEM;

static long options =             /* extra information to be recorded */
#if defined(CRYPTO_MDEBUG_TIME) || defined(CRYPTO_MDEBUG_ALL)
	V_CRYPTO_MDEBUG_TIME |
#endif
#if defined(CRYPTO_MDEBUG_THREAD) || defined(CRYPTO_MDEBUG_ALL)
	V_CRYPTO_MDEBUG_THREAD |
#endif
	0;


static unsigned int num_disable = 0; /* num_disable > 0
                                      *     iff
                                      * mh_mode == CRYPTO_MEM_CHECK_ON (w/o ..._ENABLE)
                                      */
static unsigned long disabling_thread = 0; /* Valid iff num_disable > 0.
                                            * CRYPTO_LOCK_MALLOC2 is locked
                                            * exactly in this case (by the
                                            * thread named in disabling_thread).
                                            */

int CRYPTO_mem_ctrl(int mode)
	{
	int ret=mh_mode;

	CRYPTO_w_lock(CRYPTO_LOCK_MALLOC);
	switch (mode)
		{
	/* for applications (not to be called while multiple threads
	 * use the library): */
	case CRYPTO_MEM_CHECK_ON: /* aka MemCheck_start() */
		mh_mode = CRYPTO_MEM_CHECK_ON|CRYPTO_MEM_CHECK_ENABLE;
		num_disable = 0;
		break;
	case CRYPTO_MEM_CHECK_OFF: /* aka MemCheck_stop() */
		mh_mode = 0;
		num_disable = 0; /* should be true *before* MemCheck_stop is used,
		                    or there'll be a lot of confusion */
		break;

	/* switch off temporarily (for library-internal use): */
	case CRYPTO_MEM_CHECK_DISABLE: /* aka MemCheck_off() */
		if (mh_mode & CRYPTO_MEM_CHECK_ON)
			{
			if (!num_disable || (disabling_thread != CRYPTO_thread_id())) /* otherwise we already have the MALLOC2 lock */
				{
				/* Long-time lock CRYPTO_LOCK_MALLOC2 must not be claimed while
				 * we're holding CRYPTO_LOCK_MALLOC, or we'll deadlock if
				 * somebody else holds CRYPTO_LOCK_MALLOC2 (and cannot release
				 * it because we block entry to this function).
				 * Give them a chance, first, and then claim the locks in
				 * appropriate order (long-time lock first).
				 */
				CRYPTO_w_unlock(CRYPTO_LOCK_MALLOC);
				/* Note that after we have waited for CRYPTO_LOCK_MALLOC2
				 * and CRYPTO_LOCK_MALLOC, we'll still be in the right
				 * "case" and "if" branch because MemCheck_start and
				 * MemCheck_stop may never be used while there are multiple
				 * OpenSSL threads. */
				CRYPTO_w_lock(CRYPTO_LOCK_MALLOC2);
				CRYPTO_w_lock(CRYPTO_LOCK_MALLOC);
				mh_mode &= ~CRYPTO_MEM_CHECK_ENABLE;
				disabling_thread=CRYPTO_thread_id();
				}
			num_disable++;
			}
		break;
	case CRYPTO_MEM_CHECK_ENABLE: /* aka MemCheck_on() */
		if (mh_mode & CRYPTO_MEM_CHECK_ON)
			{
			if (num_disable) /* always true, or something is going wrong */
				{
				num_disable--;
				if (num_disable == 0)
					{
					mh_mode|=CRYPTO_MEM_CHECK_ENABLE;
					CRYPTO_w_unlock(CRYPTO_LOCK_MALLOC2);
					}
				}
			}
		break;

	default:
		break;
		}
	CRYPTO_w_unlock(CRYPTO_LOCK_MALLOC);
	return(ret);
	}

int CRYPTO_is_mem_check_on(void)
	{
	int ret = 0;

	if (mh_mode & CRYPTO_MEM_CHECK_ON)
		{
		CRYPTO_r_lock(CRYPTO_LOCK_MALLOC);

		ret = (mh_mode & CRYPTO_MEM_CHECK_ENABLE)
			|| (disabling_thread != CRYPTO_thread_id());

		CRYPTO_r_unlock(CRYPTO_LOCK_MALLOC);
		}
	return(ret);
	}	


void CRYPTO_dbg_set_options(long bits)
	{
	options = bits;
	}

long CRYPTO_dbg_get_options(void)
	{
	return options;
	}

/* static int mem_cmp(MEM *a, MEM *b) */
static int mem_cmp(const void *a_void, const void *b_void)
	{
	return((const char *)((const MEM *)a_void)->addr
		- (const char *)((const MEM *)b_void)->addr);
	}

/* static unsigned long mem_hash(MEM *a) */
static unsigned long mem_hash(const void *a_void)
	{
	unsigned long ret;

	ret=(unsigned long)((const MEM *)a_void)->addr;

	ret=ret*17851+(ret>>14)*7+(ret>>4)*251;
	return(ret);
	}

/* static int app_info_cmp(APP_INFO *a, APP_INFO *b) */
static int app_info_cmp(const void *a_void, const void *b_void)
	{
	return(((const APP_INFO *)a_void)->thread
		!= ((const APP_INFO *)b_void)->thread);
	}

/* static unsigned long app_info_hash(APP_INFO *a) */
static unsigned long app_info_hash(const void *a_void)
	{
	unsigned long ret;

	ret=(unsigned long)((const APP_INFO *)a_void)->thread;

	ret=ret*17851+(ret>>14)*7+(ret>>4)*251;
	return(ret);
	}

static APP_INFO *pop_info(void)
	{
	APP_INFO tmp;
	APP_INFO *ret = NULL;

	if (amih != NULL)
		{
		tmp.thread=CRYPTO_thread_id();
		if ((ret=(APP_INFO *)lh_delete(amih,&tmp)) != NULL)
			{
			APP_INFO *next=ret->next;

			if (next != NULL)
				{
				next->references++;
				lh_insert(amih,(char *)next);
				}
#ifdef LEVITTE_DEBUG_MEM
			if (ret->thread != tmp.thread)
				{
				fprintf(stderr, "pop_info(): deleted info has other thread ID (%lu) than the current thread (%lu)!!!!\n",
					ret->thread, tmp.thread);
				abort();
				}
#endif
			if (--(ret->references) <= 0)
				{
				ret->next = NULL;
				if (next != NULL)
					next->references--;
				OPENSSL_free(ret);
				}
			}
		}
	return(ret);
	}

int CRYPTO_push_info_(const char *info, const char *file, int line)
	{
	APP_INFO *ami, *amim;
	int ret=0;

	if (is_MemCheck_on())
		{
		MemCheck_off(); /* obtain MALLOC2 lock */

		if ((ami = (APP_INFO *)OPENSSL_malloc(sizeof(APP_INFO))) == NULL)
			{
			ret=0;
			goto err;
			}
		if (amih == NULL)
			{
			if ((amih=lh_new(app_info_hash, app_info_cmp)) == NULL)
				{
				OPENSSL_free(ami);
				ret=0;
				goto err;
				}
			}

		ami->thread=CRYPTO_thread_id();
		ami->file=file;
		ami->line=line;
		ami->info=info;
		ami->references=1;
		ami->next=NULL;

		if ((amim=(APP_INFO *)lh_insert(amih,(char *)ami)) != NULL)
			{
#ifdef LEVITTE_DEBUG_MEM
			if (ami->thread != amim->thread)
				{
				fprintf(stderr, "CRYPTO_push_info(): previous info has other thread ID (%lu) than the current thread (%lu)!!!!\n",
					amim->thread, ami->thread);
				abort();
				}
#endif
			ami->next=amim;
			}
 err:
		MemCheck_on(); /* release MALLOC2 lock */
		}

	return(ret);
	}

int CRYPTO_pop_info(void)
	{
	int ret=0;

	if (is_MemCheck_on()) /* _must_ be true, or something went severely wrong */
		{
		MemCheck_off(); /* obtain MALLOC2 lock */

		ret=(pop_info() != NULL);

		MemCheck_on(); /* release MALLOC2 lock */
		}
	return(ret);
	}

int CRYPTO_remove_all_info(void)
	{
	int ret=0;

	if (is_MemCheck_on()) /* _must_ be true */
		{
		MemCheck_off(); /* obtain MALLOC2 lock */

		while(pop_info() != NULL)
			ret++;

		MemCheck_on(); /* release MALLOC2 lock */
		}
	return(ret);
	}


static unsigned long break_order_num=0;
void CRYPTO_dbg_malloc(void *addr, int num, const char *file, int line,
	int before_p)
	{
	MEM *m,*mm;
	APP_INFO tmp,*amim;

	switch(before_p & 127)
		{
	case 0:
		break;
	case 1:
		if (addr == NULL)
			break;

		if (is_MemCheck_on())
			{
			MemCheck_off(); /* make sure we hold MALLOC2 lock */
			if ((m=(MEM *)OPENSSL_malloc(sizeof(MEM))) == NULL)
				{
				OPENSSL_free(addr);
				MemCheck_on(); /* release MALLOC2 lock
				                * if num_disabled drops to 0 */
				return;
				}
			if (mh == NULL)
				{
				if ((mh=lh_new(mem_hash, mem_cmp)) == NULL)
					{
					OPENSSL_free(addr);
					OPENSSL_free(m);
					addr=NULL;
					goto err;
					}
				}

			m->addr=addr;
			m->file=file;
			m->line=line;
			m->num=num;
			if (options & V_CRYPTO_MDEBUG_THREAD)
				m->thread=CRYPTO_thread_id();
			else
				m->thread=0;

			if (order == break_order_num)
				{
				/* BREAK HERE */
				m->order=order;
				}
			m->order=order++;
#ifdef LEVITTE_DEBUG_MEM
			fprintf(stderr, "LEVITTE_DEBUG_MEM: [%5d] %c 0x%p (%d)\n",
				m->order,
				(before_p & 128) ? '*' : '+',
				m->addr, m->num);
#endif
			if (options & V_CRYPTO_MDEBUG_TIME)
				m->time=time(NULL);
			else
				m->time=0;

			tmp.thread=CRYPTO_thread_id();
			m->app_info=NULL;
			if (amih != NULL
				&& (amim=(APP_INFO *)lh_retrieve(amih,(char *)&tmp)) != NULL)
				{
				m->app_info = amim;
				amim->references++;
				}

			if ((mm=(MEM *)lh_insert(mh,(char *)m)) != NULL)
				{
				/* Not good, but don't sweat it */
				if (mm->app_info != NULL)
					{
					mm->app_info->references--;
					}
				OPENSSL_free(mm);
				}
		err:
			MemCheck_on(); /* release MALLOC2 lock
			                * if num_disabled drops to 0 */
			}
		break;
		}
	return;
	}

void CRYPTO_dbg_free(void *addr, int before_p)
	{
	MEM m,*mp;

	switch(before_p)
		{
	case 0:
		if (addr == NULL)
			break;

		if (is_MemCheck_on() && (mh != NULL))
			{
			MemCheck_off(); /* make sure we hold MALLOC2 lock */

			m.addr=addr;
			mp=(MEM *)lh_delete(mh,(char *)&m);
			if (mp != NULL)
				{
#ifdef LEVITTE_DEBUG_MEM
			fprintf(stderr, "LEVITTE_DEBUG_MEM: [%5d] - 0x%p (%d)\n",
				mp->order, mp->addr, mp->num);
#endif
				if (mp->app_info != NULL)
					{
					mp->app_info->references--;
					}
				OPENSSL_free(mp);
				}

			MemCheck_on(); /* release MALLOC2 lock
			                * if num_disabled drops to 0 */
			}
		break;
	case 1:
		break;
		}
	}

void CRYPTO_dbg_realloc(void *addr1, void *addr2, int num,
	const char *file, int line, int before_p)
	{
	MEM m,*mp;

#ifdef LEVITTE_DEBUG_MEM
	fprintf(stderr, "LEVITTE_DEBUG_MEM: --> CRYPTO_dbg_malloc(addr1 = %p, addr2 = %p, num = %d, file = \"%s\", line = %d, before_p = %d)\n",
		addr1, addr2, num, file, line, before_p);
#endif

	switch(before_p)
		{
	case 0:
		break;
	case 1:
		if (addr2 == NULL)
			break;

		if (addr1 == NULL)
			{
			CRYPTO_dbg_malloc(addr2, num, file, line, 128 | before_p);
			break;
			}

		if (is_MemCheck_on())
			{
			MemCheck_off(); /* make sure we hold MALLOC2 lock */

			m.addr=addr1;
			mp=(MEM *)lh_delete(mh,(char *)&m);
			if (mp != NULL)
				{
#ifdef LEVITTE_DEBUG_MEM
				fprintf(stderr, "LEVITTE_DEBUG_MEM: [%5d] * 0x%p (%d) -> 0x%p (%d)\n",
					mp->order,
					mp->addr, mp->num,
					addr2, num);
#endif
				mp->addr=addr2;
				mp->num=num;
				lh_insert(mh,(char *)mp);
				}

			MemCheck_on(); /* release MALLOC2 lock
			                * if num_disabled drops to 0 */
			}
		break;
		}
	return;
	}


typedef struct mem_leak_st
	{
	BIO *bio;
	int chunks;
	long bytes;
	} MEM_LEAK;

static void print_leak(const MEM *m, MEM_LEAK *l)
	{
	char buf[1024];
	char *bufp = buf;
	APP_INFO *amip;
	int ami_cnt;
	struct tm *lcl = NULL;
	unsigned long ti;

	if(m->addr == (char *)l->bio)
	    return;

	if (options & V_CRYPTO_MDEBUG_TIME)
		{
		lcl = localtime(&m->time);
	
		sprintf(bufp, "[%02d:%02d:%02d] ",
			lcl->tm_hour,lcl->tm_min,lcl->tm_sec);
		bufp += strlen(bufp);
		}

	sprintf(bufp, "%5lu file=%s, line=%d, ",
		m->order,m->file,m->line);
	bufp += strlen(bufp);

	if (options & V_CRYPTO_MDEBUG_THREAD)
		{
		sprintf(bufp, "thread=%lu, ", m->thread);
		bufp += strlen(bufp);
		}

	sprintf(bufp, "number=%d, address=%08lX\n",
		m->num,(unsigned long)m->addr);
	bufp += strlen(bufp);

	BIO_puts(l->bio,buf);
	
	l->chunks++;
	l->bytes+=m->num;

	amip=m->app_info;
	ami_cnt=0;
	if (!amip)
		return;
	ti=amip->thread;
	
	do
		{
		int buf_len;
		int info_len;

		ami_cnt++;
		memset(buf,'>',ami_cnt);
		sprintf(buf + ami_cnt,
			" thread=%lu, file=%s, line=%d, info=\"",
			amip->thread, amip->file, amip->line);
		buf_len=strlen(buf);
		info_len=strlen(amip->info);
		if (128 - buf_len - 3 < info_len)
			{
			memcpy(buf + buf_len, amip->info, 128 - buf_len - 3);
			buf_len = 128 - 3;
			}
		else
			{
			strcpy(buf + buf_len, amip->info);
			buf_len = strlen(buf);
			}
		sprintf(buf + buf_len, "\"\n");
		
		BIO_puts(l->bio,buf);

		amip = amip->next;
		}
	while(amip && amip->thread == ti);
		
#ifdef LEVITTE_DEBUG_MEM
	if (amip)
		{
		fprintf(stderr, "Thread switch detected in backtrace!!!!\n");
		abort();
		}
#endif
	}

static IMPLEMENT_LHASH_DOALL_ARG_FN(print_leak, const MEM *, MEM_LEAK *)

void CRYPTO_mem_leaks(BIO *b)
	{
	MEM_LEAK ml;
	char buf[80];

	if (mh == NULL && amih == NULL)
		return;

	MemCheck_off(); /* obtain MALLOC2 lock */

	ml.bio=b;
	ml.bytes=0;
	ml.chunks=0;
	if (mh != NULL)
		lh_doall_arg(mh, LHASH_DOALL_ARG_FN(print_leak),
				(char *)&ml);
	if (ml.chunks != 0)
		{
		sprintf(buf,"%ld bytes leaked in %d chunks\n",
			ml.bytes,ml.chunks);
		BIO_puts(b,buf);
		}
	else
		{
		/* Make sure that, if we found no leaks, memory-leak debugging itself
		 * does not introduce memory leaks (which might irritate
		 * external debugging tools).
		 * (When someone enables leak checking, but does not call
		 * this function, we declare it to be their fault.)
		 *
		 * XXX    This should be in CRYPTO_mem_leaks_cb,
		 * and CRYPTO_mem_leaks should be implemented by
		 * using CRYPTO_mem_leaks_cb.
		 * (Also their should be a variant of lh_doall_arg
		 * that takes a function pointer instead of a void *;
		 * this would obviate the ugly and illegal
		 * void_fn_to_char kludge in CRYPTO_mem_leaks_cb.
		 * Otherwise the code police will come and get us.)
		 */
		int old_mh_mode;

		CRYPTO_w_lock(CRYPTO_LOCK_MALLOC);

		/* avoid deadlock when lh_free() uses CRYPTO_dbg_free(),
		 * which uses CRYPTO_is_mem_check_on */
		old_mh_mode = mh_mode;
		mh_mode = CRYPTO_MEM_CHECK_OFF;

		if (mh != NULL)
			{
			lh_free(mh);
			mh = NULL;
			}
		if (amih != NULL)
			{
			if (lh_num_items(amih) == 0) 
				{
				lh_free(amih);
				amih = NULL;
				}
			}

		mh_mode = old_mh_mode;
		CRYPTO_w_unlock(CRYPTO_LOCK_MALLOC);
		}
	MemCheck_on(); /* release MALLOC2 lock */
	}

#ifndef OPENSSL_NO_FP_API
void CRYPTO_mem_leaks_fp(FILE *fp)
	{
	BIO *b;

	if (mh == NULL) return;
	/* Need to turn off memory checking when allocated BIOs ... especially
	 * as we're creating them at a time when we're trying to check we've not
	 * left anything un-free()'d!! */
	MemCheck_off();
	if ((b=BIO_new(BIO_s_file())) == NULL)
		return;
	MemCheck_on();
	BIO_set_fp(b,fp,BIO_NOCLOSE);
	CRYPTO_mem_leaks(b);
	BIO_free(b);
	}
#endif



/* FIXME: We really don't allow much to the callback.  For example, it has
   no chance of reaching the info stack for the item it processes.  Should
   it really be this way?  -- Richard Levitte */
/* NB: The prototypes have been typedef'd to CRYPTO_MEM_LEAK_CB inside crypto.h
 * If this code is restructured, remove the callback type if it is no longer
 * needed. -- Geoff Thorpe */
static void cb_leak(const MEM *m, CRYPTO_MEM_LEAK_CB **cb)
	{
	(**cb)(m->order,m->file,m->line,m->num,m->addr);
	}

static IMPLEMENT_LHASH_DOALL_ARG_FN(cb_leak, const MEM *, CRYPTO_MEM_LEAK_CB **)

void CRYPTO_mem_leaks_cb(CRYPTO_MEM_LEAK_CB *cb)
	{
	if (mh == NULL) return;
	CRYPTO_w_lock(CRYPTO_LOCK_MALLOC2);
	lh_doall_arg(mh, LHASH_DOALL_ARG_FN(cb_leak), &cb);
	CRYPTO_w_unlock(CRYPTO_LOCK_MALLOC2);
	}
