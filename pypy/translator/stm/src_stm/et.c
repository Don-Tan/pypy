/* -*- c-basic-offset: 2 -*- */

#ifndef PYPY_NOT_MAIN_FILE

/* XXX assumes that time never wraps around (in a 'long'), which may be
 * correct on 64-bit machines but not on 32-bit machines if the process
 * runs for long enough.
 *
 * XXX measure the overhead of the global_timestamp
 */

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

#define USE_PTHREAD_MUTEX    /* optional */
#ifdef USE_PTHREAD_MUTEX
# include <pthread.h>
#endif

#include "src_stm/et.h"
#include "src_stm/atomic_ops.h"

#ifdef PYPY_STANDALONE         /* obscure: cannot include debug_print.h if compiled */
# define RPY_STM_DEBUG_PRINT   /* via ll2ctypes; only include it in normal builds */
# include "src/debug_print.h"
#endif

/************************************************************/

#define IS_LOCKED(num)  ((num) < 0)
#define IS_LOCKED_OR_NEWER(num, max_age) \
  __builtin_expect(((unsigned long)(num)) > ((unsigned long)(max_age)), 0)
typedef long owner_version_t;

typedef volatile owner_version_t orec_t;

/*** Specify the number of orecs in the global array. */
#define NUM_STRIPES  1048576

/*** declare the table of orecs */
static char orecs[NUM_STRIPES * sizeof(orec_t)];

/*** map addresses to orec table entries */
inline static orec_t *get_orec(void* addr)
{
  unsigned long index = (unsigned long)addr;
#ifdef RPY_STM_ASSERT
  assert(!(index & (sizeof(orec_t)-1)));
#endif
  char *p = orecs + (index & ((NUM_STRIPES-1) * sizeof(orec_t)));
  return (orec_t *)p;
}

#include "src_stm/lists.c"

/************************************************************/

#define ABORT_REASONS 8
#define SPINLOOP_REASONS 10
#define OTHERINEV_REASONS 5

struct tx_descriptor {
  void *rpython_tls_object;
  jmp_buf *setjmp_buf;
  owner_version_t start_time;
  owner_version_t end_time;
  unsigned long last_known_global_timestamp;
  struct OrecList reads;
  unsigned num_commits;
  unsigned num_aborts[ABORT_REASONS];
  unsigned num_spinloops[SPINLOOP_REASONS];
  unsigned int spinloop_counter;
  int transaction_active;
  owner_version_t my_lock_word;
  struct RedoLog redolog;   /* last item, because it's the biggest one */
};

static const struct tx_descriptor null_tx = {
  .transaction_active = 0,
  .my_lock_word = 0
};
#define NULL_TX  ((struct tx_descriptor *)(&null_tx))

/* global_timestamp contains in its lowest bit a flag equal to 1
   if there is an inevitable transaction running */
static volatile unsigned long global_timestamp = 2;
static __thread struct tx_descriptor *thread_descriptor = NULL_TX;

/************************************************************/

static unsigned long get_global_timestamp(struct tx_descriptor *d)
{
  return (d->last_known_global_timestamp = global_timestamp);
}

static _Bool change_global_timestamp(struct tx_descriptor *d,
                                     unsigned long old,
                                     unsigned long new)
{
  if (bool_cas(&global_timestamp, old, new))
    {
      d->last_known_global_timestamp = new;
      return 1;
    }
  return 0;
}

static void set_global_timestamp(struct tx_descriptor *d, unsigned long new)
{
  global_timestamp = new;
  d->last_known_global_timestamp = new;
}

static void tx_abort(int);

static void tx_spinloop(int num)
{
  unsigned int c;
  int i;
  struct tx_descriptor *d = thread_descriptor;
  d->num_spinloops[num]++;

  //printf("tx_spinloop(%d)\n", num);
  
  c = d->spinloop_counter;
  d->spinloop_counter = c * 9;
  i = c & 0xff0000;
  while (i >= 0) {
    spinloop();
    i -= 0x10000;
  }
}

