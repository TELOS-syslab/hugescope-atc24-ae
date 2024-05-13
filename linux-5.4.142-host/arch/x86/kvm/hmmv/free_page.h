//保存free的页面缓存池

struct free_pages {
	struct page *objs_dram_1[FREE_PAGE_SIZE];
	struct page *objs_dram_2[FREE_PAGE_SIZE];
	struct page *objs_nvm_1[FREE_PAGE_SIZE];
	struct page *objs_nvm_2[FREE_PAGE_SIZE];
	int dram_size_1;
	int dram_size_2;
	int nvm_size_1;
	int nvm_size_2;
};
struct free_pages *pm_free_pages = NULL;

static int add_pm_free_pages_in_dram(void *data)
{
	while (pm_free_pages->dram_size_1 < FREE_PAGE_SIZE)
		pm_free_pages->objs_dram_1[pm_free_pages->dram_size_1++] =
			__alloc_pages_node(
				0, GFP_HIGHUSER_MOVABLE | __GFP_THISNODE, 0);
	while (pm_free_pages->dram_size_2 < FREE_PAGE_SIZE)
		pm_free_pages->objs_dram_2[pm_free_pages->dram_size_2++] =
			__alloc_pages_node(
				0, GFP_HIGHUSER_MOVABLE | __GFP_THISNODE, 0);

	printk("finish free pages in dram\n");
	return 0;
}

static int add_pm_free_pages_in_nvm(void *data)
{
	while (pm_free_pages->nvm_size_1 < FREE_PAGE_SIZE)
		pm_free_pages->objs_nvm_1[pm_free_pages->nvm_size_1++] =
			__alloc_pages_node(
				2, GFP_HIGHUSER_MOVABLE | __GFP_THISNODE, 0);
	while (pm_free_pages->nvm_size_2 < FREE_PAGE_SIZE)
		pm_free_pages->objs_nvm_2[pm_free_pages->nvm_size_2++] =
			__alloc_pages_node(
				2, GFP_HIGHUSER_MOVABLE | __GFP_THISNODE, 0);
	printk("finish free pages in nvm\n");
	return 0;
}

int add_pm_free_pages(void)
{
	if (!pm_free_pages) {
		pm_free_pages =
			(struct free_pages *)vmalloc(sizeof(struct free_pages));
		pm_free_pages->dram_size_1 = 0;
		pm_free_pages->dram_size_2 = 0;
		pm_free_pages->nvm_size_1 = 0;
		pm_free_pages->nvm_size_2 = 0;
	}

	kthread_run(add_pm_free_pages_in_dram, (void *)NULL,
		    "add_pm_free_pages_in_dram");
	kthread_run(add_pm_free_pages_in_nvm, (void *)NULL,
		    "add_pm_free_pages_in_nvm");
	return 0;
}

void del_pm_free_pages(void)
{
	if (!pm_free_pages)
		return;
	else {
		while (pm_free_pages->dram_size_1 > 0) {
			__free_pages(pm_free_pages->objs_dram_1
					     [pm_free_pages->dram_size_1 - 1],
				     0);
			pm_free_pages->dram_size_1--;
		}

		while (pm_free_pages->nvm_size_1 > 0) {
			__free_pages(pm_free_pages->objs_nvm_1
					     [pm_free_pages->nvm_size_1 - 1],
				     0);
			pm_free_pages->nvm_size_1--;
		}
		while (pm_free_pages->dram_size_2 > 0) {
			__free_pages(pm_free_pages->objs_dram_2
					     [pm_free_pages->dram_size_2 - 1],
				     0);
			pm_free_pages->dram_size_2--;
		}

		while (pm_free_pages->nvm_size_2 > 0) {
			__free_pages(pm_free_pages->objs_nvm_2
					     [pm_free_pages->nvm_size_2 - 1],
				     0);
			pm_free_pages->nvm_size_2--;
		}
	}
	vfree(pm_free_pages);
	pm_free_pages = NULL;
	return;
}

struct page *get_free_page_from_target_cache(int thread)
{
	struct page *p = NULL;

	//	printk("dram page size is %d\n",pm_free_pages->dram_size);
	//	printk("nvm page size is %d\n",pm_free_pages->nvm_size);

	switch (thread) {
	case 0:
		if (pm_free_pages->dram_size_1 > 0) {
			p = pm_free_pages
				    ->objs_dram_1[pm_free_pages->dram_size_1 -
						  1];
			pm_free_pages->dram_size_1--;
		}
		break;

	case 1:
		if (pm_free_pages->dram_size_2 > 0) {
			p = pm_free_pages
				    ->objs_dram_2[pm_free_pages->dram_size_2 -
						  1];
			pm_free_pages->dram_size_2--;
		}

		break;
	case 2:
		if (pm_free_pages->nvm_size_1 > 0) {
			p = pm_free_pages
				    ->objs_nvm_1[pm_free_pages->nvm_size_1 - 1];
			pm_free_pages->nvm_size_1--;
		}
		break;

	case 3:
		if (pm_free_pages->nvm_size_2 > 0) {
			p = pm_free_pages
				    ->objs_nvm_2[pm_free_pages->nvm_size_2 - 1];
			pm_free_pages->nvm_size_2--;
		}
		break;
	};

	return p;
}
