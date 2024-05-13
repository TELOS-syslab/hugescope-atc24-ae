#include <linux/sort.h>

#include "ss_work.h"

static int hot_threshold = 0;
extern atomic_t pool_page_num;

int my_cmp(const void *a, const void *b)
{
	long *da1 = (long *)a;
	long *da2 = (long *)b;

	if (*da1 > *da2)
		return 1;
	else if (*da1 < *da2)
		return -1;
	else
		return 0;
}

void build_buckets(struct vm_context *vmc, int watch)
{
	int i;
	int j;
	int k;
	int write_w;
	int read_w;
	long value;
	int tmp_num;
	long *tmp;

#ifdef HMM_SVM_FLAG
	read_w = vmc->pp.w[0];
	write_w = vmc->pp.w[1];
#else
	if (vmc->bucket_num != 0)
		return;
	write_w = 3;
	read_w = 1;
#endif

	vmc->bucket_num=31;
	if (vmc->buckets == NULL)
		vmc->buckets =
			(long *)kmalloc((35) * sizeof(long), GFP_KERNEL);
	for(i=0; i<vmc->bucket_num; ++i){
		vmc->buckets[i]=i;
	}

/*	printk("r_w %d w_w %d\n", read_w, write_w);
	k = 0;
	tmp_num = 0;
	for (i = 1; i <= 1 + watch; i++)
		tmp_num += i;
	if (vmc->buckets == NULL)
		vmc->buckets =
			(long *)kmalloc((tmp_num) * sizeof(long), GFP_KERNEL);
	tmp = (long *)kmalloc((tmp_num) * sizeof(long), GFP_KERNEL);
	for (i = 0; i < tmp_num; ++i) {
		vmc->buckets[i] = 0;
		tmp[i] = 0;
	}
	for (i = 0; i <= watch; i++) {
		for (j = 0; j <= watch - i; j++) {
			value = read_w * j + write_w * i;
			tmp[k++] = value;
		}
	}

	sort(tmp, tmp_num, sizeof(long), my_cmp, NULL);
	vmc->buckets[0] = tmp[0];
	j = 1;
	for (i = 1; i < tmp_num; ++i) {
		if (vmc->buckets[j - 1] != tmp[i])
			vmc->buckets[j++] = tmp[i];
	}
	kfree(tmp);
	vmc->bucket_num = j;
*/
	printk("bucket num is %d\n", vmc->bucket_num);
}
int get_bucket_num(struct vm_context *vmc, long w)
{
	int i;
/*	for (i = 0; i < vmc->bucket_num - 1; i++)
		if ((vmc->buckets[i] <= w) && (w <= vmc->buckets[i + 1])) {
			return i;
		}*/
	if(w<=30)return w;

	return -1;
}

long cal_value(struct vm_context *vmc, struct page_node node)
{
	int i;
	int write_w;
	int read_w;
	long value;

	// SVM
#ifdef HMM_SVM_FLAG
	read_w = vmc->pp.w[0];
	write_w = vmc->pp.w[1];
#else
	read_w = 1;
	write_w = 3;
#endif
	value = 0;

	if(node.sp){
		if(node.sp_h){
			//如果是大页拆分的热小页，直接继承原来大页的acc map
			node = vmc->busy_pages[(node.gfn>>9)<<9];
		}
		else{
			//如果是从大页拆分出来的冷小页，感觉直接返回0有点极端了，当作读了一次
			return 1;  
		}
	}

	for (i = 0; i < TRACE_LEN; i++) {
		switch (node.acc_map[i]) {
		case 1: // read
			value += read_w;
			break;
		case 2: // write
			value += write_w;
			break;
		default:
			break;
		};

	}
	return value;
}