static _Bool is_inevitable_or_inactive(struct tx_descriptor *d)
{
  return d->setjmp_buf == NULL;
}

static _Bool is_inevitable(struct tx_descriptor *d)
{
  assert(d->transaction_active);
  return is_inevitable_or_inactive(d);
}

/*** run the redo log to commit a transaction, and release the locks */
static void tx_redo(struct tx_descriptor *d)
{
  abort();
#if 0
  owner_version_t newver = d->end_time;
  wlog_t *item;
  /* loop in "forward" order: in this order, if there are duplicate orecs
     then only the last one has p != -1. */
  REDOLOG_LOOP_FORWARD(d->redolog, item)
    {
      *item->addr = item->val;
      /* but we must only unlock the orec if it's the last time it
         appears in the redolog list.  If it's not, then p == -1. */
      if (item->p != -1)
        {
          orec_t* o = get_orec(item->addr);
          CFENCE;
          *o = newver;
        }
    } REDOLOG_LOOP_END;
#endif
}

/*** on abort, release locks and restore the old version number. */
static void releaseAndRevertLocks(struct tx_descriptor *d)
{
  wlog_t *item;
  REDOLOG_LOOP_FORWARD(d->redolog, item)
    {
      if (item->p != -1)
        {
          orec_t* o = get_orec(item->addr);
          *o = item->p;
        }
    } REDOLOG_LOOP_END;
}

/*** release locks and restore the old version number, ready to retry later */
static void releaseLocksForRetry(struct tx_descriptor *d)
{
  wlog_t *item;
  REDOLOG_LOOP_FORWARD(d->redolog, item)
    {
      if (item->p != -1)
        {
          orec_t* o = get_orec(item->addr);
          *o = item->p;
          item->p = -1;
        }
    } REDOLOG_LOOP_END;
}

/*** lock all locations */
static void acquireLocks(struct tx_descriptor *d)
{
  wlog_t *item;
  // try to lock every location in the write set
  REDOLOG_LOOP_BACKWARD(d->redolog, item)
    {
      // get orec, read its version#
      orec_t* o = get_orec(item->addr);
      owner_version_t ovt;

    retry:
      ovt = *o;

      // if orec not locked, lock it
      //
      // NB: if ovt > start time, we may introduce inconsistent
      // reads.  Since most writes are also reads, we'll just abort under this
      // condition.  This can introduce false conflicts
      if (!IS_LOCKED_OR_NEWER(ovt, d->start_time)) {
        if (!bool_cas(o, ovt, d->my_lock_word))
          goto retry;
        // save old version to item->p.  Now we hold the lock.
        // in case of duplicate orecs, only the last one has p != -1.
        item->p = ovt;
      }
      // else if the location is too recent...
      else if (!IS_LOCKED(ovt))
        tx_abort(0);
      // else it is locked: if we don't hold the lock...
      else if (ovt != d->my_lock_word) {
        // we can either abort or spinloop.  Because we are at the end of
        // the transaction we might try to spinloop, even though after the
        // lock is released the ovt will be very recent, possibly
        // > d->start_time.  It is necessary to spinloop in case we are
        // inevitable, so use that as a criteria.  Another solution to avoid
        // deadlocks would be to sort the order in which we take the locks.
        if (is_inevitable(d))
          tx_spinloop(8);
        else
          tx_abort(6);
        goto retry;
      }
    } REDOLOG_LOOP_END;
}

static void common_cleanup(struct tx_descriptor *d)
{
  d->reads.size = 0;
  redolog_clear(&d->redolog);
  assert(d->transaction_active);
  d->transaction_active = 0;
  d->setjmp_buf = NULL;
}

static void tx_cleanup(struct tx_descriptor *d)
{
  // release the locks and restore version numbers
  releaseAndRevertLocks(d);
  // reset all lists
  common_cleanup(d);
}

