/*
 * Copyright (C) 2015 Red Hat. All rights reserved.
 *
 * This file is released under the GPL.
 */

#include "dm-cache-policy.h"
#include "dm-cache-policy-internal.h"
#include "dm.h"

#include <linux/hash.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include <linux/math64.h>

#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/Bitmap.h>

// #include <linux/Bitmap.h>

//#include <linux/dm_vssd.h>
// extern struct cache *cache_global;
// struct io_tracker {
// 	spinlock_t lock;

// 	/*
// 	 * Sectors of in-flight IO.
// 	 */
// 	sector_t in_flight;

// 	/*
// 	 * The time, in jiffies, when this device became idle (if it is
// 	 * indeed idle).
// 	 */
// 	unsigned long idle_time;
// 	unsigned long last_update_time;
// };
// enum cache_metadata_mode {
// 	CM_WRITE,		/* metadata may be changed */
// 	CM_READ_ONLY,		/* metadata may not be changed */
// 	CM_FAIL
// };

// enum cache_io_mode {
	
// 	 * Data is written to cached blocks only.  These blocks are marked
// 	 * dirty.  If you lose the cache device you will lose data.
// 	 * Potential performance increase for both reads and writes.
	 
// 	CM_IO_WRITEBACK,

// 	/*
// 	 * Data is written to both cache and origin.  Blocks are never
// 	 * dirty.  Potential performance benfit for reads only.
// 	 */
// 	CM_IO_WRITETHROUGH,

// 	/*
// 	 * A degraded mode useful for various cache coherency situations
// 	 * (eg, rolling back snapshots).  Reads and writes always go to the
// 	 * origin.  If a write goes to a cached oblock, then the cache
// 	 * block is invalidated.
// 	 */
// 	CM_IO_PASSTHROUGH
// };

// struct cache_features {
// 	enum cache_metadata_mode mode;
// 	enum cache_io_mode io_mode;
// };

// struct cache_stats {
// 	atomic_t read_hit;
// 	atomic_t read_miss;
// 	atomic_t write_hit;
// 	atomic_t write_miss;
// 	atomic_t demotion;
// 	atomic_t promotion;
// 	atomic_t copies_avoided;
// 	atomic_t cache_cell_clash;
// 	atomic_t commit_count;
// 	atomic_t discard_count;
// };

// struct cache {
// 	struct dm_target *ti;
// 	struct dm_target_callbacks callbacks;

// 	struct dm_cache_metadata *cmd;

// 	/*
// 	 * Metadata is written to this device.
// 	 */
// 	struct dm_dev *metadata_dev;

// 	/*
// 	 * The slower of the two data devices.  Typically a spindle.
// 	 */
// 	struct dm_dev *origin_dev;

// 	/*
// 	 * The faster of the two data devices.  Typically an SSD.
// 	 */
// 	struct dm_dev *cache_dev;

// 	/*
// 	 * Size of the origin device in _complete_ blocks and native sectors.
// 	 */
// 	dm_oblock_t origin_blocks;
// 	sector_t origin_sectors;

// 	/*
// 	 * Size of the cache device in blocks.
// 	 */
// 	dm_cblock_t cache_size;

// 	/*
// 	 * Fields for converting from sectors to blocks.
// 	 */
// 	sector_t sectors_per_block;
// 	int sectors_per_block_shift;

// 	spinlock_t lock;
// 	struct list_head deferred_cells;
// 	struct bio_list deferred_bios;
// 	struct bio_list deferred_flush_bios;
// 	struct bio_list deferred_writethrough_bios;
// 	struct list_head quiesced_migrations;
// 	struct list_head completed_migrations;
// 	struct list_head need_commit_migrations;
// 	sector_t migration_threshold;
// 	wait_queue_head_t migration_wait;
// 	atomic_t nr_allocated_migrations;

// 	/*
// 	 * The number of in flight migrations that are performing
// 	 * background io. eg, promotion, writeback.
// 	 */
// 	atomic_t nr_io_migrations;

// 	wait_queue_head_t quiescing_wait;
// 	atomic_t quiescing;
// 	atomic_t quiescing_ack;

// 	/*
// 	 * cache_size entries, dirty if set
// 	 */
// 	atomic_t nr_dirty;
// 	unsigned long *dirty_bitset;

// 	/*
// 	 * origin_blocks entries, discarded if set.
// 	 */
// 	dm_dblock_t discard_nr_blocks;
// 	unsigned long *discard_bitset;
// 	uint32_t discard_block_size; /* a power of 2 times sectors per block */

// 	/*
// 	 * Rather than reconstructing the table line for the status we just
// 	 * save it and regurgitate.
// 	 */
// 	unsigned nr_ctr_args;
// 	const char **ctr_args;

// 	struct dm_kcopyd_client *copier;
// 	struct workqueue_struct *wq;
// 	struct work_struct worker;

// 	struct delayed_work waker;
// 	unsigned long last_commit_jiffies;

// 	struct dm_bio_prison *prison;
// 	struct dm_deferred_set *all_io_ds;

// 	mempool_t *migration_pool;

// 	struct dm_cache_policy *policy;
// 	unsigned policy_nr_args;

// 	bool need_tick_bio:1;
// 	bool sized:1;
// 	bool invalidate:1;
// 	bool commit_requested:1;
// 	bool loaded_mappings:1;
// 	bool loaded_discards:1;

// 	/*
// 	 * Cache features such as write-through.
// 	 */
// 	struct cache_features features;

// 	struct cache_stats stats;

// 	/*
// 	 * Invalidation fields.
// 	 */
// 	spinlock_t invalidation_lock;
// 	struct list_head invalidation_requests;

// 	struct io_tracker origin_tracker;
// };

#define DM_MSG_PREFIX "cache-policy-smq"

/*ADDED BY PRAFULL...BEGIN ADD..*/
struct smq_policy *mq1=NULL;
struct policy_locker *locker1;
extern unsigned long (*new_policy)(int count,int flag,int *buf);
extern unsigned long (*get_stats)(int *);
extern int register_vssd(int *count,int *buf);

atomic_t hits = ATOMIC_INIT(0);
atomic_t miss = ATOMIC_INIT(0);
atomic_t requests = ATOMIC_INIT(0);
//unsigned long (*register_vssd)(int count,int flag,int *buf) = NULL;
//EXPORT_SYMBOL(register_vssd);


extern struct bitmap_t * register_dm_cache(int);


//extern unsigned long (*dealloc_blocks)(struct smq_policy *mq,int *buf);
//extern unsigned long do_resize(int count,int flag,int *buf);
/*ADDED BY PRAFULL...END ADD..*/

/*----------------------------------------------------------------*/

/*
 * Safe division functions that return zero on divide by zero.
 */
static unsigned safe_div(unsigned n, unsigned d)
{
	return d ? n / d : 0u;
}

static unsigned safe_mod(unsigned n, unsigned d)
{
	return d ? n % d : 0u;
}

/*----------------------------------------------------------------*/

struct entry {
	unsigned hash_next:28;
	unsigned prev:28;
	unsigned next:28;
	unsigned level:7;
	bool dirty:1;
	bool allocated:1;
	bool sentinel:1;

	dm_oblock_t oblock;

	int group_id;
};

/*----------------------------------------------------------------*/

#define INDEXER_NULL ((1u << 28u) - 1u)

/*
 * An entry_space manages a set of entries that we use for the queues.
 * The clean and dirty queues share entries, so this object is separate
 * from the queue itself.
 */
struct entry_space {
	struct entry *begin;
	struct entry *end;
};

static int space_init(struct entry_space *es, unsigned nr_entries)
{
	if (!nr_entries) {
		es->begin = es->end = NULL;
		return 0;
	}

	es->begin = vzalloc(sizeof(struct entry) * nr_entries);
	if (!es->begin)
		return -ENOMEM;

	es->end = es->begin + nr_entries;
	return 0;
}

static void space_exit(struct entry_space *es)
{
	vfree(es->begin);
}

static struct entry *__get_entry(struct entry_space *es, unsigned block)
{
	struct entry *e;

	e = es->begin + block;
	BUG_ON(e >= es->end);

	return e;
}

static unsigned to_index(struct entry_space *es, struct entry *e)
{
	BUG_ON(e < es->begin || e >= es->end);
	return e - es->begin;
}

static struct entry *to_entry(struct entry_space *es, unsigned block)
{
	if (block == INDEXER_NULL)
		return NULL;

	return __get_entry(es, block);
}

/*----------------------------------------------------------------*/

struct ilist {
	unsigned nr_elts;	/* excluding sentinel entries */
	unsigned head, tail;
};

static void l_init(struct ilist *l)
{
	l->nr_elts = 0;
	l->head = l->tail = INDEXER_NULL;
}

static struct entry *l_head(struct entry_space *es, struct ilist *l)
{
	return to_entry(es, l->head);
}

static struct entry *l_tail(struct entry_space *es, struct ilist *l)
{
	return to_entry(es, l->tail);
}

static struct entry *l_next(struct entry_space *es, struct entry *e)
{
	return to_entry(es, e->next);
}

static struct entry *l_prev(struct entry_space *es, struct entry *e)
{
	return to_entry(es, e->prev);
}

static bool l_empty(struct ilist *l)
{
	return l->head == INDEXER_NULL;
}

static void l_add_head(struct entry_space *es, struct ilist *l, struct entry *e)
{
	struct entry *head = l_head(es, l);

	e->next = l->head;
	e->prev = INDEXER_NULL;

	if (head)
		head->prev = l->head = to_index(es, e);
	else
		l->head = l->tail = to_index(es, e);

	if (!e->sentinel)
		l->nr_elts++;
}

static void l_add_tail(struct entry_space *es, struct ilist *l, struct entry *e)
{
	struct entry *tail = l_tail(es, l);

	e->next = INDEXER_NULL;
	e->prev = l->tail;

	if (tail)
		tail->next = l->tail = to_index(es, e);
	else
		l->head = l->tail = to_index(es, e);

	if (!e->sentinel)
		l->nr_elts++;
}

static void l_add_before(struct entry_space *es, struct ilist *l,
		struct entry *old, struct entry *e)
{
	struct entry *prev = l_prev(es, old);

	if (!prev)
		l_add_head(es, l, e);

	else {
		e->prev = old->prev;
		e->next = to_index(es, old);
		prev->next = old->prev = to_index(es, e);

		if (!e->sentinel)
			l->nr_elts++;
	}
}

