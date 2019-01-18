struct bitmap_t
{
	char name[24];
	unsigned long int max_blocks;
	unsigned long int size_in_ints;
	unsigned long int size_in_bytes;
	unsigned long int nset;

	__u32 list[1];		//	
};

#define ASSERT(expr) \
        if(!(expr)) { \
                printk( "\n" __FILE__ ":%d: Assertion " #expr " failed!\n",__LINE__); \
                panic(#expr); \
        }

#define sizeofBitmap(bitmap) 	(sizeof(struct bitmap_t) + bitmap->size_in_bytes)

void int_to_binary(unsigned int ptr, char *buf)
{

	int i;

	for (i = 0; i < 32; i++) 
	{
		if( (ptr>>(32-i-1)) & 1) 
			buf[i] = '1';
		else
			buf[i] = '0';
	}
	buf[i] = '\0';
}

static void 
Bitmap_print(struct bitmap_t *bitmap)
{
	uint64_t block_num;
	char buf[50];

	printk(KERN_INFO"VSSD: Bitmap: [%s]  \n", bitmap->name);

	for(block_num = 0; block_num<bitmap->max_blocks ;block_num += 32)
	{
		int_to_binary(bitmap->list[block_num/32], buf);
		printk(KERN_INFO "VSSD: %5lld to %5lld. [%s] \n", block_num, block_num+31, buf);
	}
}

static bool 
Bitmap_is_set(struct bitmap_t *bitmap, unsigned long int block_num)
{
	ASSERT(bitmap != NULL);
	ASSERT(block_num < bitmap->max_blocks);

	if((bitmap->list[block_num/32] & (1 << (block_num % 32))) == (1 << (block_num % 32))) 
		return true;

	return false;
}

static void
Bitmap_set(struct bitmap_t *bitmap, unsigned long int block_num)
{
	ASSERT(bitmap != NULL);
	ASSERT(block_num < bitmap->max_blocks);
	ASSERT(!Bitmap_is_set(bitmap, block_num));

	bitmap->list[block_num/32] |= (1 << (block_num % 32));
	bitmap->nset++;

	ASSERT(bitmap->nset > 0 && bitmap->nset <= bitmap->max_blocks);
}

static void
Bitmap_unset(struct bitmap_t *bitmap, unsigned long int block_num)
{
	ASSERT(bitmap != NULL);
	ASSERT(block_num < bitmap->max_blocks);
	ASSERT(Bitmap_is_set(bitmap, block_num));

	bitmap->list[block_num/32] &= ~(1 << (block_num % 32));
	bitmap->nset--;

	ASSERT(bitmap->nset >= 0 && bitmap->nset < bitmap->max_blocks);
}

static struct bitmap_t *
Bitmap_create_and_initialize(unsigned long int max_blocks, char *name)
{
	struct bitmap_t *bitmap;
	unsigned long int size_in_ints;

	ASSERT(max_blocks > 0);

	size_in_ints = (max_blocks / 32) + ((max_blocks % 32) == 0 ? 0: 1);
	bitmap = (struct bitmap_t *)kmalloc(sizeof(struct bitmap_t) + size_in_ints * sizeof(__u32), GFP_KERNEL);
	
	bitmap->max_blocks = max_blocks;
	bitmap->size_in_ints = size_in_ints;
	bitmap->size_in_bytes = bitmap->size_in_ints * sizeof(__u32);
	bitmap->nset = 0;
	sprintf(bitmap->name, "%s", name);

	memset(bitmap->list, 0, bitmap->size_in_bytes);

	return bitmap;	
}

static void 
Bitmap_initialize(struct bitmap_t *bitmap, unsigned long int max_blocks)
{
	bitmap->max_blocks = max_blocks;
	bitmap->size_in_ints = (max_blocks / 32) + ((max_blocks % 32) == 0 ? 0: 1);
	bitmap->size_in_bytes = bitmap->size_in_ints * sizeof(unsigned int);
	bitmap->nset = 0;

	memset(bitmap->list, 0, bitmap->size_in_bytes);
}

static struct bitmap_t *
Bitmap_duplicate(struct bitmap_t *bitmap, char *name)
{
	struct bitmap_t *dup_bitmap;

	ASSERT(bitmap);
	dup_bitmap = (struct bitmap_t *)kmalloc(sizeof(struct bitmap_t) + bitmap->size_in_bytes, GFP_KERNEL);
	memcpy(dup_bitmap, bitmap, sizeof(struct bitmap_t));
	sprintf(dup_bitmap->name, "%s", name);

	memcpy(dup_bitmap->list, bitmap->list, dup_bitmap->size_in_bytes);

	return dup_bitmap;
}