static void tx_restart(struct tx_descriptor *d)
{
  jmp_buf *env = d->setjmp_buf;
  tx_cleanup(d);
  tx_spinloop(0);
  longjmp(*env, 1);
}

/*** increase the abort count and restart the transaction */
static void tx_abort(int reason)
{
  struct tx_descriptor *d = thread_descriptor;
  assert(!is_inevitable(d));
  d->num_aborts[reason]++;
#ifdef RPY_STM_DEBUG_PRINT
  PYPY_DEBUG_START("stm-abort");
  if (PYPY_HAVE_DEBUG_PRINTS)
      fprintf(PYPY_DEBUG_FILE, "thread %lx aborting %d\n",
                               (long)pthread_self(), reason);
  PYPY_DEBUG_STOP("stm-abort");
#endif
  tx_restart(d);
}

/**
 * fast-path validation, assuming that I don't hold locks.
 */
static void validate_fast(struct tx_descriptor *d, int lognum)
{
  int i;
  owner_version_t ovt;
  assert(!is_inevitable(d));
  for (i=0; i<d->reads.size; i++)
    {
    retry:
      ovt = *(d->reads.items[i]);
      if (IS_LOCKED_OR_NEWER(ovt, d->start_time))
        {
          // If locked, we wait until it becomes unlocked.  The chances are
          // that it will then have a very recent start_time, likely
          // > d->start_time, but it might still be better than always aborting
          if (IS_LOCKED(ovt))
            {
              tx_spinloop(lognum);  /* tx_spinloop(1), tx_spinloop(2),
                                       tx_spinloop(3) */
              goto retry;
            }
          else
            // abort if the timestamp is newer than my start time.  
            tx_abort(lognum);  /* tx_abort(1), tx_abort(2), tx_abort(3) */
        }
    }
}

/**
 * validate the read set by making sure that all orecs that we've read have
 * timestamps at least as old as our start time, unless we locked those orecs.
 */
static void validate(struct tx_descriptor *d)
{
  int i;
  owner_version_t ovt;
  assert(!is_inevitable(d));
  for (i=0; i<d->reads.size; i++)
    {
      ovt = *(d->reads.items[i]);      // read this orec
      if (IS_LOCKED_OR_NEWER(ovt, d->start_time))
        {
          if (!IS_LOCKED(ovt))
            // if unlocked and newer than start time, abort
            tx_abort(4);
          else
            {
              // if locked and not by me, abort
              if (ovt != d->my_lock_word)
                tx_abort(5);
            }
        }
    }
}

#ifdef USE_PTHREAD_MUTEX
/* mutex: only to avoid busy-looping too much in tx_spinloop() below */
static pthread_mutex_t mutex_inevitable = PTHREAD_MUTEX_INITIALIZER;
# ifdef RPY_STM_ASSERT
unsigned long locked_by = 0;
static void mutex_lock(void)
{
  unsigned long pself = (unsigned long)pthread_self();
  if (PYPY_HAVE_DEBUG_PRINTS) fprintf(PYPY_DEBUG_FILE,
                                      "%lx: mutex inev locking...\n", pself);
  assert(locked_by != pself);
  pthread_mutex_lock(&mutex_inevitable);
  locked_by = pself;
  if (PYPY_HAVE_DEBUG_PRINTS) fprintf(PYPY_DEBUG_FILE,
                                      "%lx: mutex inev locked\n", pself);
}
static void mutex_unlock(void)
{
  unsigned long pself = (unsigned long)pthread_self();
  locked_by = 0;
  if (PYPY_HAVE_DEBUG_PRINTS) fprintf(PYPY_DEBUG_FILE,
                                      "%lx: mutex inev unlocked\n", pself);
  pthread_mutex_unlock(&mutex_inevitable);
}
# else
#  define mutex_lock()    pthread_mutex_lock(&mutex_inevitable)
#  define mutex_unlock()  pthread_mutex_unlock(&mutex_inevitable)
# endif
#else
# define mutex_lock()     /* nothing */
# define mutex_unlock()   /* nothing */
#endif