static void l_del(struct entry_space *es, struct ilist *l, struct entry *e)
{
	struct entry *prev = l_prev(es, e);
	struct entry *next = l_next(es, e);

	if (prev)
		prev->next = e->next;
	else
		l->head = e->next;

	if (next)
		next->prev = e->prev;
	else
		l->tail = e->prev;

	if (!e->sentinel)
		l->nr_elts--;
}

static struct entry *l_pop_tail(struct entry_space *es, struct ilist *l)
{
	struct entry *e;

	for (e = l_tail(es, l); e; e = l_prev(es, e))
		if (!e->sentinel) {
			l_del(es, l, e);
			return e;
		}

	return NULL;
}

/*----------------------------------------------------------------*/

/*
 * The stochastic-multi-queue is a set of lru lists stacked into levels.
 * Entries are moved up levels when they are used, which loosely orders the
 * most accessed entries in the top levels and least in the bottom.  This
 * structure is *much* better than a single lru list.
 */
#define MAX_LEVELS 64u

struct queue {
	struct entry_space *es;

	unsigned nr_elts;
	unsigned nr_levels;
	struct ilist qs[MAX_LEVELS];

	/*
	 * We maintain a count of the number of entries we would like in each
	 * level.
	 */
	unsigned last_target_nr_elts;
	unsigned nr_top_levels;
	unsigned nr_in_top_levels;
	unsigned target_count[MAX_LEVELS];
};

static void q_init(struct queue *q, struct entry_space *es, unsigned nr_levels)
{
	unsigned i;

	q->es = es;
	q->nr_elts = 0;
	q->nr_levels = nr_levels;

	for (i = 0; i < q->nr_levels; i++) {
		l_init(q->qs + i);
		q->target_count[i] = 0u;
	}

	q->last_target_nr_elts = 0u;
	q->nr_top_levels = 0u;
	q->nr_in_top_levels = 0u;
}

static unsigned q_size(struct queue *q)
{
	return q->nr_elts;
}

/*
 * Insert an entry to the back of the given level.
 */
static void q_push(struct queue *q, struct entry *e)
{
	if (!e->sentinel)
		q->nr_elts++;

	l_add_tail(q->es, q->qs + e->level, e);
}

static void q_push_before(struct queue *q, struct entry *old, struct entry *e)
{
	if (!e->sentinel)
		q->nr_elts++;

	l_add_before(q->es, q->qs + e->level, old, e);
}

static void q_del(struct queue *q, struct entry *e)
{
	l_del(q->es, q->qs + e->level, e);
	if (!e->sentinel)
		q->nr_elts--;
}

/*
 * Return the oldest entry of the lowest populated level.
 */
static struct entry *q_peek(struct queue *q, unsigned max_level, bool can_cross_sentinel)
{
	unsigned level;
	struct entry *e;

	max_level = min(max_level, q->nr_levels);

	for (level = 0; level < max_level; level++)
		for (e = l_head(q->es, q->qs + level); e; e = l_next(q->es, e)) {
			if (e->sentinel) {
				if (can_cross_sentinel)
					continue;
				else
					break;
			}

			return e;
		}

	return NULL;
}

static struct entry *q_peek_symflex(struct queue *q, unsigned max_level, bool can_cross_sentinel, struct app_group_t *current_group)
{
	unsigned level;
	struct entry *e;

	max_level = min(max_level, q->nr_levels);

	for (level = 0; level < max_level; level++)
	{
		for (e = l_head(q->es, q->qs + level); e; e = l_next(q->es, e)) 
		{
			if (e->sentinel) {
				if (can_cross_sentinel)
					continue;
				else
					break;
			}

			if(current_group == NULL)
				return e;

			if(e->group_id == current_group->id)
				return e;
		}
	}

	return NULL;
}

static struct entry *q_pop(struct queue *q)
{
	struct entry *e = q_peek(q, q->nr_levels, true);

	if (e)
		q_del(q, e);

	return e;
}

/*
 * Pops an entry from a level that is not past a sentinel.
 */
static struct entry *q_pop_old(struct queue *q, unsigned max_level)
{
	struct entry *e = q_peek(q, max_level, false);

	if (e)
		q_del(q, e);

	return e;
}

/*
 * This function assumes there is a non-sentinel entry to pop.  It's only
 * used by redistribute, so we know this is true.  It also doesn't adjust
 * the q->nr_elts count.
 */
static struct entry *__redist_pop_from(struct queue *q, unsigned level)
{
	struct entry *e;

	for (; level < q->nr_levels; level++)
		for (e = l_head(q->es, q->qs + level); e; e = l_next(q->es, e))
			if (!e->sentinel) {
				l_del(q->es, q->qs + e->level, e);
				return e;
			}

	return NULL;
}

static void q_set_targets_subrange_(struct queue *q, unsigned nr_elts, unsigned lbegin, unsigned lend)
{
	unsigned level, nr_levels, entries_per_level, remainder;

	BUG_ON(lbegin > lend);
	BUG_ON(lend > q->nr_levels);
	nr_levels = lend - lbegin;
	entries_per_level = safe_div(nr_elts, nr_levels);
	remainder = safe_mod(nr_elts, nr_levels);

	for (level = lbegin; level < lend; level++)
		q->target_count[level] =
			(level < (lbegin + remainder)) ? entries_per_level + 1u : entries_per_level;
}

/*
 * Typically we have fewer elements in the top few levels which allows us
 * to adjust the promote threshold nicely.
 */
static void q_set_targets(struct queue *q)
{
	if (q->last_target_nr_elts == q->nr_elts)
		return;

	q->last_target_nr_elts = q->nr_elts;

	if (q->nr_top_levels > q->nr_levels)
		q_set_targets_subrange_(q, q->nr_elts, 0, q->nr_levels);

	else {
		q_set_targets_subrange_(q, q->nr_in_top_levels,
				q->nr_levels - q->nr_top_levels, q->nr_levels);

		if (q->nr_in_top_levels < q->nr_elts)
			q_set_targets_subrange_(q, q->nr_elts - q->nr_in_top_levels,
					0, q->nr_levels - q->nr_top_levels);
		else
			q_set_targets_subrange_(q, 0, 0, q->nr_levels - q->nr_top_levels);
	}
}

static void q_redistribute(struct queue *q)
{
	unsigned target, level;
	struct ilist *l, *l_above;
	struct entry *e;

	q_set_targets(q);

	for (level = 0u; level < q->nr_levels - 1u; level++) {
		l = q->qs + level;
		target = q->target_count[level];

		/*
		 * Pull down some entries from the level above.
		 */
		while (l->nr_elts < target) {
			e = __redist_pop_from(q, level + 1u);
			if (!e) {
				/* bug in nr_elts */
				break;
			}

			e->level = level;
			l_add_tail(q->es, l, e);
		}

		/*
		 * Push some entries up.
		 */
		l_above = q->qs + level + 1u;
		while (l->nr_elts > target) {
			e = l_pop_tail(q->es, l);

			if (!e)
				/* bug in nr_elts */
				break;

			e->level = level + 1u;
			l_add_head(q->es, l_above, e);
		}
	}
}

static void q_requeue_before(struct queue *q, struct entry *dest, struct entry *e, unsigned extra_levels)
{
	struct entry *de;
	unsigned new_level;

	q_del(q, e);

	if (extra_levels && (e->level < q->nr_levels - 1u)) {
		new_level = min(q->nr_levels - 1u, e->level + extra_levels);
		for (de = l_head(q->es, q->qs + new_level); de; de = l_next(q->es, de)) {
			if (de->sentinel)
				continue;

			q_del(q, de);
			de->level = e->level;

			if (dest)
				q_push_before(q, dest, de);
			else
				q_push(q, de);
			break;
		}

		e->level = new_level;
	}

	q_push(q, e);
}

static void q_requeue(struct queue *q, struct entry *e, unsigned extra_levels)
{
	q_requeue_before(q, NULL, e, extra_levels);
}

/*----------------------------------------------------------------*/

#define FP_SHIFT 8
#define SIXTEENTH (1u << (FP_SHIFT - 4u))
#define EIGHTH (1u << (FP_SHIFT - 3u))

struct stats {
	unsigned hit_threshold;
	unsigned hits;
	unsigned misses;
};

enum performance {
	Q_POOR,
	Q_FAIR,
	Q_WELL
};

static void stats_init(struct stats *s, unsigned nr_levels)
{
	s->hit_threshold = (nr_levels * 3u) / 4u;
	s->hits = 0u;
	s->misses = 0u;
}

static void stats_reset(struct stats *s)
{
	//	printk(KERN_ALERT"Cache statistics reset....\n");
	s->hits = s->misses = 0u;
}

static void stats_level_accessed(struct stats *s, unsigned level)
{
	if (level >= s->hit_threshold)
		s->hits++;
	else
		s->misses++;
}
/*static void stats_level_accessed_resize(struct stats *s, unsigned level)
  {
  if (level >= s->hit_threshold)
  atomic_inc(&hits);
  else
  atomic_inc(&miss);
  }*/
static void stats_miss(struct stats *s)
{
	s->misses++;
}
// static void stats_miss_symflex(struct stats *s, struct stats *app_group_status)
// {
// 	s->misses++;
// 	app_group_status->misses++;
// }



/*
 * There are times when we don't have any confidence in the hotspot queue.
 * Such as when a fresh cache is created and the blocks have been spread
 * out across the levels, or if an io load changes.  We detect this by
 * seeing how often a lookup is in the top levels of the hotspot queue.
 */
static enum performance stats_assess(struct stats *s)
{
	unsigned confidence = safe_div(s->hits << FP_SHIFT, s->hits + s->misses);
	//printk(KERN_ALERT"dmcache: My confidence is %u\n",confidence);

	if (confidence < SIXTEENTH)
		return Q_POOR;

	else if (confidence < EIGHTH)
		return Q_FAIR;

	else
		return Q_WELL;
}

/*----------------------------------------------------------------*/

struct hash_table {
	struct entry_space *es;
	unsigned long long hash_bits;
	unsigned *buckets;
};

/*
 * All cache entries are stored in a chained hash table.  To save space we
 * use indexing again, and only store indexes to the next entry.
 */
