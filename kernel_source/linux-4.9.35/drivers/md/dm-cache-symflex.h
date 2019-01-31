struct cache_stats {
	atomic_t read_hit;
	atomic_t read_miss;
	atomic_t write_hit;
	atomic_t write_miss;
	atomic_t demotion;
	atomic_t promotion;
	atomic_t copies_avoided;
	atomic_t cache_cell_clash;
	atomic_t commit_count;
	atomic_t discard_count;

	/*	unaisp	*/
	atomic_t request_count;
};

/*
 * Defines a range of cblocks, begin to (end - 1) are in the range.  end is
 * the one-past-the-end value.
 */
struct cblock_range {
	dm_cblock_t begin;
	dm_cblock_t end;
};

struct invalidation_request {
	struct list_head list;
	struct cblock_range *cblocks;

	atomic_t complete;
	int err;

	wait_queue_head_t result_wait;
};

struct app_group_t
{
	int id;
	unsigned long int size;
	unsigned long int allocated;
	int ratio;

	struct cache_stats stats;

	//
	unsigned long int weighted_size;
	unsigned long int weighted_extra_size;
	unsigned long int extra_size;
};