static void wait_end_inevitability(struct tx_descriptor *d)
{
  unsigned long curts;
  releaseLocksForRetry(d);

  // We are going to wait until the other inevitable transaction
  // finishes.  XXX We could do better here: we could check if
  // committing 'd' would create a conflict for the other inevitable
  // thread 'd_inev' or not.  It requires peeking in 'd_inev' from this
  // thread (which we never do so far) in order to do something like
  // 'validate_fast(d_inev); d_inev->start_time = updated;'

  while ((curts = get_global_timestamp(d)) & 1)
    {
      // while we're about to wait anyway, we can do a validate_fast
      if (d->start_time < curts - 1)
        {
          validate_fast(d, 3);
          d->start_time = curts - 1;
        }
      tx_spinloop(4);
      mutex_lock();
      mutex_unlock();
    }
  acquireLocks(d);
}

static void commitInevitableTransaction(struct tx_descriptor *d)
{
  unsigned long ts;
  _Bool ok;

  // no-one else can modify global_timestamp if I'm inevitable
  // and d_inev_checking is 0
  ts = get_global_timestamp(d);
  assert(ts & 1);
  set_global_timestamp(d, ts + 1);
  d->end_time = ts + 1;
  assert(d->end_time == (d->start_time + 2));

  // run the redo log, and release the locks
  tx_redo(d);

  mutex_unlock();
}

/* lazy/lazy read instrumentation */
long stm_read_word(long* addr)
{
  struct tx_descriptor *d = thread_descriptor;
#ifdef RPY_STM_ASSERT
  assert((((long)addr) & (sizeof(void*)-1)) == 0);
#endif
  if (!d->transaction_active)
    return *addr;

  // check writeset first
  wlog_t* found;
  REDOLOG_FIND(d->redolog, addr, found, goto not_found);
  return found->val;

 not_found:;
  // get the orec addr
  orec_t* o = get_orec((void*)addr);
  owner_version_t ovt;

 retry:
  // read the orec BEFORE we read anything else
  ovt = *o;
  CFENCE;

  // this tx doesn't hold any locks, so if the lock for this addr is held,
  // there is contention.  A lock is never hold for too long, so spinloop
  // until it is released.
  if (IS_LOCKED_OR_NEWER(ovt, d->start_time))
    {
      if (IS_LOCKED(ovt)) {
        tx_spinloop(7);
        goto retry;
      }
      // else this location is too new, scale forward
      owner_version_t newts = get_global_timestamp(d) & ~1;
      validate_fast(d, 1);
      d->start_time = newts;
    }

  // orec is unlocked, with ts <= start_time.  read the location
  long tmp = *addr;

  // postvalidate AFTER reading addr:
  CFENCE;
  if (*o != ovt)
    goto retry;       /* oups, try again */

  oreclist_insert(&d->reads, o);

  return tmp;
}

void stm_write_word(long* addr, long val)
{
  struct tx_descriptor *d = thread_descriptor;
  assert((((long)addr) & (sizeof(void*)-1)) == 0);
  if (!d->transaction_active) {
    *addr = val;
    return;
  }
  redolog_insert(&d->redolog, addr, val);
}