static int h_init(struct hash_table *ht, struct entry_space *es, unsigned nr_entries)
{
	unsigned i, nr_buckets;

	ht->es = es;
	nr_buckets = roundup_pow_of_two(max(nr_entries / 4u, 16u));
	ht->hash_bits = __ffs(nr_buckets);

	ht->buckets = vmalloc(sizeof(*ht->buckets) * nr_buckets);
	if (!ht->buckets)
		return -ENOMEM;

	for (i = 0; i < nr_buckets; i++)
		ht->buckets[i] = INDEXER_NULL;

	return 0;
}

static void h_exit(struct hash_table *ht)
{
	vfree(ht->buckets);
}

static struct entry *h_head(struct hash_table *ht, unsigned bucket)
{
	return to_entry(ht->es, ht->buckets[bucket]);
}

static struct entry *h_next(struct hash_table *ht, struct entry *e)
{
	return to_entry(ht->es, e->hash_next);
}

static void __h_insert(struct hash_table *ht, unsigned bucket, struct entry *e)
{
	e->hash_next = ht->buckets[bucket];
	ht->buckets[bucket] = to_index(ht->es, e);
}

static void h_insert(struct hash_table *ht, struct entry *e)
{
	unsigned h = hash_64(from_oblock(e->oblock), ht->hash_bits);
	__h_insert(ht, h, e);
}

static struct entry *__h_lookup(struct hash_table *ht, unsigned h, dm_oblock_t oblock,
		struct entry **prev)
{
	struct entry *e;

	*prev = NULL;
	for (e = h_head(ht, h); e; e = h_next(ht, e)) {
		if (e->oblock == oblock)
			return e;

		*prev = e;
	}

	return NULL;
}

static void __h_unlink(struct hash_table *ht, unsigned h,
		struct entry *e, struct entry *prev)
{
	if (prev)
		prev->hash_next = e->hash_next;
	else
		ht->buckets[h] = e->hash_next;
}

/*
 * Also moves each entry to the front of the bucket.
 */
static struct entry *h_lookup(struct hash_table *ht, dm_oblock_t oblock)
{
	struct entry *e, *prev;
	unsigned h = hash_64(from_oblock(oblock), ht->hash_bits);

	e = __h_lookup(ht, h, oblock, &prev);
	if (e && prev) {
		/*
		 * Move to the front because this entry is likely
		 * to be hit again.
		 */
		__h_unlink(ht, h, e, prev);
		__h_insert(ht, h, e);
	}

	return e;
}

static void h_remove(struct hash_table *ht, struct entry *e)
{
	unsigned h = hash_64(from_oblock(e->oblock), ht->hash_bits);
	struct entry *prev;

	/*
	 * The down side of using a singly linked list is we have to
	 * iterate the bucket to remove an item.
	 */
	e = __h_lookup(ht, h, e->oblock, &prev);
	if (e)
		__h_unlink(ht, h, e, prev);
}

/*----------------------------------------------------------------*/

struct entry_alloc {
	struct entry_space *es;
	unsigned begin;

	unsigned nr_allocated;
	struct ilist free;
	/*ADDED BY PRAFULL...BEGIN ADD..*/
	struct ilist invalid;
	/*ADDED BY PRAFULL...END ADD..*/

};

static void init_allocator(struct entry_alloc *ea, struct entry_space *es,
		unsigned begin, unsigned end)
{
	unsigned i;

	ea->es = es;
	ea->nr_allocated = 0u;
	ea->begin = begin;
	/*ADDED BY PRAFULL...BEGIN ADD..*/
	l_init(&ea->invalid);
	/*ADDED BY PRAFULL...END ADD..*/

	l_init(&ea->free);
	for (i = begin; i != end; i++)
		l_add_tail(ea->es, &ea->free, __get_entry(ea->es, i));
}

static void init_entry(struct entry *e)
{
	/*
	 * We can't memset because that would clear the hotspot and
	 * sentinel bits which remain constant.
	 */
	e->hash_next = INDEXER_NULL;
	e->next = INDEXER_NULL;
	e->prev = INDEXER_NULL;
	e->level = 0u;
	e->allocated = true;
}

static struct entry *alloc_entry_resize(struct entry_alloc *ea){
	struct entry *e;

	if (l_empty(&ea->free))
		return NULL;

	e = l_pop_tail(ea->es, &ea->free);
	init_entry(e);
	return e;
}
static struct entry *alloc_entry(struct entry_alloc *ea)
{
	struct entry *e;

	if (l_empty(&ea->free))
		return NULL;

	e = l_pop_tail(ea->es, &ea->free);
	init_entry(e);
	ea->nr_allocated++;

	return e;
}

/*
 * This assumes the cblock hasn't already been allocated.
 */
static struct entry *alloc_particular_entry(struct entry_alloc *ea, unsigned i)
{
	struct entry *e = __get_entry(ea->es, ea->begin + i);

	BUG_ON(e->allocated);

	l_del(ea->es, &ea->free, e);
	init_entry(e);
	ea->nr_allocated++;

	return e;
}

static void free_entry(struct entry_alloc *ea, struct entry *e)
{
	BUG_ON(!ea->nr_allocated);
	BUG_ON(!e->allocated);

	ea->nr_allocated--;
	e->allocated = false;
	l_add_tail(ea->es, &ea->free, e);
}

static bool allocator_empty(struct entry_alloc *ea)
{

	return l_empty(&ea->free);
}

static unsigned get_index(struct entry_alloc *ea, struct entry *e)
{
	return to_index(ea->es, e) - ea->begin;
}

static struct entry *get_entry(struct entry_alloc *ea, unsigned index)
{
	return __get_entry(ea->es, ea->begin + index);
}

/*----------------------------------------------------------------*/

#define NR_HOTSPOT_LEVELS 64u
#define NR_CACHE_LEVELS 64u

#define WRITEBACK_PERIOD (10 * HZ)
#define DEMOTE_PERIOD (60 * HZ)

#define HOTSPOT_UPDATE_PERIOD (HZ)
#define CACHE_UPDATE_PERIOD (10u * HZ)

struct smq_policy {
	struct dm_cache_policy policy;

	/* protects everything */
	spinlock_t lock;
	dm_cblock_t cache_size;
	sector_t cache_block_size;

	sector_t hotspot_block_size;
	unsigned nr_hotspot_blocks;
	unsigned cache_blocks_per_hotspot_block;
	unsigned hotspot_level_jump;

	struct entry_space es;
	struct entry_alloc writeback_sentinel_alloc;
	struct entry_alloc demote_sentinel_alloc;
	struct entry_alloc hotspot_alloc;
	struct entry_alloc cache_alloc;

	unsigned long *hotspot_hit_bits;
	unsigned long *cache_hit_bits;

	/*
	 * We maintain three queues of entries.  The cache proper,
	 * consisting of a clean and dirty queue, containing the currently
	 * active mappings.  The hotspot queue uses a larger block size to
	 * track blocks that are being hit frequently and potential
	 * candidates for promotion to the cache.
	 */
	struct queue hotspot;
	struct queue clean;
	struct queue dirty;

	struct stats hotspot_stats;
	struct stats cache_stats;

	/*
	 * Keeps track of time, incremented by the core.  We use this to
	 * avoid attributing multiple hits within the same tick.
	 */
	unsigned tick;

	/*
	 * The hash tables allows us to quickly find an entry by origin
	 * block.
	 */
	struct hash_table table;
	struct hash_table hotspot_table;

	bool current_writeback_sentinels;
	unsigned long next_writeback_period;

	bool current_demote_sentinels;
	unsigned long next_demote_period;

	unsigned write_promote_level;
	unsigned read_promote_level;

	unsigned long next_hotspot_period;
	unsigned long next_cache_period;
};

/*----------------------------------------------------------------*/

static struct entry *get_sentinel(struct entry_alloc *ea, unsigned level, bool which)
{
	return get_entry(ea, which ? level : NR_CACHE_LEVELS + level);
}

static struct entry *writeback_sentinel(struct smq_policy *mq, unsigned level)
{
	return get_sentinel(&mq->writeback_sentinel_alloc, level, mq->current_writeback_sentinels);
}

static struct entry *demote_sentinel(struct smq_policy *mq, unsigned level)
{
	return get_sentinel(&mq->demote_sentinel_alloc, level, mq->current_demote_sentinels);
}

static void __update_writeback_sentinels(struct smq_policy *mq)
{
	unsigned level;
	struct queue *q = &mq->dirty;
	struct entry *sentinel;

	for (level = 0; level < q->nr_levels; level++) {
		sentinel = writeback_sentinel(mq, level);
		q_del(q, sentinel);
		q_push(q, sentinel);
	}
}

static void __update_demote_sentinels(struct smq_policy *mq)
{
	unsigned level;
	struct queue *q = &mq->clean;
	struct entry *sentinel;

	for (level = 0; level < q->nr_levels; level++) {
		sentinel = demote_sentinel(mq, level);
		q_del(q, sentinel);
		q_push(q, sentinel);
	}
}

static void update_sentinels(struct smq_policy *mq)
{
	if (time_after(jiffies, mq->next_writeback_period)) {
		__update_writeback_sentinels(mq);
		mq->next_writeback_period = jiffies + WRITEBACK_PERIOD;
		mq->current_writeback_sentinels = !mq->current_writeback_sentinels;
	}

	if (time_after(jiffies, mq->next_demote_period)) {
		__update_demote_sentinels(mq);
		mq->next_demote_period = jiffies + DEMOTE_PERIOD;
		mq->current_demote_sentinels = !mq->current_demote_sentinels;
	}
}

static void __sentinels_init(struct smq_policy *mq)
{
	unsigned level;
	struct entry *sentinel;

	for (level = 0; level < NR_CACHE_LEVELS; level++) {
		sentinel = writeback_sentinel(mq, level);
		sentinel->level = level;
		q_push(&mq->dirty, sentinel);

		sentinel = demote_sentinel(mq, level);
		sentinel->level = level;
		q_push(&mq->clean, sentinel);
	}
}

static void sentinels_init(struct smq_policy *mq)
{
	mq->next_writeback_period = jiffies + WRITEBACK_PERIOD;
	mq->next_demote_period = jiffies + DEMOTE_PERIOD;

	mq->current_writeback_sentinels = false;
	mq->current_demote_sentinels = false;
	__sentinels_init(mq);

	mq->current_writeback_sentinels = !mq->current_writeback_sentinels;
	mq->current_demote_sentinels = !mq->current_demote_sentinels;
	__sentinels_init(mq);
}

