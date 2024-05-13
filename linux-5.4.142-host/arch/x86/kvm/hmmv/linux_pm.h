
extern void fill_epte(struct vm_context* vmc, struct page_node page);
extern int move_to_new_page(struct page *newpage, struct page *page,
			    enum migrate_mode mode);
extern struct page *alloc_new_node_page(struct page *page, unsigned long node);
extern int expected_page_refs(struct address_space *mapping, struct page *page);

extern u64 lcd_hva_to_pfn(struct mm_struct *mm, u64 hva);
void do_refill(struct vm_context *vmc, struct page_node pagei, int level){
	struct mm_struct *mm = vmc->kvm->mm;
	struct page_node arg;
	struct page* page;
	int l;
	int err;
	u64 hva;
	hva = gfn_to_hva(vmc->kvm, pagei.gfn);
	arg.gfn=pagei.gfn;
	arg.level=level;
	arg.spte=pagei.spte;
	arg.new_pfn=lcd_hva_to_pfn(mm, hva);
	if(arg.new_pfn==0){
		err = 1;
		goto refill_err;
	}

	page = pfn_to_page(arg.new_pfn);
	if(page==NULL){
		err = 2;
		goto refill_err;
	}
	if(PageTransHuge(page)){
		l = 2;
	}
	else l = 1;
	if(l != level){
		err = 3;
		goto refill_err;
	}

	fill_epte(vmc, arg);
	return;

refill_err:
	printk("level err %d: gfn %#lx old pfn %#lx new pfn %#lx\n",err, pagei.gfn, pagei.pfn, arg.new_pfn);
}

int migrate_regular_or_tran_page(struct vm_context *vmc, struct page_node pagei, struct page *page)
{
	int rc;
	int ret;
	int node = 0;
	bool is_lru;
	struct page *newpage;
	struct anon_vma *anon_vma = NULL;
	struct address_space *mapping;

	page = compound_head(page);
	isolate_lru_page(page);

	is_lru = !__PageMovable(page);
	//for freed page.. no use
	if (page_count(page) == 1) {
		printk("page %#lx is free\n", pagei.pfn);
		ClearPageActive(page);
		ClearPageUnevictable(page);
		if (unlikely(__PageMovable(page))) {
			lock_page(page);
			if (!PageMovable(page))
				__ClearPageIsolated(page);
			unlock_page(page);
		}
		return 0;
	}

	//lock old page
	if (!trylock_page(page)) {
		printk("can lock old page\n");
		lock_page(page);
	}

	node = pagei.target_node;
	if(node==-1||node==DRAM_NODE){
       		newpage = alloc_new_node_page(page,DRAM_NODE);
     }else{
       		newpage = alloc_new_node_page(page,NVM_NODE);
     }	

	//write for writeback
	if (PageWriteback(page)) {
		wait_on_page_writeback(page);
	}
	//delays freeing anon_vma pointer until the end of migration.
	if (PageAnon(page) && !PageKsm(page))
		anon_vma = page_get_anon_vma(page);
	//lock new page
	if (unlikely(!trylock_page(newpage))) {
		printk("unlock new page\n");
		goto out_unlock;
	}
	//no idea
	if (unlikely(!is_lru)) {
		printk("!is_lru\n");
		rc = move_to_new_page(newpage, page, MIGRATE_SYNC);
		goto out_unlock_both;
	}

	//unamp all mapping of old page
	try_to_unmap(page,
		     TTU_MIGRATION | TTU_IGNORE_MLOCK | TTU_IGNORE_ACCESS);
	mapping = page_mapping(page);
	if (!mapping) { //匿名页
		rc = migrate_page(mapping, newpage, page, MIGRATE_SYNC);
	} else if (mapping->a_ops->migratepage) {
		printk("mapping->a_ops\n");
		rc = mapping->a_ops->migratepage(mapping, newpage, page,
						 MIGRATE_SYNC);
	} else {
		printk("fallback\n");
	}

	if (rc == MIGRATEPAGE_SUCCESS) {
		if (__PageMovable(page))
			__ClearPageIsolated(page);
		if (!PageMappingFlags(page))
			page->mapping = NULL;

		if (likely(!is_zone_device_page(newpage)))
			flush_dcache_page(newpage);
	}

	//remove the mapping of old page to new page
	remove_migration_ptes(page, rc == MIGRATEPAGE_SUCCESS ? newpage : page,
			      false);

out_unlock_both:

	//if(rc==MIGRATEPAGE_SUCCESS)do_refill(vmc, pagei, pagei.level);
	unlock_page(newpage);
out_unlock:
	if (anon_vma)
		put_anon_vma(anon_vma);
	unlock_page(page);

	if (rc == MIGRATEPAGE_SUCCESS) {
		put_page(page);
		if (unlikely(!is_lru))
			put_page(newpage);
		else
			putback_lru_page(newpage);
		ret = 1;
	} else {
		put_page(newpage);
		ret = 0;
	}

	return ret;
}

void __begin_migration(struct vm_context *vmc)
{
	int i;
	int ret_thp;
	int thp_size;
	int ret_rp;
	int rp_size;
	struct page *page;
	struct page_node *pages;
	int size;
    int j;
	int rc;
	migrate_prep();

    j = 2;
    while(j--){
        if(j){
	        pages = vmc->pm_nvm_pages;
	        size = vmc->nvm_page_index;
        }else{
	        pages = vmc->pm_dram_pages;
	        size = vmc->dram_page_index;
        }
    
	    printk("j %d size %d\n",j, size);

	ret_thp = thp_size = ret_rp = rp_size = 0;

	for (i = 0; i < size; i++) {
		page = pfn_to_page(pages[i].pfn);
		if (PageTransHuge(page)) {
			if (PageHuge(page)) {
			} else {
				rc = migrate_regular_or_tran_page(vmc,
					pages[i], page);
				if(rc){
					//do_refill(vmc, pages[i], 2);
				}
            	ret_thp += rc;
				thp_size++;
			}
		} else {
			rc = migrate_regular_or_tran_page(vmc,pages[i], page);
			if(rc){
					//do_refill(vmc, pages[i], 1);
			}
		    ret_rp += rc;
			rp_size++;
		}
	}

	printk("thp %d rp %d\n", thp_size, rp_size);
	printk("success_thp %d success_rp %d\n", ret_thp, ret_rp);
	printk("success_thp_Mem %d MB, fail_thp_Mem %d MB\n", ret_thp * 2,
	       (thp_size - ret_thp) * 2);
	printk("success_rp_Mem %d MB, fail_rp_Mem %d MB\n", ret_rp / 256,
	       (rp_size - ret_rp) / 256);
	printk("success_Mem %d MB, fail_Mem %d MB\n",
	       ret_thp * 2 + ret_rp / 256,
	       (thp_size - ret_thp) * 2 + (rp_size - ret_rp) / 256);
    }
}