static struct tx_descriptor *descriptor_init(void)
{
  assert(thread_descriptor == NULL_TX);
  if (1)  /* for hg diff */
    {
      struct tx_descriptor *d = malloc(sizeof(struct tx_descriptor));
      memset(d, 0, sizeof(struct tx_descriptor));

#ifdef RPY_STM_DEBUG_PRINT
      PYPY_DEBUG_START("stm-init");
#endif

      /* initialize 'my_lock_word' to be a unique negative number */
      d->my_lock_word = (owner_version_t)d;
      if (!IS_LOCKED(d->my_lock_word))
        d->my_lock_word = ~d->my_lock_word;
      assert(IS_LOCKED(d->my_lock_word));
      d->spinloop_counter = (unsigned int)(d->my_lock_word | 1);

      thread_descriptor = d;

#ifdef RPY_STM_DEBUG_PRINT
      if (PYPY_HAVE_DEBUG_PRINTS) fprintf(PYPY_DEBUG_FILE, "thread %lx starting\n",
                                          (long)pthread_self());
      PYPY_DEBUG_STOP("stm-init");
#endif
      return d;
    }
}

static void descriptor_done(void)
{
  struct tx_descriptor *d = thread_descriptor;
  assert(d != NULL_TX);

  thread_descriptor = NULL_TX;

#ifdef RPY_STM_DEBUG_PRINT
  PYPY_DEBUG_START("stm-done");
  if (PYPY_HAVE_DEBUG_PRINTS) {
    int num_aborts = 0, num_spinloops = 0;
    int i, prevchar;
    char line[256], *p = line;

    for (i=0; i<ABORT_REASONS; i++)
      num_aborts += d->num_aborts[i];
    for (i=0; i<SPINLOOP_REASONS; i++)
      num_spinloops += d->num_spinloops[i];

    p += sprintf(p, "thread %lx: %d commits, %d aborts\n",
                 (long)pthread_self(),
                 d->num_commits,
                 num_aborts);

    for (i=0; i<ABORT_REASONS; i++)
      p += sprintf(p, "%c%d", i == 0 ? '[' : ',',
                   d->num_aborts[i]);

    for (i=1; i<SPINLOOP_REASONS; i++)  /* num_spinloops[0] == num_aborts */
      p += sprintf(p, "%c%d", i == 1 ? '|' : ',',
                   d->num_spinloops[i]);

    p += sprintf(p, "]\n");
    fwrite(line, 1, p - line, PYPY_DEBUG_FILE);
  }
  PYPY_DEBUG_STOP("stm-done");
#endif

  free(d);
}

static void begin_transaction(jmp_buf* buf)
{
  struct tx_descriptor *d = thread_descriptor;
  assert(!d->transaction_active);
  d->transaction_active = 1;
  d->setjmp_buf = buf;
  d->start_time = d->last_known_global_timestamp & ~1;
}

static long commit_transaction(void)
{
  struct tx_descriptor *d = thread_descriptor;

  // if I don't have writes, I'm committed
  if (!redolog_any_entry(&d->redolog))
    {
      if (is_inevitable(d))
        {
          unsigned long ts = get_global_timestamp(d);
          assert(ts & 1);
          set_global_timestamp(d, ts - 1);
          mutex_unlock();
        }
      d->num_commits++;
      common_cleanup(d);
      return d->start_time;
    }

  // bring that variable over to this CPU core (optimization, maybe)
  global_timestamp;

  // acquire locks
  acquireLocks(d);

  if (is_inevitable(d))
    {
      commitInevitableTransaction(d);
    }
  else
    {
      while (1)
        {
          unsigned long expected = get_global_timestamp(d);
          if (expected & 1)
            {
              // wait until it is done.  hopefully we can then proceed
              // without conflicts.
              wait_end_inevitability(d);
              continue;
            }
          if (change_global_timestamp(d, expected, expected + 2))
            {
              d->end_time = expected + 2;
              break;
            }
        }

      // validate (but skip validation if nobody else committed)
      if (d->end_time != (d->start_time + 2))
        validate(d);

      // run the redo log, and release the locks
      tx_redo(d);
    }

  // remember that this was a commit
  d->num_commits++;

  // reset all lists
  common_cleanup(d);
  return d->end_time;
}