/*----------------------------------------------------------------*/

/*
 * These methods tie together the dirty queue, clean queue and hash table.
 */
static void push_new(struct smq_policy *mq, struct entry *e)
{
	struct queue *q = e->dirty ? &mq->dirty : &mq->clean;
	h_insert(&mq->table, e);
	q_push(q, e);
}

static void push(struct smq_policy *mq, struct entry *e)
{
	struct entry *sentinel;

	h_insert(&mq->table, e);

	/*
	 * Punch this into the queue just in front of the sentinel, to
	 * ensure it's cleaned straight away.
	 */
	if (e->dirty) {
		sentinel = writeback_sentinel(mq, e->level);
		q_push_before(&mq->dirty, sentinel, e);
	} else {
		sentinel = demote_sentinel(mq, e->level);
		q_push_before(&mq->clean, sentinel, e);
	}
}

/*
 * Removes an entry from cache.  Removes from the hash table.
 */
static void __del(struct smq_policy *mq, struct queue *q, struct entry *e)
{
	q_del(q, e);
	h_remove(&mq->table, e);
}

static void del(struct smq_policy *mq, struct entry *e)
{
	__del(mq, e->dirty ? &mq->dirty : &mq->clean, e);
}

static struct entry *pop_old(struct smq_policy *mq, struct queue *q, unsigned max_level)
{
	struct entry *e = q_pop_old(q, max_level);
	if (e)
		h_remove(&mq->table, e);
	return e;
}

static dm_cblock_t infer_cblock(struct smq_policy *mq, struct entry *e)
{
	return to_cblock(get_index(&mq->cache_alloc, e));
}

static void requeue(struct smq_policy *mq, struct entry *e)
{
	struct entry *sentinel;

	if (!test_and_set_bit(from_cblock(infer_cblock(mq, e)), mq->cache_hit_bits)) {
		if (e->dirty) {
			sentinel = writeback_sentinel(mq, e->level);
			q_requeue_before(&mq->dirty, sentinel, e, 1u);
		} else {
			sentinel = demote_sentinel(mq, e->level);
			q_requeue_before(&mq->clean, sentinel, e, 1u);
		}
	}
}

static unsigned default_promote_level(struct smq_policy *mq)
{
	/*
	 * The promote level depends on the current performance of the
	 * cache.
	 *
	 * If the cache is performing badly, then we can't afford
	 * to promote much without causing performance to drop below that
	 * of the origin device.
	 *
	 * If the cache is performing well, then we don't need to promote
	 * much.  If it isn't broken, don't fix it.
	 *
	 * If the cache is middling then we promote more.
	 *
	 * This scheme reminds me of a graph of entropy vs probability of a
	 * binary variable.
	 */
	static unsigned table[] = {1, 1, 1, 2, 4, 6, 7, 8, 7, 6, 4, 4, 3, 3, 2, 2, 1};

	unsigned hits = mq->cache_stats.hits;
	unsigned misses = mq->cache_stats.misses;
	unsigned index = safe_div(hits << 4u, hits + misses);
	return table[index];
}

static void update_promote_levels(struct smq_policy *mq)
{
	/*
	 * If there are unused cache entries then we want to be really
	 * eager to promote.
	 */
	unsigned threshold_level = allocator_empty(&mq->cache_alloc) ?
		default_promote_level(mq) : (NR_HOTSPOT_LEVELS / 2u);

	/*
	 * If the hotspot queue is performing badly then we have little
	 * confidence that we know which blocks to promote.  So we cut down
	 * the amount of promotions.
	 */
	switch (stats_assess(&mq->hotspot_stats)) {
		case Q_POOR:
			threshold_level /= 4u;
			break;

		case Q_FAIR:
			threshold_level /= 2u;
			break;

		case Q_WELL:
			break;
	}

	mq->read_promote_level = NR_HOTSPOT_LEVELS - threshold_level;
	mq->write_promote_level = (NR_HOTSPOT_LEVELS - threshold_level) + 2u;
}

/*
 * If the hotspot queue is performing badly, then we try and move entries
 * around more quickly.
 */
static void update_level_jump(struct smq_policy *mq)
{
	switch (stats_assess(&mq->hotspot_stats)) {
		case Q_POOR:
			mq->hotspot_level_jump = 4u;
			break;

		case Q_FAIR:
			mq->hotspot_level_jump = 2u;
			break;

		case Q_WELL:
			mq->hotspot_level_jump = 1u;
			break;
	}
}

static void end_hotspot_period(struct smq_policy *mq)
{
	clear_bitset(mq->hotspot_hit_bits, mq->nr_hotspot_blocks);
	update_promote_levels(mq);

	if (time_after(jiffies, mq->next_hotspot_period)) {
		update_level_jump(mq);
		q_redistribute(&mq->hotspot);
		stats_reset(&mq->hotspot_stats);
		mq->next_hotspot_period = jiffies + HOTSPOT_UPDATE_PERIOD;
	}
}

static void end_cache_period(struct smq_policy *mq)
{
	if (time_after(jiffies, mq->next_cache_period)) {
		clear_bitset(mq->cache_hit_bits, from_cblock(mq->cache_size));

		q_redistribute(&mq->dirty);
		q_redistribute(&mq->clean);
		stats_reset(&mq->cache_stats);

		mq->next_cache_period = jiffies + CACHE_UPDATE_PERIOD;
	}
}

static int demote_cblock(struct smq_policy *mq,
		struct policy_locker *locker,
		dm_oblock_t *oblock)
{
	struct entry *demoted;

	demoted = q_peek(&mq->clean, mq->clean.nr_levels, false);
	if (!demoted)
		/*
		 * We could get a block from mq->dirty, but that
		 * would add extra latency to the triggering bio as it
		 * waits for the writeback.  Better to not promote this
		 * time and hope there's a clean block next time this block
		 * is hit.
		 */
		return -ENOSPC;
	if (locker->fn(locker, demoted->oblock))
		/*
		 * We couldn't lock this block.
		 */
		return -EBUSY;
	del(mq, demoted);
	*oblock = demoted->oblock;
	free_entry(&mq->cache_alloc, demoted);

	return 0;
}

static int demote_cblock_symflex(struct smq_policy *mq, struct policy_locker *locker, dm_oblock_t *oblock, 
	struct app_group_t *current_group, struct app_group_t * app_groups)
{
	struct entry *demoted;

	demoted = q_peek_symflex(&mq->clean, mq->clean.nr_levels, false, current_group);
	if (!demoted)
		/*
		 * We could get a block from mq->dirty, but that
		 * would add extra latency to the triggering bio as it
		 * waits for the writeback.  Better to not promote this
		 * time and hope there's a clean block next time this block
		 * is hit.
		 */
		return -ENOSPC;

	if (locker->fn(locker, demoted->oblock))
		/*
		 * We couldn't lock this block.
		 */
		return -EBUSY;

	app_groups[demoted->group_id].allocated --;

	del(mq, demoted);
	*oblock = demoted->oblock;
	free_entry(&mq->cache_alloc, demoted);

	return 0;
}

enum promote_result {
	PROMOTE_NOT,
	PROMOTE_TEMPORARY,
	PROMOTE_PERMANENT
};

/*
 * Converts a boolean into a promote result.
 */
static enum promote_result maybe_promote(bool promote)
{
	return promote ? PROMOTE_PERMANENT : PROMOTE_NOT;
}

static enum promote_result should_promote(struct smq_policy *mq, struct entry *hs_e, struct bio *bio,
		bool fast_promote)
{
	if (bio_data_dir(bio) == WRITE) {
		if (!allocator_empty(&mq->cache_alloc) && fast_promote)
			return PROMOTE_TEMPORARY;

		else
			return maybe_promote(hs_e->level >= mq->write_promote_level);
	} 
	else{
		if(!allocator_empty(&mq->cache_alloc))
			return maybe_promote(true);
		return maybe_promote(hs_e->level >= mq->read_promote_level);
	}
}

static enum promote_result should_promote_symflex(struct smq_policy *mq, struct entry *hs_e, struct bio *bio,
		bool fast_promote, bool paritition_is_full)
{
	if (bio_data_dir(bio) == WRITE) 
	{
		if (!allocator_empty(&mq->cache_alloc) && fast_promote)
			return PROMOTE_TEMPORARY;

		else
			return maybe_promote(hs_e->level >= mq->write_promote_level);
	} 
	else
	{
		if(!allocator_empty(&mq->cache_alloc) && !paritition_is_full)
			return maybe_promote(true);

		return maybe_promote(hs_e->level >= mq->read_promote_level);
	}
}

static void insert_in_cache(struct smq_policy *mq, dm_oblock_t oblock,
		struct policy_locker *locker,
		struct policy_result *result, enum promote_result pr)
{
	int r;
	struct entry *e;

	if (allocator_empty(&mq->cache_alloc)) 
	{
		result->op = POLICY_REPLACE;
		r = demote_cblock(mq, locker, &result->old_oblock);
		if (r) 
		{
			result->op = POLICY_MISS;
			return;
		}

	} else
		result->op = POLICY_NEW;

	e = alloc_entry(&mq->cache_alloc);
	BUG_ON(!e);
	e->oblock = oblock;

	if (pr == PROMOTE_TEMPORARY)
		push(mq, e);
	else
		push_new(mq, e);

	result->cblock = infer_cblock(mq, e);
}
static void insert_in_cache_symflex(struct smq_policy *mq, dm_oblock_t oblock,
		struct policy_locker *locker,
		struct policy_result *result, enum promote_result pr, struct app_group_t *current_group, struct app_group_t * app_groups)
{
	int r;
	struct entry *e;

	if (allocator_empty(&mq->cache_alloc) || current_group->allocated >= current_group->size) 
	{
		result->op = POLICY_REPLACE;
		r = demote_cblock_symflex(mq, locker, &result->old_oblock, current_group, app_groups);
		if (r) 
		{
			result->op = POLICY_MISS;
			return;
		}
		//		current_group->allocated--;   decremented in demote_cblock_symflex

	} else
		result->op = POLICY_NEW;

	e = alloc_entry(&mq->cache_alloc);
	BUG_ON(!e);
	e->oblock = oblock;

	current_group->allocated++;
	e->group_id = current_group->id;

	if (pr == PROMOTE_TEMPORARY)
		push(mq, e);
	else
		push_new(mq, e);

