#include "ss_work.h"

static int hot_threshold = 0;
extern atomic_t pool_page_num;

void pre_migration_for_single_vm(struct vm_context *vmc)
{
	int i;
	int j;
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
	int dram_page;
	int nvm_page;
    int tmp;

	dram_page = 0;
	nvm_page = 0;

	// printk("-----------------------------------------------------------\n");
	/*printk("pre_migration page num is %d MB\n",
	       mb_shift(vmc->busy_page_num));*/

	printk("pre_migration page num is %d MB\n", 
		vmc->hpage_num*2+vmc->spage_num/256);

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
	//printk("bucket list init. finished.\n");
    tmp = 0;
	for (i = 0; i < vmc->page_num; i++) {
    	if (!(vmc->busy_pages[i].p||vmc->busy_pages[i].f)){
    		continue;
        }
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
    //计算热页阈值，查看桶中的分布
	for (i = 0; i < vmc->bucket_num; i++) {
		if (mb_shift(dlist_size[i]) + mb_shift(nlist_size[i]) != 0)
			printk("w %ld dram_num %d MB nvm_num %d MB\n",
			       vmc->buckets[i], mb_shift(dlist_size[i]),
			       mb_shift(nlist_size[i]));
	}
	printk("dram %d MB nvm %d MB\n", mb_shift(dram_page), mb_shift(nvm_page));
/*
	j = dram_page;
	d_t_n = 0;
	n_t_d = 0;
    

    for (w = vmc->bucket_num - 1; j > 0 && w >= 0; w--) {
		j -= dlist_size[w];
		if (j <= 0)
			break;
		node = nlist[w]->next;
		while (node != NULL) {
			insert_pm_page(vmc, vmc->busy_pages[node->gfn],
				       DRAM_NODE);
			n_t_d++;
			j--;
			if (j <= 0)
				break;
			node = node->next;
		}
	}

	for (w = 0; (d_t_n < n_t_d) && (w <= vmc->bucket_num - 1); w++) {
		node = dlist[w]->next;
		while (node != NULL) {
			insert_pm_page(vmc, vmc->busy_pages[node->gfn],
				       NVM_NODE);
			d_t_n++;
			if (d_t_n == n_t_d)
				break;
			node = node->next;
		}
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

	printk("d_t_n %d MB n_t_d %d MB\n", mb_shift(d_t_n), mb_shift(n_t_d));
	
    if(mb_shift(vmc->dram_page_index) > 512) { //大于256mb
        vmc->q = 0;
    } else {
        vmc->q = 3;
    }
    
    migration(vmc);*/
}

/*************************************************************************************/

void pre_migration_thp_for_mutil_vm(struct vm_context *vmc)
{
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
		if (!(vmc->busy_pages[i].p||vmc->busy_pages[i].f))
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
			printk("w %ld dram_num %d MB nvm_num %d MB\n",
			       vmc->buckets[i], mb_shift(dlist_size[i]),
			       mb_shift(nlist_size[i]));
	}
	printk("dram %d MB default-dram %d MB nvm %d MB\n", dram_page * 2, DEFAULT_DRAM_THP_NUM*2, nvm_page * 2);

	sum_page = dram_page + nvm_page - dlist_size[0] - nlist_size[0];
	tmp = sum_page /10 * 9;
	vmc->dram_pages = dram_page;

	for (i = vmc->bucket_num - 1; i > 0; i--) {
		hot_page += dlist_size[i] + nlist_size[i];
		if (hot_page >= tmp) {
			hot_threshold = i;
			break;
		}
	}

	d_t_n = 0;
	n_t_d = 0;

	printk("hot_threshold %d hot_page %d MB dram_pages %ld MB "
	       "pool_pages %d MB\n",
	       hot_threshold, mb_shift(hot_page), mb_shift(vmc->dram_pages),
	       mb_shift(atomic_read(&pool_page_num)));

	// case1: hot <= dram
	if (hot_page <= vmc->dram_pages) {
		printk("hot set is smaller than dram pages\n");
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

		tmp = d_t_n;
		for (w = 0; d_t_n < n_t_d && w < hot_threshold; w++) {
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

		//如果冷页超额，继续把冷页迁移到nvm，并把这些页放入pool
		tmp = d_t_n;
		do {
			while (vmc->dram_pages > DEFAULT_DRAM_THP_NUM &&
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
		} while (vmc->dram_pages > DEFAULT_DRAM_THP_NUM && w < hot_threshold);
		printk("cold pages of dram goto pool (excess defualt) %d MB\n",
		       mb_shift(d_t_n - tmp));

		//case2: hot>  dram
	} else {
		printk("hot set is bigger than dram pages\n");
		// dram中的cold页，全部放入NVM
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
		       mb_shift(d_t_n));

		// nvm中热页往DRAM迁移，把当前DRAM吃满
		for (w = vmc->bucket_num - 1;
		     (n_t_d < d_t_n) && w >= hot_threshold; w--) {
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
		tmp = 0;
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
                tmp++;
			}
			w--;
			if (atomic_read(&pool_page_num) <= 0 ||
			    w < hot_threshold)
				break;
			node = nlist[w]->next;
		} while (w >= hot_threshold);
		printk("hot pages of nvm goto dram (pool) %d MB\n",
		       mb_shift(tmp));
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
    vfree(vmc->busy_pages);
    vmc->busy_pages = NULL;
	
    vmc->end_time = ktime_get_real_ns();
	printk("hot pages or cold pages. time is %lu s\n",
	       (vmc->end_time - vmc->start_time) >> 30);

	if (vmc->dram_page_index+vmc->nvm_page_index > 50) { //大于100mb
		migration(vmc);
		// vmc->tracking = false;
		vmc->q = 0;
	} else {
		vmc->tracking = false;
		vmc->q = 3;
	}
}


void pre_migration_fix_threshold(struct vm_context* vmc){
    int i;
    int threshold;
    int value;
    int dram_cold_size;
    int nvm_hot_size;
    struct pnode* node;
    struct pnode* dram_cold;
    struct pnode* nvm_hot;
    int m_size;
    int tmp;
    struct page_node pnode;

    dram_cold_size = nvm_hot_size = 0;
    threshold = 9;

    printk("1hot or cold begin\n");
    dram_cold = (struct pnode *)vmalloc(sizeof(struct pnode));
    dram_cold->gfn = 0;
    dram_cold->w = 0;
    dram_cold->next = NULL;
    nvm_hot = (struct pnode *)vmalloc(sizeof(struct pnode));
    nvm_hot->gfn = 0;
    nvm_hot->w = 0;
    nvm_hot->next = NULL;
    printk("hot or cold begin\n");
    for (i = 0; i < vmc->page_num; i++) {
        if (!(vmc->busy_pages[i].p||vmc->busy_pages[i].f))
            continue;
        value = cal_value(vmc, vmc->busy_pages[i]);
        if((value>=threshold)&&(vmc->busy_pages[i].current_node==NVM_NODE)){
            nvm_hot_size++;
            node = (struct pnode *)vmalloc(sizeof(struct pnode));
            node->next = nvm_hot->next;
            node->gfn = vmc->busy_pages[i].gfn;
            nvm_hot->next = node;

        }else if((value<threshold)&&(vmc->busy_pages[i].current_node==DRAM_NODE)){
            dram_cold_size++;
            node = (struct pnode *)vmalloc(sizeof(struct pnode));
            node->next = dram_cold->next;
            node->gfn = vmc->busy_pages[i].gfn;
            dram_cold->next = node;
        }

    }
    printk("dram_cold %d  nvm_hot %d\n",dram_cold_size,nvm_hot_size);
    m_size = dram_cold_size>nvm_hot_size? nvm_hot_size:dram_cold_size;
    tmp = m_size;

    node = dram_cold->next;
    while(node!=NULL && tmp--){
        insert_pm_page(vmc, vmc->busy_pages[node->gfn],
                       NVM_NODE);
        node = node->next;
    }
    node = nvm_hot->next;
    while(m_size-- && node!=NULL){
        insert_pm_page(vmc, vmc->busy_pages[node->gfn],
                       DRAM_NODE);
        node = node->next;
    }
    printk("to dram %d to nvm %d\n",vmc->dram_page_index,vmc->nvm_page_index);
    migration(vmc);
}


void pre_migration_for_single_vm_huge_and_small(struct vm_context *vmc)
{
	int i;
	int j;
	long w;
	int d_t_n;
	int n_t_d;
	int *dlist_size;
	int *nlist_size;

	int *dlist_hsize;
	int *nlist_hsize;
	int *dlist_ssize;
	int *nlist_ssize;
	
	struct pnode **dlist;
	struct pnode **nlist;
	struct pnode *node;
	struct pnode *nodex;

	// for pool
	int dram_page;
	int nvm_page;
    int tmp;

	int pagesize;
	int w_threshold;

	dram_page = 0;
	nvm_page = 0;

	int count_access_pages=0, count_tmp=0, count_smalls=0;

	// printk("-----------------------------------------------------------\n");
	/*printk("pre_migration page num is %d MB\n",
	       mb_shift(vmc->busy_page_num));*/

	printk("pre_migration page num is %d MB\n", 
		vmc->hpage_num*2+vmc->spage_num/256);

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

	dlist_hsize = (int *)kmalloc(vmc->bucket_num * sizeof(int), GFP_KERNEL);
	nlist_hsize = (int *)kmalloc(vmc->bucket_num * sizeof(int), GFP_KERNEL);
	dlist_ssize = (int *)kmalloc(vmc->bucket_num * sizeof(int), GFP_KERNEL);
	nlist_ssize = (int *)kmalloc(vmc->bucket_num * sizeof(int), GFP_KERNEL);

	//链表数组初始化，分别保存dram和nvm
	for (i = 0; i < vmc->bucket_num; i++) {
		node = (struct pnode *)kmalloc(sizeof(struct pnode),
					       GFP_KERNEL);
		node->gfn = 0;
		node->w = i;
		node->level = 2;
		node->next = NULL;
		dlist[i] = node;
		dlist_size[i] = 0;
		dlist_hsize[i] = 0;
		dlist_ssize[i] = 0;

		node = (struct pnode *)kmalloc(sizeof(struct pnode),
					       GFP_KERNEL);
		node->gfn = 0;
		node->w = i;
		node->level = 2;
		node->next = NULL;
		nlist[i] = node;
		nlist_size[i] = 0;
		nlist_hsize[i] = 0;
		nlist_ssize[i] = 0;
	}
	printk("bucket list init. finished.\n");
    tmp = 0;
	for (i = 0; i < vmc->page_num; i++) {
    	if (!(vmc->busy_pages[i].p||vmc->busy_pages[i].f)){
    		continue;
        }

		//实验5 大页计数
		//if(vmc->busy_pages[i].current_node==DRAM_NODE)count_smalls+=vmc->busy_pages[i].actual_access;

		//实验5 混合计数
		if(vmc->busy_pages[i].current_node==DRAM_NODE){
			if(vmc->busy_pages[i].level==1)count_smalls++;
			else {
				if(vmc->busy_pages[i].actual_access==0)count_smalls+=vmc->huge_split_th;
				else count_smalls+=vmc->busy_pages[i].actual_access;
			}
		}

		if(vmc->busy_pages[i].sp_collapse_tag)continue;

		if(vmc->busy_pages[i].level == 1)pagesize=1;
			else pagesize=512;
		//计算桶编号
    	w = get_bucket_num(vmc, cal_value_gfn(vmc, i/*vmc->busy_pages[i]*/));
        if (vmc->busy_pages[i].current_node == DRAM_NODE) {
			//printk("dram node pre mig.\n");
			node = dlist[w];

			if(pagesize==1)dlist_ssize[w]++;
			else dlist_hsize[w]++;
			dlist_size[w]+=pagesize;
			dram_page+=pagesize;
		} else {
            node = nlist[w];

			if(pagesize==1)nlist_ssize[w]++;
			else nlist_hsize[w]++;
			nlist_size[w]+=pagesize;
			nvm_page+=pagesize;
		}

		nodex = (struct pnode *)kmalloc(sizeof(struct pnode),
						GFP_KERNEL);
		nodex->gfn = vmc->busy_pages[i].gfn;
		nodex->w = w;
		nodex->level = vmc->busy_pages[i].level;
		nodex->next = node->next;
		node->next = nodex;
	}
    //计算热页阈值，查看桶中的分布
	for (i = 0; i < vmc->bucket_num; i++) {
		if (dlist_ssize[i] + dlist_hsize[i] + nlist_ssize[i] + nlist_hsize[i] != 0)
			printk("w %ld: dram_num %d MB sp %d hp %d, nvm_num %d MB sp %d hp %d\n",
			       vmc->buckets[i], (dlist_size[i])/256, dlist_ssize[i], dlist_hsize[i],
			       (nlist_size[i])/256, nlist_ssize[i], nlist_hsize[i]);
	}
	printk("dram %d MB nvm %d MB\n", (dram_page)/256, (nvm_page)/256);

	j = dram_page;
	d_t_n = 0;
	n_t_d = 0;
    

    for (w = vmc->bucket_num - 1; j > 0 && w >= 0; w--) {
		j -= dlist_size[w];
		if (j <= 0)
			break;
		node = nlist[w]->next;
		while (node != NULL) {
			if(node->level == 1)pagesize=1;
			else pagesize=512;

			if(j>=pagesize){
				insert_pm_page(vmc, vmc->busy_pages[node->gfn],
				       DRAM_NODE);

				//实验5 大页计数
				//if(pagesize == 512)count_smalls+=vmc->busy_pages[node->gfn].actual_access;
				//实验5 混合计数
				if(pagesize == 1)count_smalls++;
				else{
					if(vmc->busy_pages[i].actual_access==0)count_smalls+=vmc->huge_split_th;
					else count_smalls+=vmc->busy_pages[i].actual_access;
				}

				n_t_d+=pagesize;
				j-=pagesize;
				if(j==0)break;
			}
			
			if(j<=0)break;
			node = node->next;
		}
	}
	w_threshold = w;

	n_t_d += vmc->sp_collapse_nvm_num;

	for (w = 0; (d_t_n < n_t_d) && 
			(w <= w_threshold)/*(w <= vmc->bucket_num - 1)*/; w++) {
		node = dlist[w]->next;
		while (node != NULL) {
			if(node->level==1)pagesize=1;
			else pagesize=512;
			if(d_t_n+pagesize<=n_t_d){
				insert_pm_page(vmc, vmc->busy_pages[node->gfn],
				       NVM_NODE);
				//实验5 大页计数
				//count_smalls-=vmc->busy_pages[node->gfn].actual_access;
				//实验5 混合计数
				if(pagesize == 1)count_smalls--;
				else{
					if(vmc->busy_pages[i].actual_access==0)count_smalls-=vmc->huge_split_th;
					else count_smalls-=vmc->busy_pages[i].actual_access;
				}

				d_t_n+=pagesize;
			}
			
			if (d_t_n >= n_t_d)
				break;
			node = node->next;
		}
	}

	//实验五相关
	//小页计数
	/*count_access_pages = n_t_d;
	count_tmp = dlist_size[0];
	if(dlist_size[0]>d_t_n)count_access_pages += (dram_page-dlist_size[0]);
	else count_access_pages += (dram_page-d_t_n);
*/
	//大页计数

	count_access_pages = count_smalls;

	printk("!==================== dram access pages : %d %d Mb\n", count_access_pages, count_access_pages/256);

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

	printk("d_t_n %d MB n_t_d %d MB\n", (d_t_n)/256, (n_t_d)/256);
/*	
    if((vmc->dram_page_index_pages)/256 > 512) { //大于256mb
        vmc->q = 0;
    } else {
        vmc->q = 3;
    }
*/  
    migration(vmc);
}