void* stm_perform_transaction(void*(*callback)(void*, long), void *arg)
{
  void *result;
  jmp_buf _jmpbuf;
  volatile long v_counter = 0;
  long counter;
  /* you need to call descriptor_init() before calling
     stm_perform_transaction() */
  assert(thread_descriptor != NULL_TX);
  setjmp(_jmpbuf);
  begin_transaction(&_jmpbuf);
  counter = v_counter;
  v_counter = counter + 1;
  result = callback(arg, counter);
  commit_transaction();
  return result;
}

void stm_try_inevitable(STM_CCHARP1(why))
{
  /* when a transaction is inevitable, its start_time is equal to
     global_timestamp and global_timestamp cannot be incremented
     by another thread.  We set the lowest bit in global_timestamp
     to 1. */
  struct tx_descriptor *d = thread_descriptor;
  if (!d->transaction_active)
    return;

#ifdef RPY_STM_ASSERT
  PYPY_DEBUG_START("stm-inevitable");
  if (PYPY_HAVE_DEBUG_PRINTS)
    {
      fprintf(PYPY_DEBUG_FILE, "%s%s\n", why,
              (!d->transaction_active) ? " (inactive)" :
              is_inevitable(d) ? " (already inevitable)" : "");
    }
#endif

  if (is_inevitable_or_inactive(d))
    {
#ifdef RPY_STM_ASSERT
      PYPY_DEBUG_STOP("stm-inevitable");
#endif
      return;  /* I am already inevitable, or not in a transaction at all */
    }

  while (1)
    {
      unsigned long curtime = get_global_timestamp(d);
      if (d->start_time != (curtime & ~1))
        {                             /* scale forward */
          validate_fast(d, 2);
          d->start_time = curtime & ~1;
        }
      mutex_lock();
      if (curtime & 1)   /* there is, or was, already an inevitable thread */
        {
          /* should we spinloop here, or abort (and likely come back
             in try_inevitable() very soon)?  unclear.  For now
             let's try to spinloop, after the waiting done by
             acquiring the mutex */
          mutex_unlock();
          tx_spinloop(6);
          continue;
        }
      if (change_global_timestamp(d, curtime, curtime + 1))
        break;
      mutex_unlock();
    }
  d->setjmp_buf = NULL;   /* inevitable from now on */
#ifdef RPY_STM_ASSERT
  PYPY_DEBUG_STOP("stm-inevitable");
#endif
}

void stm_abort_and_retry(void)
{
  tx_abort(7);     /* manual abort */
}

// XXX little-endian only!
#define READ_PARTIAL_WORD(T, fieldsize, addr)           \
  int misalignment = ((long)addr) & (sizeof(void*)-1);  \
  long *p = (long*)(((char *)addr) - misalignment);     \
  unsigned long word = stm_read_word(p);                \
  assert(sizeof(T) == fieldsize);                       \
  return (T)(word >> (misalignment * 8));

unsigned char stm_read_partial_1(void *addr) {
  READ_PARTIAL_WORD(unsigned char, 1, addr)
}
unsigned short stm_read_partial_2(void *addr) {
  READ_PARTIAL_WORD(unsigned short, 2, addr)
}
#if PYPY_LONG_BIT == 64
unsigned int stm_read_partial_4(void *addr) {
  READ_PARTIAL_WORD(unsigned int, 4, addr)
}
#endif

// XXX little-endian only!
#define WRITE_PARTIAL_WORD(fieldsize, addr, nval)                       \
  int misalignment = ((long)addr) & (sizeof(void*)-1);                  \
  long *p = (long*)(((char *)addr) - misalignment);                     \
  long val = ((long)nval) << (misalignment * 8);                        \
  long word = stm_read_word(p);                                         \
  long mask = ((1L << (fieldsize * 8)) - 1) << (misalignment * 8);      \
  val = (val & mask) | (word & ~mask);                                  \
  stm_write_word(p, val);