	result->cblock = infer_cblock(mq, e);
}

static dm_oblock_t to_hblock(struct smq_policy *mq, dm_oblock_t b)
{
	sector_t r = from_oblock(b);
	(void) sector_div(r, mq->cache_blocks_per_hotspot_block);
	return to_oblock(r);
}

static struct entry *update_hotspot_queue(struct smq_policy *mq, dm_oblock_t b, struct bio *bio)
{
	unsigned hi;
	dm_oblock_t hb = to_hblock(mq, b);
	struct entry *e = h_lookup(&mq->hotspot_table, hb);

	if (e) {
		stats_level_accessed(&mq->hotspot_stats, e->level);

		hi = get_index(&mq->hotspot_alloc, e);
		q_requeue(&mq->hotspot, e,
				test_and_set_bit(hi, mq->hotspot_hit_bits) ?
				0u : mq->hotspot_level_jump);

	} else {
		stats_miss(&mq->hotspot_stats);

		e = alloc_entry(&mq->hotspot_alloc);
		if (!e) {
			e = q_pop(&mq->hotspot);
			if (e) {
				h_remove(&mq->hotspot_table, e);
				hi = get_index(&mq->hotspot_alloc, e);
				clear_bit(hi, mq->hotspot_hit_bits);
			}

		}

		if (e) {
			e->oblock = hb;
			q_push(&mq->hotspot, e);
			h_insert(&mq->hotspot_table, e);
		}
	}

	return e;
}
/*ADDED BY PRAFULL...BEGIN ADD..*/
//struct smq_policy *mq,struct policy_locker *locker,int flag
unsigned long relative_reclaim(int flag)
{

	printk("Hook works\n");

	return -1;

	// Add your logic here     
}



/*ADDED BY PRAFULL...END ADD..*/
/*
 * Looks the oblock up in the hash table, then decides whether to put in
 * pre_cache, or cache etc.
 */
static int map(struct smq_policy *mq, struct bio *bio, dm_oblock_t oblock,
		bool can_migrate, bool fast_promote,
		struct policy_locker *locker, struct policy_result *result, struct app_group_t * current_group, struct app_group_t * app_groups)
{

	struct entry *e, *hs_e;
	enum promote_result pr;
	mq1 = mq;

	/*ADDED BY PRAFULL...END ADD..*/

	hs_e = update_hotspot_queue(mq, oblock, bio);

	e = h_lookup(&mq->table, oblock);
	if (e) {

		stats_level_accessed(&mq->cache_stats, e->level);
		requeue(mq, e);
		result->op = POLICY_HIT;
		result->cblock = infer_cblock(mq, e);

	} 
	else 
	{
		stats_miss(&mq->cache_stats);

		pr = should_promote_symflex(mq, hs_e, bio, fast_promote, current_group->allocated >= current_group->size);
		if (pr == PROMOTE_NOT)
			result->op = POLICY_MISS;
		else 
		{
			if (!can_migrate) {
				result->op = POLICY_MISS;
				return -EWOULDBLOCK;
			}

			insert_in_cache_symflex(mq, oblock, locker, result, pr, current_group, app_groups);
		}
	}

	return 0;
}

/*----------------------------------------------------------------*/

/*
 * Public interface, via the policy struct.  See dm-cache-policy.h for a
 * description of these.
 */

static struct smq_policy *to_smq_policy(struct dm_cache_policy *p)
{
	return container_of(p, struct smq_policy, policy);
}

static void smq_destroy(struct dm_cache_policy *p)
{
	struct smq_policy *mq = to_smq_policy(p);

	h_exit(&mq->hotspot_table);
	h_exit(&mq->table);
	free_bitset(mq->hotspot_hit_bits);
	free_bitset(mq->cache_hit_bits);
	space_exit(&mq->es);
	kfree(mq);
}

/*SymFlex functions*/
int get_lcm(struct app_group_t *app_groups)
{
	int lcm = 1;
	int i;

	for(i=0; i<3; i++)
	{
		if(app_groups[i].ratio > 0)
			lcm *= app_groups[i].ratio;
	}

	return lcm;
}

void find_weighted_size(struct app_group_t *app_groups)
{
	int i;
	int lcm;
	int factor;
	struct app_group_t *app_group;

	lcm = get_lcm(app_groups);

	for(i=0; i<3; i++)
	{
		app_group = &app_groups[i];

		if(app_group->ratio > 0)
		{
			factor = lcm / app_group->ratio;
			app_group->weighted_size = factor * app_group->size;
		}
	}
}

struct app_group_t * get_group_with_least_size(struct app_group_t *app_groups)
{
	int i;
	struct app_group_t *min_weighted_size_group = NULL;
	struct app_group_t *app_group;

	for(i=0; i<3; i++)
	{
		app_group = &app_groups[i];

		if(app_group->ratio > 0)
		{
			if(!min_weighted_size_group || app_group->weighted_size < min_weighted_size_group->weighted_size)
				min_weighted_size_group = app_group;
		}
	}

	return min_weighted_size_group;
}

struct app_group_t * get_group_with_max_size(struct app_group_t *app_groups)
{
	int i;
	struct app_group_t *max_weighted_size_group = NULL;
	struct app_group_t *app_group;

	for(i=0; i<3; i++)
	{
		app_group = &app_groups[i];

		if(app_group->ratio > 0)
		{
			if(!max_weighted_size_group || app_group->weighted_size > max_weighted_size_group->weighted_size)
				max_weighted_size_group = app_group;
		}
	}

	return max_weighted_size_group;
}


void find_extra_blocks_to_evict(struct app_group_t *app_groups, struct app_group_t * min_weight_group)
{
	int i;
	struct app_group_t *app_group;
	int lcm;
	int factor;

	lcm = get_lcm(app_groups);

	for(i=0; i<3; i++)
	{
		app_group = &app_groups[i];

		if(app_group->ratio > 0)
		{
			factor = lcm / app_group->ratio;

			app_group->weighted_extra_size = app_group->weighted_size - min_weight_group->weighted_size;
			app_group->extra_size = app_group->weighted_extra_size / factor;
		}
	}
}

void find_extra_blocks_to_allocate(struct app_group_t *app_groups, struct app_group_t * max_weight_group)
{
	int i;
	struct app_group_t *app_group;
	int lcm;
	int factor;

	lcm = get_lcm(app_groups);

	for(i=0; i<3; i++)
	{
		app_group = &app_groups[i];

		if(app_group->ratio > 0)
		{
			factor = lcm / app_group->ratio;

			app_group->weighted_extra_size = max_weight_group->weighted_size - app_group->weighted_size;
			app_group->extra_size = app_group->weighted_extra_size / factor;
		}
	}
}

int inflate_extra_blocks(struct smq_policy *mq, struct policy_locker *locker, struct app_group_t *app_groups, int *buf, unsigned long int max_count, unsigned long int *blocks_evicted)
{
	int i, j;
	int ret;
	dm_cblock_t lbn;
	dm_oblock_t old_oblock;
	unsigned long int new_size;
	unsigned long int evicted_count = 0;
	struct entry *e;
	struct app_group_t *current_group;

	for(i=0; i<3 && evicted_count < max_count; i++)
	{
		current_group = &app_groups[i];

		if(current_group->ratio <= 0 || current_group->extra_size <= 0)
			continue;

		new_size = current_group->size - current_group->extra_size;

		for(j=0; j<current_group->extra_size && evicted_count < max_count; j++)
		{
			if(allocator_empty(&mq->cache_alloc) || current_group->allocated > new_size) 
			{
				ret = demote_cblock_symflex(mq, locker, &old_oblock, current_group, app_groups);
				if(ret)
				{
					buf[evicted_count] = -1;
					*blocks_evicted = evicted_count;

					return -1;
				}
				current_group->allocated --;
			}

			e = alloc_entry_resize(&mq->cache_alloc);//gets an entry from free list.
			l_add_tail(mq->cache_alloc.es, &mq->cache_alloc.invalid, e);//add that entry to invalid list

			lbn  = infer_cblock(mq, e);

			buf[evicted_count] = lbn;			
			evicted_count ++;
			current_group->size --;
		}
	}

	*blocks_evicted = evicted_count;

	return 0;
}

static int inflate_multiple_of_ratio_blocks(struct smq_policy *mq, struct policy_locker *locker, struct app_group_t *app_groups, int *buf, unsigned long int max_count, unsigned long int *blocks_evicted)
{
	int i, j;
	int ret;
	int round;
	int sum_of_ratio = 0;
	dm_cblock_t lbn;
	dm_oblock_t old_oblock;
	unsigned long int evicted_count = 0;
	struct entry *e;
	struct app_group_t *current_group;

	for(i=0; i<3; i++)
	{
		current_group = &app_groups[i];

		if(current_group->ratio <= 0)
			continue;

		sum_of_ratio += current_group->ratio;
	}
	round = max_count / sum_of_ratio;		

	for(i=0; i<3 && evicted_count < max_count; i++)
	{
		current_group = &app_groups[i];

		if(current_group->ratio <= 0)
			continue;

		for(j=0; j<round * current_group->ratio && evicted_count < max_count; j++)
		{
			if(allocator_empty(&mq->cache_alloc) || current_group->allocated >= current_group->size) 
			{
				ret = demote_cblock_symflex(mq, locker, &old_oblock, current_group, app_groups);
				if(ret)
				{
					buf[evicted_count] = -1;
					*blocks_evicted = evicted_count;

					return -1;
				}
			}

			e = alloc_entry_resize(&mq->cache_alloc);//gets an entry from free list.
			l_add_tail(mq->cache_alloc.es, &mq->cache_alloc.invalid, e);//add that entry to invalid list

			lbn  = infer_cblock(mq, e);

			buf[evicted_count] = lbn;			
			evicted_count ++;
			current_group->size --;
		}
	}

	*blocks_evicted = evicted_count;

	return 0;
}