void pre_migration(struct vm_context *vmc)
{
	bool flag;
	int i;
	long w;
	int d_t_n;
	int n_t_d;
	int *dlist_size;
	int *nlist_size;
	struct pnode **dlist;
	struct pnode **nlist;
	struct pnode *node;
	struct pnode *nodex;

	// for pool
	int sum_page;
	int hot_page;
	int cold_page;
	int dram_page;
	int nvm_page;
	int tmp;

	hot_page = 0;
	cold_page = 0;
	dram_page = 0;
	nvm_page = 0;
	sum_page = 0;

	// printk("-----------------------------------------------------------\n");
	printk("pre_migration page num is %d MB\n",
	       mb_shift(vmc->busy_page_num));
	vmc->busy_page_num = 0;
	vmc->start_time = ktime_get_real_ns();

#ifdef HMM_SVM_FLAG
	if (!accuracy_checker(vmc)) {
		svm_train_data(&(vmc->pp), vmc->pp.threadhold, vmc->busy_pages,
			       vmc->page_num, SVM_TRACE_LEN);
		svm_train(&(vmc->pp));
		printk("svm has trained, w: %ld %ld, b: %ld\n", vmc->pp.w[0],
		       vmc->pp.w[1], *(vmc->pp.b));
	}
#endif
	build_buckets(vmc, TRACE_LEN);
	dlist = (struct pnode **)kmalloc(
		vmc->bucket_num * sizeof(struct pnode *), GFP_KERNEL);
	nlist = (struct pnode **)kmalloc(
		vmc->bucket_num * sizeof(struct pnode *), GFP_KERNEL);
	dlist_size = (int *)kmalloc(vmc->bucket_num * sizeof(int), GFP_KERNEL);
	nlist_size = (int *)kmalloc(vmc->bucket_num * sizeof(int), GFP_KERNEL);

	//链表数组初始化，分别保存dram和nvm
	for (i = 0; i < vmc->bucket_num; i++) {
		node = (struct pnode *)kmalloc(sizeof(struct pnode),
					       GFP_KERNEL);
		node->gfn = 0;
		node->w = i;
		node->next = NULL;
		dlist[i] = node;
		dlist_size[i] = 0;

		node = (struct pnode *)kmalloc(sizeof(struct pnode),
					       GFP_KERNEL);
		node->gfn = 0;
		node->w = i;
		node->next = NULL;
		nlist[i] = node;
		nlist_size[i] = 0;
	}
	printk("bucket list init. finished.\n");

	for (i = 0; i < vmc->page_num; i++) {
		if (!(vmc->busy_pages[i].p))
			continue;
		//计算桶编号
		w = get_bucket_num(vmc, cal_value(vmc, vmc->busy_pages[i]));
		if (vmc->busy_pages[i].current_node == DRAM_NODE) {
			node = dlist[w];
			dlist_size[w]++;
			dram_page++;
		} else {
			node = nlist[w];
			nlist_size[w]++;
			nvm_page++;
		}

		nodex = (struct pnode *)kmalloc(sizeof(struct pnode),
						GFP_KERNEL);
		nodex->gfn = vmc->busy_pages[i].gfn;
		nodex->w = w;
		nodex->next = node->next;
		node->next = nodex;
	}
	// printk("bucket sorting finished.\n");
	//计算热页阈值，查看桶中的分布
	for (i = 0; i < vmc->bucket_num; i++) {
		if (mb_shift(dlist_size[i]) + mb_shift(nlist_size[i]) != 0)
			printk("w %d dram_num %d MB nvm_num %d MB\n",
			       vmc->buckets[i], mb_shift(dlist_size[i]),
			       mb_shift(nlist_size[i]));
	}
    return;
	hot_page = 0;
	sum_page = dram_page + nvm_page - dlist_size[0] - nlist_size[0];
	tmp = sum_page;
	vmc->free_page_num =
		vmc->free_page_num > 12800 ? vmc->free_page_num - 12800 : 0;
	vmc->dram_pages = dram_page + vmc->free_page_num;

	for (i = vmc->bucket_num - 1; i > 0; i--) {
		hot_page += dlist_size[i] + nlist_size[i];
		if (hot_page >= tmp) {
			hot_threshold = i;
			break;
		}
	}

	d_t_n = 0;
	n_t_d = 0;
	//策略与原则
	// 1. 保证热页都能放在DRAM
	// 2. 优先交换free页到NVM，然后交换cold页到NVM
	// 3. 满足页平衡后，若free页有空余，释放80%到pool；若冷页超额，则释放DRAM
	// pages到pool，直到DRAM_PAGE_SIZE

	printk("hot_threshold %d hot_page %d MB dram_pages %d MB free_pages %d MB "
	       "pool_pages %d MB\n",
	       hot_threshold, mb_shift(hot_page), mb_shift(vmc->dram_pages),
	       mb_shift(vmc->free_page_num),
	       mb_shift(atomic_read(&pool_page_num)));
	// case1: hot <= dram
	if (hot_page <= vmc->dram_pages) {
		printk("hot set is smaller than dram pages\n");
		flag = false;
		// nvm的热页迁到dram
		for (w = vmc->bucket_num - 1, node = nlist[w]->next;
		     w >= hot_threshold; w--) {
			node = nlist[w]->next;
			while (node != NULL) {
				insert_pm_page(vmc, vmc->busy_pages[node->gfn],
					       DRAM_NODE);
				node = node->next;
				n_t_d++;
			}
		}
		printk("hot pages of nvm goto dram %d MB\n", mb_shift(n_t_d));
		//若当前DRAM上的使用页面（hot and cold pages in DRAM, hot pages of
		// NVM)小于DRAM_PAGE_NUM 继续迁移cold pages of NVM到DRAM,直到达到最低DRAM
		// size或者迁移全部cold pages of NVM 保证和free pages 置换，而不是cold
		// pages of dram
		tmp = DRAM_PAGE_NUM - (dram_page + n_t_d);
		tmp = tmp > vmc->free_page_num ? vmc->free_page_num : tmp;

		do {
			while (tmp > 0 && node != NULL) {
				insert_pm_page(vmc, vmc->busy_pages[node->gfn],
					       DRAM_NODE);
				node = node->next;
				cold_page++;
				n_t_d++;
				tmp--;
			}
			w--;
			if (w < 0)
				break;
			node = nlist[w]->next;
		} while (tmp > 0 && w >= 0);
		printk("cold pages of nvm goto dram %d MB\n",
		       mb_shift(cold_page));

		//对等地，我们应该迁移相同数量（n_t_d）到NVM
		//先从FREE PAGES选
		for (i = 0; i < vmc->free_page_num; i++) {
			if (d_t_n < n_t_d) {
				insert_pm_page(
					vmc,
					vmc->busy_pages
						[vmc->free_page_idx_buf[i]],
					NVM_NODE);
				d_t_n++;
			} else {
				break;
			}
		}
		printk("free pages of dram goto nvm %d MB\n", mb_shift(d_t_n));

		//如果free page 不够,从 cold pages中选
		tmp = d_t_n;
		w = 0;
		node = dlist[w]->next;
		for (; d_t_n < n_t_d && w < hot_threshold; w++) {
			node = dlist[w]->next;
			while (node != NULL) {
				insert_pm_page(vmc, vmc->busy_pages[node->gfn],
					       NVM_NODE);
				node = node->next;
				d_t_n++;
				if (d_t_n >= n_t_d)
					break;
			}
			if (d_t_n >= n_t_d)
				break;
		}
		printk("cold pages of dram goto nvm %d MB\n",
		       mb_shift(d_t_n - tmp));

		//注水: 如果free pages 还有剩余，向pool注入
		tmp = d_t_n;
		for (;
		     vmc->dram_pages > DRAM_PAGE_NUM && i < vmc->free_page_num;
		     ++i) {
			insert_pm_page(
				vmc, vmc->busy_pages[vmc->free_page_idx_buf[i]],
				-2);
			vmc->pre_pool_page_num++;
			vmc->dram_pages--;
			d_t_n++;
		}
		printk("free pages of dram goto pool %d MB\n",
		       mb_shift(d_t_n - tmp));

		//如果冷页超额，继续把冷页迁移到nvm，并把这些页放入pool
		tmp = d_t_n;
		do {
			while (vmc->dram_pages > DRAM_PAGE_NUM &&
			       w < hot_threshold && node != NULL) {
				insert_pm_page(vmc, vmc->busy_pages[node->gfn],
					       -2);
				vmc->pre_pool_page_num++;
				vmc->dram_pages--;
				node = node->next;
				d_t_n++;
			}
			w++;
			node = dlist[w]->next;
		} while (vmc->dram_pages > DRAM_PAGE_NUM && w < hot_threshold);
		printk("cold pages of dram goto pool (excess defualt) %d MB\n",
		       mb_shift(d_t_n - tmp));

		// case2: hot > dram
	} else {
		flag = true;
		printk("hot set is bigger than dram pages\n");
		// dram中的free页放入NVM,预留 500MB
		for (i = 0; i < vmc->free_page_num; i++) {
			insert_pm_page(
				vmc, vmc->busy_pages[vmc->free_page_idx_buf[i]],
				NVM_NODE);
			d_t_n++;
		}
		printk("free pages of dram goto nvm %d MB\n", mb_shift(d_t_n));

		// dram中的cold页，全部放入NVM
		tmp = d_t_n;
		for (w = 0; w < hot_threshold; w++) {
			node = dlist[w]->next;
			while (node != NULL) {
				insert_pm_page(vmc, vmc->busy_pages[node->gfn],
					       NVM_NODE);
				node = node->next;
				d_t_n++;
			}
		}
		printk("cold pages of dram goto nvm %d MB\n",
		       mb_shift(d_t_n - tmp));

		// nvm中热页往DRAM迁移，把当前DRAM吃满
		w = vmc->bucket_num - 1;
		node = nlist[w]->next;
		for (; (n_t_d < d_t_n) && w >= hot_threshold; w--) {
			node = nlist[w]->next;
			while (node != NULL) {
				insert_pm_page(vmc, vmc->busy_pages[node->gfn],
					       DRAM_NODE);
				node = node->next;
				n_t_d++;
				if (n_t_d >= d_t_n) {
					break;
				}
			}
			if (n_t_d >= d_t_n) {
				break;
			}
		}
		printk("hot pages of nvm goto dram %d MB\n", mb_shift(n_t_d));

		//继续将NVM中的热页往DRAM(pool)迁移
		tmp = n_t_d;
		do {
			while (node != NULL &&
			       atomic_read(&pool_page_num) > 0) {
				insert_pm_page(vmc, vmc->busy_pages[node->gfn],
					       -1);
				node = node->next;
				vmc->dram_pages++;
				atomic_sub(1, &pool_page_num);
				vmc->pool_page_num++;
				n_t_d++;
			}
			w--;
			if (atomic_read(&pool_page_num) <= 0 ||
			    w < hot_threshold)
				break;
			node = nlist[w]->next;
		} while (w >= hot_threshold);
		printk("hot pages of nvm goto dram (pool) %d MB\n",
		       mb_shift(n_t_d - tmp));
	}
	//释放
	for (w = vmc->bucket_num - 1; w >= 0; w--) {
		node = nlist[w]->next;
		while (node != NULL) {
			nodex = node;
			node = node->next;
			kfree(nodex);
		}
		kfree(nlist[w]);

		node = dlist[w]->next;
		while (node != NULL) {
			nodex = node;
			node = node->next;
			kfree(nodex);
		}
		kfree(dlist[w]);
	}
	kfree(dlist);
	kfree(nlist);

	vmc->end_time = ktime_get_real_ns();
	printk("hot pages or cold pages. time is %lu s\n",
	       (vmc->end_time - vmc->start_time) >> 30);
	if (vmc->dram_page_index+vmc->nvm_page_index > 25600) { //大于100mb
		migration(vmc);
		// vmc->tracking = false;
		vmc->q = 0;
	} else {
		vmc->tracking = false;
		vmc->q = 3;
	}
}