void stm_write_partial_1(void *addr, unsigned char nval) {
  WRITE_PARTIAL_WORD(1, addr, nval)
}
void stm_write_partial_2(void *addr, unsigned short nval) {
  WRITE_PARTIAL_WORD(2, addr, nval)
}
#if PYPY_LONG_BIT == 64
void stm_write_partial_4(void *addr, unsigned int nval) {
  WRITE_PARTIAL_WORD(4, addr, nval)
}
#endif


#if PYPY_LONG_BIT == 32
long long stm_read_doubleword(long *addr)
{
  /* 32-bit only */
  unsigned long res0 = (unsigned long)stm_read_word(addr);
  unsigned long res1 = (unsigned long)stm_read_word(addr + 1);
  return (((unsigned long long)res1) << 32) | res0;
}

void stm_write_doubleword(long *addr, long long val)
{
  /* 32-bit only */
  stm_write_word(addr, (long)val);
  stm_write_word(addr + 1, (long)(val >> 32));
}
#endif

double stm_read_double(long *addr)
{
  long long x;
  double dd;
#if PYPY_LONG_BIT == 32
  x = stm_read_doubleword(addr);   /* 32 bits */
#else
  x = stm_read_word(addr);         /* 64 bits */
#endif
  assert(sizeof(double) == 8 && sizeof(long long) == 8);
  memcpy(&dd, &x, 8);
  return dd;
}

void stm_write_double(long *addr, double val)
{
  long long ll;
  assert(sizeof(double) == 8 && sizeof(long long) == 8);
  memcpy(&ll, &val, 8);
#if PYPY_LONG_BIT == 32
  stm_write_doubleword(addr, ll);   /* 32 bits */
#else
  stm_write_word(addr, ll);         /* 64 bits */
#endif
}

float stm_read_float(long *addr)
{
  unsigned int x;
  float ff;
#if PYPY_LONG_BIT == 32
  x = stm_read_word(addr);         /* 32 bits */
#else
  if (((long)(char*)addr) & 7) {
    addr = (long *)(((char *)addr) - 4);
    x = (unsigned int)(stm_read_word(addr) >> 32);   /* 64 bits, unaligned */
  }
  else
    x = (unsigned int)stm_read_word(addr);           /* 64 bits, aligned */
#endif
  assert(sizeof(float) == 4 && sizeof(unsigned int) == 4);
  memcpy(&ff, &x, 4);
  return ff;
}

void stm_write_float(long *addr, float val)
{
  unsigned int ii;
  assert(sizeof(float) == 4 && sizeof(unsigned int) == 4);
  memcpy(&ii, &val, 4);
#if PYPY_LONG_BIT == 32
  stm_write_word(addr, ii);         /* 32 bits */
#else
  stm_write_partial_4(addr, ii);    /* 64 bits */
#endif
}

long stm_debug_get_state(void)
{
  struct tx_descriptor *d = thread_descriptor;
  if (d == NULL_TX)
    return -1;
  if (!d->transaction_active)
    return 0;
  if (!is_inevitable(d))
    return 1;
  else
    return 2;
}

long stm_thread_id(void)
{
  struct tx_descriptor *d = thread_descriptor;
  return d->my_lock_word;
}


void stm_set_tls(void *newtls)
{
  descriptor_init()->rpython_tls_object = newtls;
}

void *stm_get_tls(void)
{
  return thread_descriptor->rpython_tls_object;
}

void stm_del_tls(void)
{
  descriptor_done();
}

void *stm_tldict_lookup(void *key)
{
  struct tx_descriptor *d = thread_descriptor;
  wlog_t* found;
  REDOLOG_FIND(d->redolog, key, found, goto not_found);
  return found->val;

 not_found:
  return NULL;
}

void stm_tldict_add(void *key, void *value)
{
  struct tx_descriptor *d = thread_descriptor;
  redolog_insert(&d->redolog, key, value);
}

#endif  /* PYPY_NOT_MAIN_FILE */