static int inflate_remaining_blocks(struct smq_policy *mq, struct policy_locker *locker, struct app_group_t *app_groups, int *buf, unsigned long int max_count, unsigned long int *blocks_evicted)
{
	int i;
	int ret;
	dm_cblock_t lbn;
	dm_oblock_t old_oblock;
	unsigned long int evicted_count = 0;
	struct entry *e;
	struct app_group_t *current_group;

	while(evicted_count < max_count)
	{
		for(i=0; i<3 && evicted_count < max_count; i++)
		{
			current_group = &app_groups[i];

			if(current_group->ratio <= 0)
				continue;

			if(allocator_empty(&mq->cache_alloc) || current_group->allocated >= current_group->size) 
			{
				ret = demote_cblock_symflex(mq, locker, &old_oblock, current_group, app_groups);
				if(ret)
				{
					buf[evicted_count] = -1;
					*blocks_evicted = evicted_count;

					return -1;
				}
			}

			e = alloc_entry_resize(&mq->cache_alloc);//gets an entry from free list.
			l_add_tail(mq->cache_alloc.es, &mq->cache_alloc.invalid, e);//add that entry to invalid list

			lbn  = infer_cblock(mq, e);

			buf[evicted_count] = lbn;			
			evicted_count ++;
			current_group->size --;
		}
	}

	*blocks_evicted = evicted_count;

	return 0;
}

int deflate_extra_blocks(struct smq_policy *mq, struct app_group_t *app_groups, int *buf, 
		unsigned long int max_count, unsigned long int *blocks_allocated)
{
	int i, j;
	unsigned long int allocated = 0;
	struct entry *e;
	struct app_group_t *current_group;

	for(i=0; i<3 && allocated < max_count; i++)
	{
		current_group = &app_groups[i];

		if(current_group->ratio <= 0 || current_group->extra_size <= 0)
			continue;

		for(j=0; j<current_group->extra_size && allocated < max_count; j++)
		{
			if(l_empty(&mq->cache_alloc.invalid))
			{
				*blocks_allocated = allocated;
				return -1;
			}

			e = get_entry(&mq->cache_alloc, buf[allocated]);
			if(!e)
			{
				printk(KERN_ALERT "VSSD: cache block-no %d is invalid \n ", buf[allocated]);
				*blocks_allocated = allocated;
				return -1;
			}	

			l_del(mq->cache_alloc.es, &mq->cache_alloc.invalid, e);
			init_entry(e);
			l_add_tail(mq->cache_alloc.es, &mq->cache_alloc.free, e);

			allocated++;
			current_group->size ++;
		}
	}

	*blocks_allocated = allocated;
	return 0;
}

static int deflate_multiple_of_ratio_blocks(struct smq_policy *mq, struct app_group_t *app_groups, int *buf, 
		unsigned long int max_count, unsigned long int *blocks_allocated)
{
	int i, j;
	int round;
	int sum_of_ratio = 0;
	unsigned long int allocated = 0;
	struct entry *e;
	struct app_group_t *current_group;

	for(i=0; i<3; i++)
	{
		current_group = &app_groups[i];

		if(current_group->ratio <= 0)
			continue;

		sum_of_ratio += current_group->ratio;
	}
	round = max_count / sum_of_ratio;		

	for(i=0; i<3 && allocated < max_count; i++)
	{
		current_group = &app_groups[i];

		if(current_group->ratio <= 0)
			continue;

		for(j=0; j<round * current_group->ratio && allocated < max_count; j++)
		{

			if(l_empty(&mq->cache_alloc.invalid))
			{
				*blocks_allocated = allocated;
				return -1;
			}

			e = get_entry(&mq->cache_alloc, buf[allocated]);
			if(!e)
			{
				printk(KERN_ALERT "VSSD: cache block-no %d is invalid \n ", buf[allocated]);
				*blocks_allocated = allocated;
				return -1;
			}	

			l_del(mq->cache_alloc.es, &mq->cache_alloc.invalid, e);
			init_entry(e);
			l_add_tail(mq->cache_alloc.es, &mq->cache_alloc.free, e);

			allocated++;
			current_group->size ++;
		}
	}

	*blocks_allocated = allocated;
	return 0;
}


static int deflate_remaining_blocks(struct smq_policy *mq, struct app_group_t *app_groups, int *buf, unsigned long int max_count, 
	unsigned long int *blocks_allocated)
{
	int i;
	unsigned long int allocated = 0;
	struct entry *e;
	struct app_group_t *current_group;

	while(allocated < max_count)
	{
		for(i=0; i<3 && allocated < max_count; i++)
		{
			current_group = &app_groups[i];

			if(current_group->ratio <= 0)
				continue;

			if(l_empty(&mq->cache_alloc.invalid))
			{
				*blocks_allocated = allocated;
				return -1;
			}

			e = get_entry(&mq->cache_alloc, buf[allocated]);
			if(!e)
			{
				printk(KERN_ALERT "VSSD: cache block-no %d is invalid \n ", buf[allocated]);
				*blocks_allocated = allocated;
				return -1;
			}	

			l_del(mq->cache_alloc.es, &mq->cache_alloc.invalid, e);
			init_entry(e);
			l_add_tail(mq->cache_alloc.es, &mq->cache_alloc.free, e);

			allocated++;
			current_group->size ++;
		}
	}

	*blocks_allocated = allocated;
	return 0;
}


static unsigned long int do_inflation(struct smq_policy *mq, int count, int *buf, struct policy_locker *locker, struct app_group_t *app_groups)
{
	int ret;
	struct app_group_t *min_weight_group;
	unsigned long int evicted_extra_blocks;
	unsigned long int evicted_second_round;
	unsigned long int evicted_remaining;
	unsigned long int blocks_to_evict;

	blocks_to_evict = count;

	find_weighted_size(app_groups);
	min_weight_group = get_group_with_least_size(app_groups);
	find_extra_blocks_to_evict(app_groups, min_weight_group);

	ret = inflate_extra_blocks(mq, locker, app_groups, buf, blocks_to_evict, &evicted_extra_blocks);
	blocks_to_evict -= evicted_extra_blocks;
	if(ret < 0)						//	Migration
		return count - blocks_to_evict;

	if(blocks_to_evict == 0)		//	Evicted all blocks
		return count;		


	ret = inflate_multiple_of_ratio_blocks(mq, locker, app_groups, buf + (count - blocks_to_evict), blocks_to_evict, &evicted_second_round);
	blocks_to_evict -= evicted_second_round;
	if(ret < 0)						//	Migration
		return count - blocks_to_evict;

	if(blocks_to_evict == 0)		//	Evicted all blocks
		return count;		

	ret = inflate_remaining_blocks(mq, locker, app_groups, buf + (count - blocks_to_evict), blocks_to_evict, &evicted_remaining);
	blocks_to_evict -= evicted_remaining;
	if(ret < 0)						//	Migration
		return count - blocks_to_evict;

	if(blocks_to_evict == 0)		//	Evicted all blocks
		return count;		

	return count;
}


static unsigned long int do_deflation(struct smq_policy *mq, int count, int *buf, struct policy_locker *locker, struct app_group_t *app_groups)
{

	int ret;
	struct app_group_t *max_weight_group;
	unsigned long int available_blocks;
	unsigned long int allocated_blocks;

	available_blocks = count;

	find_weighted_size(app_groups);
	max_weight_group = get_group_with_max_size(app_groups);
	find_extra_blocks_to_allocate(app_groups, max_weight_group);

	ret = deflate_extra_blocks(mq, app_groups, buf, available_blocks, &allocated_blocks);
	available_blocks -= allocated_blocks;
	if(ret < 0)						//	Migration
		return count - available_blocks;

	if(available_blocks == 0)		//	
		return count;		


	ret = deflate_multiple_of_ratio_blocks(mq, app_groups, buf + (count - available_blocks), available_blocks, &allocated_blocks);
	available_blocks -= allocated_blocks;
	if(ret < 0)						//	Migration
		return count - available_blocks;

	if(available_blocks == 0)		//	
		return count;		


	ret = deflate_remaining_blocks(mq, app_groups, buf + (count - available_blocks), available_blocks, &allocated_blocks);
	available_blocks -= allocated_blocks;
	if(ret < 0)						//	Migration
		return count - available_blocks;

	if(available_blocks == 0)		//	
		return count;	
}

	static unsigned long 
smq_do_resize(struct dm_cache_policy *p, int count, int inflation, int *buf, struct policy_locker *locker, struct app_group_t *app_groups)
{
	struct smq_policy *mq;

	mq = to_smq_policy(p); 

	if(!mq)
	{
		printk(KERN_ALERT"Parameters not initialized yet...\n");
		return 0;
	}

	if(inflation)
		return do_inflation(mq, count, buf, locker, app_groups);
	else
		return do_deflation(mq, count, buf, locker, app_groups);


	return 0;
}

/*SymFlex functions*/


static int smq_map(struct dm_cache_policy *p, dm_oblock_t oblock,
		bool can_block, bool can_migrate, bool fast_promote,
		struct bio *bio, struct policy_locker *locker,
		struct policy_result *result, struct app_group_t *current_group, struct app_group_t * app_groups)
{
	int r;
	unsigned long flags;
	struct smq_policy *mq = to_smq_policy(p);

	result->op = POLICY_MISS;

	spin_lock_irqsave(&mq->lock, flags);
	r = map(mq, bio, oblock, can_migrate, fast_promote, locker, result, current_group, app_groups);
	spin_unlock_irqrestore(&mq->lock, flags);

	return r;
}

static int smq_lookup(struct dm_cache_policy *p, dm_oblock_t oblock, dm_cblock_t *cblock)
{
	int r;
	unsigned long flags;
	struct smq_policy *mq = to_smq_policy(p);
	struct entry *e;

	spin_lock_irqsave(&mq->lock, flags);
	e = h_lookup(&mq->table, oblock);
	if (e) {
		*cblock = infer_cblock(mq, e);
		r = 0;
	} else
		r = -ENOENT;
	spin_unlock_irqrestore(&mq->lock, flags);

	return r;
}

static void __smq_set_clear_dirty(struct smq_policy *mq, dm_oblock_t oblock, bool set)
{
	struct entry *e;

	e = h_lookup(&mq->table, oblock);
	BUG_ON(!e);

	del(mq, e);
	e->dirty = set;
	push(mq, e);
}

static void smq_set_dirty(struct dm_cache_policy *p, dm_oblock_t oblock)
{
	unsigned long flags;
	struct smq_policy *mq = to_smq_policy(p);

	spin_lock_irqsave(&mq->lock, flags);
	__smq_set_clear_dirty(mq, oblock, true);
	spin_unlock_irqrestore(&mq->lock, flags);
}

static void smq_clear_dirty(struct dm_cache_policy *p, dm_oblock_t oblock)
{
	struct smq_policy *mq = to_smq_policy(p);
	unsigned long flags;

	spin_lock_irqsave(&mq->lock, flags);
	__smq_set_clear_dirty(mq, oblock, false);
	spin_unlock_irqrestore(&mq->lock, flags);
}

static unsigned random_level(dm_cblock_t cblock)
{
	return hash_32_generic(from_cblock(cblock), 9) & (NR_CACHE_LEVELS - 1);
}

static int smq_load_mapping(struct dm_cache_policy *p,
		dm_oblock_t oblock, dm_cblock_t cblock,
		uint32_t hint, bool hint_valid)
{
	struct smq_policy *mq = to_smq_policy(p);
	struct entry *e;

	e = alloc_particular_entry(&mq->cache_alloc, from_cblock(cblock));
	e->oblock = oblock;
	e->dirty = false;	/* this gets corrected in a minute */
	e->level = hint_valid ? min(hint, NR_CACHE_LEVELS - 1) : random_level(cblock);
	push(mq, e);

	return 0;
}

static uint32_t smq_get_hint(struct dm_cache_policy *p, dm_cblock_t cblock)
{
	struct smq_policy *mq = to_smq_policy(p);
	struct entry *e = get_entry(&mq->cache_alloc, from_cblock(cblock));

	if (!e->allocated)
		return 0;

	return e->level;
}

static void __remove_mapping(struct smq_policy *mq, dm_oblock_t oblock)
{
	struct entry *e;

	e = h_lookup(&mq->table, oblock);
	BUG_ON(!e);

	del(mq, e);
	free_entry(&mq->cache_alloc, e);
}

static void smq_remove_mapping(struct dm_cache_policy *p, dm_oblock_t oblock)
{
	struct smq_policy *mq = to_smq_policy(p);
	unsigned long flags;

	spin_lock_irqsave(&mq->lock, flags);
	__remove_mapping(mq, oblock);
	spin_unlock_irqrestore(&mq->lock, flags);
}

static int __remove_cblock(struct smq_policy *mq, dm_cblock_t cblock)
{
	struct entry *e = get_entry(&mq->cache_alloc, from_cblock(cblock));

	if (!e || !e->allocated)
		return -ENODATA;

	del(mq, e);
	free_entry(&mq->cache_alloc, e);

	return 0;
}

static int smq_remove_cblock(struct dm_cache_policy *p, dm_cblock_t cblock)
{
	int r;
	unsigned long flags;
	struct smq_policy *mq = to_smq_policy(p);

	spin_lock_irqsave(&mq->lock, flags);
	r = __remove_cblock(mq, cblock);
	spin_unlock_irqrestore(&mq->lock, flags);

	return r;
}


#define CLEAN_TARGET_CRITICAL 5u /* percent */

static bool clean_target_met(struct smq_policy *mq, bool critical)
{
	if (critical) {
		/*
		 * Cache entries may not be populated.  So we're cannot rely on the
		 * size of the clean queue.
		 */
		unsigned nr_clean = from_cblock(mq->cache_size) - q_size(&mq->dirty);
		unsigned target = from_cblock(mq->cache_size) * CLEAN_TARGET_CRITICAL / 100u;

		return nr_clean >= target;
	} else
		return !q_size(&mq->dirty);
}

static int __smq_writeback_work(struct smq_policy *mq, dm_oblock_t *oblock,
		dm_cblock_t *cblock, bool critical_only)
{
	struct entry *e = NULL;
	bool target_met = clean_target_met(mq, critical_only);

	if (critical_only)
		/*
		 * Always try and keep the bottom level clean.
		 */
		e = pop_old(mq, &mq->dirty, target_met ? 1u : mq->dirty.nr_levels);

	else
		e = pop_old(mq, &mq->dirty, mq->dirty.nr_levels);

	if (!e)
		return -ENODATA;

	*oblock = e->oblock;
	*cblock = infer_cblock(mq, e);
	e->dirty = false;
	push_new(mq, e);

	return 0;
}

static int smq_writeback_work(struct dm_cache_policy *p, dm_oblock_t *oblock,
		dm_cblock_t *cblock, bool critical_only)
{
	int r;
	unsigned long flags;
	struct smq_policy *mq = to_smq_policy(p);

	spin_lock_irqsave(&mq->lock, flags);
	r = __smq_writeback_work(mq, oblock, cblock, critical_only);
	spin_unlock_irqrestore(&mq->lock, flags);

	return r;
}

static void __force_mapping(struct smq_policy *mq,
		dm_oblock_t current_oblock, dm_oblock_t new_oblock)
{
	struct entry *e = h_lookup(&mq->table, current_oblock);

	if (e) {
		del(mq, e);
		e->oblock = new_oblock;
		e->dirty = true;
		push(mq, e);
	}
}

static void smq_force_mapping(struct dm_cache_policy *p,
		dm_oblock_t current_oblock, dm_oblock_t new_oblock)
{
	unsigned long flags;
	struct smq_policy *mq = to_smq_policy(p);

	spin_lock_irqsave(&mq->lock, flags);
	__force_mapping(mq, current_oblock, new_oblock);
	spin_unlock_irqrestore(&mq->lock, flags);
}

static dm_cblock_t smq_residency(struct dm_cache_policy *p)
{
	dm_cblock_t r;
	unsigned long flags;
	struct smq_policy *mq = to_smq_policy(p);

	spin_lock_irqsave(&mq->lock, flags);
	r = to_cblock(mq->cache_alloc.nr_allocated);
	spin_unlock_irqrestore(&mq->lock, flags);

	return r;
}

static dm_cblock_t smq_invalid_blocks(struct dm_cache_policy *p)
{
	// dm_cblock_t r;
	// unsigned long flags;
	// struct smq_policy *mq = to_smq_policy(p);

	// spin_lock_irqsave(&mq->lock, flags);
	// r = to_cblock(mq->cache_alloc.nr_allocated);
	// spin_unlock_irqrestore(&mq->lock, flags);

	return 1234;
}

static void smq_tick(struct dm_cache_policy *p, bool can_block)
{
	struct smq_policy *mq = to_smq_policy(p);
	unsigned long flags;
	//printk(KERN_ALERT"Cache statistics reset...\n");
	spin_lock_irqsave(&mq->lock, flags);
	mq->tick++;
	update_sentinels(mq);
	end_hotspot_period(mq);
	end_cache_period(mq);
	spin_unlock_irqrestore(&mq->lock, flags);
}

/*
 * smq has no config values, but the old mq policy did.  To avoid breaking
 * software we continue to accept these configurables for the mq policy,
 * but they have no effect.
 */
static int mq_set_config_value(struct dm_cache_policy *p,
		const char *key, const char *value)
{
	unsigned long tmp;

	if (kstrtoul(value, 10, &tmp))
		return -EINVAL;

	if (!strcasecmp(key, "random_threshold") ||
			!strcasecmp(key, "sequential_threshold") ||
			!strcasecmp(key, "discard_promote_adjustment") ||
			!strcasecmp(key, "read_promote_adjustment") ||
			!strcasecmp(key, "write_promote_adjustment")) {
		DMWARN("tunable '%s' no longer has any effect, mq policy is now an alias for smq", key);
		return 0;
	}

	return -EINVAL;
}

static int mq_emit_config_values(struct dm_cache_policy *p, char *result,
		unsigned maxlen, ssize_t *sz_ptr)
{
	ssize_t sz = *sz_ptr;

	DMEMIT("10 random_threshold 0 "
			"sequential_threshold 0 "
			"discard_promote_adjustment 0 "
			"read_promote_adjustment 0 "
			"write_promote_adjustment 0 ");

	*sz_ptr = sz;
	return 0;
}

/* Init the policy plugin interface function pointers. */
static void init_policy_functions(struct smq_policy *mq, bool mimic_mq)
{
	mq->policy.destroy = smq_destroy;
	mq->policy.map = smq_map;
	mq->policy.lookup = smq_lookup;
	mq->policy.set_dirty = smq_set_dirty;
	mq->policy.clear_dirty = smq_clear_dirty;
	mq->policy.load_mapping = smq_load_mapping;
	mq->policy.get_hint = smq_get_hint;
	mq->policy.remove_mapping = smq_remove_mapping;
	mq->policy.remove_cblock = smq_remove_cblock;
	mq->policy.writeback_work = smq_writeback_work;
	mq->policy.force_mapping = smq_force_mapping;
	mq->policy.residency = smq_residency;

	mq->policy.invalid_blocks = smq_invalid_blocks;

	mq->policy.tick = smq_tick;

	mq->policy.do_resize = smq_do_resize;

	if (mimic_mq) {
		mq->policy.set_config_value = mq_set_config_value;
		mq->policy.emit_config_values = mq_emit_config_values;
	}
}

static bool too_many_hotspot_blocks(sector_t origin_size,
		sector_t hotspot_block_size,
		unsigned nr_hotspot_blocks)
{
	return (hotspot_block_size * nr_hotspot_blocks) > origin_size;
}

static void calc_hotspot_params(sector_t origin_size,
		sector_t cache_block_size,
		unsigned nr_cache_blocks,
		sector_t *hotspot_block_size,
		unsigned *nr_hotspot_blocks)
{
	*hotspot_block_size = cache_block_size * 16u;
	*nr_hotspot_blocks = max(nr_cache_blocks / 4u, 1024u);

	while ((*hotspot_block_size > cache_block_size) &&
			too_many_hotspot_blocks(origin_size, *hotspot_block_size, *nr_hotspot_blocks))
		*hotspot_block_size /= 2u;
}
// static unsigned long get_statistics(int *buf){


// 	static unsigned long total_accesses; 
// 	static unsigned long total_hits;

// 	//	printk(KERN_ALERT"Inside get statistics\n");
// 	while(cache_global==NULL){
// 		ssleep(10);
// 	}
// 	if(cache_global != NULL){
// 		printk(KERN_ALERT"Read hits = %d\n",atomic_read(&cache_global->stats.read_hit));
// 		printk(KERN_ALERT"Write hits = %d\n",atomic_read(&cache_global->stats.write_hit));
// 		printk(KERN_ALERT"Read Misses = %d\n",atomic_read(&cache_global->stats.read_miss));
// 		printk(KERN_ALERT"Write Misses = %d\n",atomic_read(&cache_global->stats.write_miss));
// 		printk(KERN_ALERT"Promotions = %d\n",atomic_read(&cache_global->stats.promotion));
// 		printk(KERN_ALERT"Demotions = %d\n",atomic_read(&cache_global->stats.demotion));
// 		total_accesses = atomic_read(&cache_global->stats.read_hit)+atomic_read(&cache_global->stats.write_hit)+atomic_read(&cache_global->stats.read_miss)+atomic_read(&cache_global->stats.write_miss)+atomic_read(&cache_global->stats.promotion)+atomic_read(&cache_global->stats.demotion);
// 		//	printk(KERN_ALERT"total_accesses = %d",total_accesses);
// 		total_hits = atomic_read(&cache_global->stats.read_hit)+atomic_read(&cache_global->stats.write_hit);
// 		//	printk(KERN_ALERT"Hit ratio is %f\n",hit_ratio);
// 		buf[0]=total_hits;
// 		buf[1]=total_accesses;
// 	}
// 	return 8956;

// }
/*static int dealloc_blocks(struct smq_policy *mq,int *buf,int count){

  int i;
  struct entry *e;
  struct policy_result res;
  dm_cblock_t out;
  int count1=0;
  printk(KERN_ALERT"Inside dealloc blocks function..\n");
  for(i=0;i<count;i++){
//	printk(KERN_ALERT"round %d\n",i);

if(l_empty(&mq->cache_alloc.free)){
printk(KERN_ALERT"No free entries available\n");
return -1;
}
//		printk(KERN_ALERT"Free entries available\n");					
e = l_head(&mq->es,&mq->cache_alloc.free);	
//		 printk(KERN_ALERT"Got the head\n");

//		printk(KERN_ALERT"Searching for %d\n",buf[i]);
for(;e;e=l_next(&mq->es,e)){
out = infer_cblock(mq,e);
//		printk(KERN_ALERT"Free list:cache block present is %u\n",out);

if(buf[i]==out){
//	printk(KERN_ALERT"dmcache:block no %d found\n",out);
e = l_pop_tail(mq->cache_alloc.es,&mq->cache_alloc.free);
init_entry(e);
l_add_tail(mq->cache_alloc.es,&mq->cache_alloc.invalid,e);
count1++;	
break;
}	

}
}	
printk(KERN_ALERT"No of blocks not found =%d",count-count1);	
}*/

static struct dm_cache_policy *__smq_create(dm_cblock_t cache_size,
		sector_t origin_size,
		sector_t cache_block_size,
		bool mimic_mq)
{
	unsigned i;
	unsigned nr_sentinels_per_queue = 2u * NR_CACHE_LEVELS;
	unsigned total_sentinels = 2u * nr_sentinels_per_queue;
	struct smq_policy *mq = kzalloc(sizeof(*mq), GFP_KERNEL);
	struct bitmap_t *bitmap;
	// int count=0;

	//	struct bitmap_t *bitmap;
	unsigned long long int j;
	struct entry *entry;
	/*	struct old_oblock_lock ool;
		ool.locker.fn = cell_locker;
		unsigned long test;
	 */
	int *buf;
	// unsigned long excess;

	buf = kzalloc(20480*sizeof(int),GFP_KERNEL);


	if (!mq)
		return NULL;

	// get_stats = &get_statistics;

	init_policy_functions(mq, mimic_mq);
	printk(KERN_ALERT"Called init policy functions..\n");
	mq->cache_size = cache_size;
	mq->cache_block_size = cache_block_size;

	calc_hotspot_params(origin_size, cache_block_size, from_cblock(cache_size),
			&mq->hotspot_block_size, &mq->nr_hotspot_blocks);

	mq->cache_blocks_per_hotspot_block = div64_u64(mq->hotspot_block_size, mq->cache_block_size);
	mq->hotspot_level_jump = 1u;
	if (space_init(&mq->es, total_sentinels + mq->nr_hotspot_blocks + from_cblock(cache_size))) {
		DMERR("couldn't initialize entry space");
		goto bad_pool_init;
	}

	init_allocator(&mq->writeback_sentinel_alloc, &mq->es, 0, nr_sentinels_per_queue);
	for (i = 0; i < nr_sentinels_per_queue; i++)
		get_entry(&mq->writeback_sentinel_alloc, i)->sentinel = true;

	init_allocator(&mq->demote_sentinel_alloc, &mq->es, nr_sentinels_per_queue, total_sentinels);
	for (i = 0; i < nr_sentinels_per_queue; i++)
		get_entry(&mq->demote_sentinel_alloc, i)->sentinel = true;

	init_allocator(&mq->hotspot_alloc, &mq->es, total_sentinels,
			total_sentinels + mq->nr_hotspot_blocks);

	init_allocator(&mq->cache_alloc, &mq->es,
			total_sentinels + mq->nr_hotspot_blocks,
			total_sentinels + mq->nr_hotspot_blocks + from_cblock(cache_size));

	mq->hotspot_hit_bits = alloc_bitset(mq->nr_hotspot_blocks);
	if (!mq->hotspot_hit_bits) {
		DMERR("couldn't allocate hotspot hit bitset");
		goto bad_hotspot_hit_bits;
	}
	clear_bitset(mq->hotspot_hit_bits, mq->nr_hotspot_blocks);

	if (from_cblock(cache_size)) {
		mq->cache_hit_bits = alloc_bitset(from_cblock(cache_size));
		if (!mq->cache_hit_bits) {
			DMERR("couldn't allocate cache hit bitset");
			goto bad_cache_hit_bits;
		}
		clear_bitset(mq->cache_hit_bits, from_cblock(mq->cache_size));
	} else
		mq->cache_hit_bits = NULL;

	mq->tick = 0;
	spin_lock_init(&mq->lock);

	q_init(&mq->hotspot, &mq->es, NR_HOTSPOT_LEVELS);
	mq->hotspot.nr_top_levels = 8;
	mq->hotspot.nr_in_top_levels = min(mq->nr_hotspot_blocks / NR_HOTSPOT_LEVELS,
			from_cblock(mq->cache_size) / mq->cache_blocks_per_hotspot_block);

	q_init(&mq->clean, &mq->es, NR_CACHE_LEVELS);
	q_init(&mq->dirty, &mq->es, NR_CACHE_LEVELS);

	stats_init(&mq->hotspot_stats, NR_HOTSPOT_LEVELS);
	stats_init(&mq->cache_stats, NR_CACHE_LEVELS);

	if (h_init(&mq->table, &mq->es, from_cblock(cache_size)))
		goto bad_alloc_table;

	if (h_init(&mq->hotspot_table, &mq->es, mq->nr_hotspot_blocks))
		goto bad_alloc_hotspot_table;

	sentinels_init(mq);
	mq->write_promote_level = mq->read_promote_level = NR_HOTSPOT_LEVELS;

	mq->next_hotspot_period = jiffies;
	mq->next_cache_period = jiffies;


	/*ADDED BY PRAFULL...END ADD..*/
	bitmap = register_dm_cache(150);
	// Bitmap_print(bitmap);

	for(j=0;  j < bitmap->max_blocks; j++)
	{
		if(Bitmap_is_set(bitmap, j))
			continue;

		entry = get_entry(&mq->cache_alloc, j);
		if(!entry)
		{
			printk(KERN_ALERT "VSSD: cache block-no %llu is invalid \n ", j);
			return NULL;
		}	

		l_del(mq->cache_alloc.es, &mq->cache_alloc.free, entry);
		// init_entry(e);
		l_add_tail(mq->cache_alloc.es, &mq->cache_alloc.invalid, entry);

	}
	printk(KERN_ALERT"dmcache:register vssd called\n");

	printk(KERN_ALERT"dmcache:registered with vssd driver..\n");	

	return &mq->policy;

bad_alloc_hotspot_table:
	h_exit(&mq->table);
bad_alloc_table:
	free_bitset(mq->cache_hit_bits);
bad_cache_hit_bits:
	free_bitset(mq->hotspot_hit_bits);
bad_hotspot_hit_bits:
	space_exit(&mq->es);
bad_pool_init:
	kfree(mq);

	return NULL;
}

static struct dm_cache_policy *smq_create(dm_cblock_t cache_size,
		sector_t origin_size,
		sector_t cache_block_size)
{
	return __smq_create(cache_size, origin_size, cache_block_size, false);
}

static struct dm_cache_policy *mq_create(dm_cblock_t cache_size,
		sector_t origin_size,
		sector_t cache_block_size)
{
	return __smq_create(cache_size, origin_size, cache_block_size, true);
}

/*----------------------------------------------------------------*/

static struct dm_cache_policy_type smq_policy_type = {
	.name = "smq",
	.version = {1, 5, 0},
	.hint_size = 4,
	.owner = THIS_MODULE,
	.create = smq_create
};

static struct dm_cache_policy_type mq_policy_type = {
	.name = "mq",
	.version = {1, 5, 0},
	.hint_size = 4,
	.owner = THIS_MODULE,
	.create = mq_create,
};

static struct dm_cache_policy_type default_policy_type = {
	.name = "default",
	.version = {1, 5, 0},
	.hint_size = 4,
	.owner = THIS_MODULE,
	.create = smq_create,
	.real = &smq_policy_type
};

static int __init smq_init(void)
{
	int r;

	r = dm_cache_policy_register(&smq_policy_type);
	if (r) {
		DMERR("register failed %d", r);
		return -ENOMEM;
	}

	r = dm_cache_policy_register(&mq_policy_type);
	if (r) {
		DMERR("register failed (as mq) %d", r);
		dm_cache_policy_unregister(&smq_policy_type);
		return -ENOMEM;
	}

	r = dm_cache_policy_register(&default_policy_type);
	if (r) {
		DMERR("register failed (as default) %d", r);
		dm_cache_policy_unregister(&mq_policy_type);
		dm_cache_policy_unregister(&smq_policy_type);
		return -ENOMEM;
	}


	return 0;
}

static void __exit smq_exit(void)
{
	dm_cache_policy_unregister(&smq_policy_type);
	dm_cache_policy_unregister(&mq_policy_type);
	dm_cache_policy_unregister(&default_policy_type);
}

module_init(smq_init);
module_exit(smq_exit);

MODULE_AUTHOR("Joe Thornber <dm-devel@redhat.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("smq cache policy");

MODULE_ALIAS("dm-cache-default");
MODULE_ALIAS("dm-cache-mq");
