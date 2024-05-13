#include <linux/page-flags.h>
#include <linux/pagemap.h>
#include <linux/vmstat.h>
#include <linux/slab.h>
#include <linux/rmap.h>
#include <linux/gfp.h>
#include <linux/migrate.h>
#include <linux/mm.h>
#include "ss_work.h"
#include "vmx/vmx.h"
#include "free_page.h"

extern int isolate_lru_page(struct page *page);
extern int putback_lru_page(struct page *page);
extern void migrate_page_copy(struct page *, struct page *);
extern bool try_to_unmap(struct page *, enum ttu_flags flags);
extern void remove_migration_ptes(struct page *old, struct page *new,
				  bool locked);
extern void fill_epte(struct kvm *kvm, struct page_node page);

#include "linux_pm.h"
//bool linux_pm = true;
bool linux_pm = false;

void get_pml_log_for_page_migration(struct kvm_vcpu *vcpu, ul gpa)
{
	ul gfn;
	struct vm_context *vmc;

	gfn = gpa >> 12;
	vmc = get_vmc(vcpu->kvm);

	if (!vmc->migration_flag)
		return;

	if (gfn < vmc->page_num) {
		vmc->pm_pml_buffer[gfn] = true;
	} else {
		printk("error: gfn is out of range of pm pml buffer\n");
	}
}
EXPORT_SYMBOL(get_pml_log_for_page_migration);

void check_pml_buffer(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx;
	u64 *pml_buf;
	u16 pml_idx;
	u64 gpa;

	vmx = to_vmx(vcpu);
	pml_buf = page_address(vmx->pml_pg);

	for (pml_idx = 0; pml_idx < 512; pml_idx++) {
		gpa = pml_buf[pml_idx];
		WARN_ON(gpa & (PAGE_SIZE - 1));
		get_pml_log_for_page_migration(vcpu, gpa);
	}
}

void unmap_and_mapping(struct page_node *pages, int size)
{
	int i;
	ul *sptep;
	int expected_count;
	struct page *new;
	struct page *old;
	struct address_space *mapping;

	//unmap
	for (i = 0; i < size; i++) {
		if (pages[i].rc != 0)
			continue;

		old = pages[i].old;
		new = pages[i].new;

		//unmap the old page
		try_to_unmap(old, TTU_MIGRATION | TTU_IGNORE_MLOCK |
					  TTU_IGNORE_ACCESS);

		mapping = page_mapping(old);
		if (likely(!__PageMovable(old))) {
			if (!mapping) {
				expected_count =
					1 + is_device_private_page(old);
				if (page_count(old) != expected_count) {
					pages[i].rc = -4;
					printk("error: %#lx cannot unmap\n",
					       pages[i].gfn);
				} else {
					//将旧页在address_space的基树结点中的数据替换为新页
					new->index = old->index;
					new->mapping = old->mapping;
					if (PageSwapBacked(old))
						__SetPageSwapBacked(new);
					pages[i].rc = 1;
				}
			} else if (mapping->a_ops->migratepage) {
				pages[i].rc = -1;
				printk("mapping->a_ops\n");
			} else {
				pages[i].rc = -1;
				printk("mapping is null\n");
			}
		}
	}
}

//define page migrate status
//  1: success
// -1: page count is one
// -2: cannot get lock first
// -3: cannot get lock second
// -4: cannot change mapping

extern struct free_mem_pool_node *free_mem_pool_head;

int copy_pages(void *data)
{
	int i;
	int rc;
	int size;
	int node;
	struct page *new;
	struct page *old;
	struct page_list *p;
	struct page_node *pages;
	struct kvm_vcpu *vcpu;
	struct vm_context *vmc;

	p = (struct page_list *)data;
	pages = p->pages;
	size = p->size;
	vmc = p->vmc;

	//printk("thread_id %d: alloc new pages\n",p->id);
	for (i = 0; i < size; i++) {
		node = pages[i].target_node;

		//get the old page
		old = pfn_to_page(pages[i].pfn);
		pages[i].old = old;

		//isolate page from lru
		isolate_lru_page(compound_head(old));

		//for freed page.. no use
		if (page_count(old) == 1) {
			ClearPageActive(old);
			ClearPageUnevictable(old);
			if (unlikely(__PageMovable(old))) {
				lock_page(old);
				if (!PageMovable(old))
					__ClearPageIsolated(old);
				unlock_page(old);
			}
			pages[i].rc = -1;
			continue;
		}

		//alloc a new page
		if (node == -1 ||
		    node == DRAM_NODE) { //cur node is 2, tar node is -1, get page from pool
			new = __alloc_pages_node(
				DRAM_NODE,
				GFP_HIGHUSER_MOVABLE | __GFP_THISNODE, 0);
		} else { //cur node is 0, tar node is -2, put page to pool
			new = __alloc_pages_node(
				NVM_NODE, GFP_HIGHUSER_MOVABLE | __GFP_THISNODE,
				0);
		}
		if (new == NULL) {
			pages[i].rc = -1;
			continue;
		}
		pages[i].new = new;
		trylock_page(new);

		//lock old page temporary
		if (!trylock_page(old)) {
			printk("cur node %d tar node %d\n",
			       pages[i].current_node, node);
			pages[i].rc = -2;
			continue;
		}

		//page write back
		if (PageWriteback(old))
			wait_on_page_writeback(old);

		//delays freeing anon_vma pointer until the end of migration.
		if (PageAnon(old) && !PageKsm(old))
			pages[i].anon_vma = page_get_anon_vma(old);
	}

	//printk("flush ept dirty bits\n");
	for (i = 0; i < size; i++) {
		//clear EPT D bits of pages needed to migrate
		if (pages[i].rc < 0)
			continue;
		flush_ept_dirty_bit_by_gfn(vmc->kvm, pages[i].gfn);
	}

	//printk("flush tlbs\n");
	flush_all_tlbs(vmc->kvm);

	//        unmap_and_mapping(pages,size);

	//printk("id %d copy data\n",p->id);
	for (i = 0; i < size; i++) {
		//copy data form old page to new page
		if (pages[i].rc < 0)
			continue;
		migrate_page_copy(pages[i].new, pages[i].old);
	}

	//printk("id %d unmap pages\n",p->id);
	unmap_and_mapping(pages, size);

	//printk("before flush PML buffer\n");
	kvm_for_each_vcpu (i, vcpu, vmc->kvm)
		check_pml_buffer(vcpu);
	//printk("after flush PML buffer\n");

	//check pml buffer and change mapping
	for (i = 0; i < size; i++) {
		old = pages[i].old;
		new = pages[i].new;
		rc = pages[i].rc;

		if (rc == -1) { //pagecount==1
			continue;
		} else if (rc == -2) { //cannot lock
			goto unlock_new;
		} else if (rc == -3) { //cannot lock again
			goto free_anon_vma;
		} else if (rc == -4) {
			goto fail;
		}

		if (vmc->pm_pml_buffer[pages[i].gfn]) {
			//dirty page: copy again
			migrate_page_copy(new, old);
		}
		if (__PageMovable(old))
			__ClearPageIsolated(old);
		if (!PageMappingFlags(old))
			old->mapping = NULL;
		if (likely(!is_zone_device_page(new)))
			flush_dcache_page(new);
	fail:
		//remove the mapping of old page to new page, if success
		//if not, rmmap old page
		remove_migration_ptes(old, rc == 1 ? new : old, false);

		//recover the map in EPT
		if (rc == 1)
			fill_epte(vmc->kvm, pages[i]);

		unlock_page(old);
	free_anon_vma:
		//put anon vma
		if (pages[i].anon_vma)
			put_anon_vma(pages[i].anon_vma);
	unlock_new:
		unlock_page(new);

		if (rc == 1) {
			//put old page
			put_page(old);
			//put new page to lru
			if (unlikely(__PageMovable(old)))
				put_page(new);
			else
				putback_lru_page(new);
		} else {
			put_page(new);
		}
	}

	//	printk("copy-data thread %d  page num %d\n",atomic_read(&(vmc->v)),size);
	atomic_inc(&(vmc->v));
	return 0;
}

void begin_migration(struct vm_context *vmc)
{
	int i;
	int num; //the page num for each thread
	int thread_num; //thread num
	int tsize;
	struct page_list *pl;
	struct page_node *pages;
	int size;
	int THREADS;

	pages = vmc->pm_pages;
	size = vmc->__page_index;

	printk("begin_migrateion. page num is %d MB\n", size / 256);

	atomic_set(&(vmc->v), 0);
	thread_num = 0;

	//init
	init_page_migration(vmc);

	THREADS = 4;
	tsize = size;
	num = size / THREADS;

	migrate_prep();

	//threads: to dram
	for (i = 0; i < THREADS; i++) {
		pl = (struct page_list *)kmalloc(sizeof(struct page_list),
						 GFP_KERNEL);
		pl->id = thread_num;
		pl->pages = pages + (num * i);
		pl->vmc = vmc;
		if (i != (THREADS - 1)) {
			pl->size = num;
			tsize -= num;
		} else {
			pl->size = tsize;
			tsize = 0;
		}
		kthread_run(copy_pages, (void *)pl, "child-%d", thread_num);
		printk("child thread-dram %d size %d\n", thread_num, pl->size);
		thread_num++;
	}

	//阻塞等待
	while (true) {
		if (atomic_read(&(vmc->v)) == thread_num)
			break;
	}
	//printk("after parallel\n");
	print_migration_log(pages, size);
}
EXPORT_SYMBOL_GPL(begin_migration);

void print_migration_log(struct page_node *pages, int size)
{
	int i;
	int err1, err2, err3, err4;

	err1 = err2 = err3 = err4 = 0;
	for (i = 0; i < size; i++) {
		switch (pages[i].rc) {
		case -1:
			err1++;
			break;
		case -2:
			err2++;
			break;
		case -3:
			err3++;
			break;
		case -4:
			err4++;
			break;
		default:
			break;
		};
	}

	printk("finish page migration %d\n",
	       size - (err1 + err2 + err3 + err4));
	if (err1 + err2 + err3 + err4 == 0)
		return;
	printk("fail page num %d\n", err1 + err2 + err3 + err4);
	printk("err: page count is one %d\n", err1);
	printk("err: cannot lock old page for writeback %d\n", err2);
	printk("err: cannot lock old page for unmap %d\n", err3);
	printk("err: cannot move mapping %d\n", err4);
}

struct kvm_rmap_head *__gfn_to_rmap(gfn_t gfn, struct kvm_memory_slot *slot)
{
	unsigned long idx;
	idx = gfn_to_index(gfn, slot->base_gfn, PT_PAGE_TABLE_LEVEL);
	return &slot->arch.rmap[0][idx];
}

int pm(void *data)
{
	ul start, end;
	struct vm_context *vmc = (struct vm_context *)data;
	start = ktime_get_real_ns();
	if (linux_pm)
		__begin_migration(vmc->pm_pages, vmc->__page_index);
	else
		begin_migration(vmc);
	vmc->migration_flag = false;
	end = ktime_get_real_ns();
	printk("page migration: time is %lus\n", (end - start) >> 30);
	insert_pool_pages(vmc);
	vmc->tracking = false;
	return 0;
}

void migration(struct vm_context *vmc)
{
	kthread_run(pm, (void *)vmc, "page migrateion");
}

void insert_pm_page(struct vm_context *vmc, struct page_node p, int tar_node)
{
	struct page_node *page;
	if (vmc->__page_index >= vmc->page_num) {
		printk("pm_pages is too small\n");
		return;
	}
	if (p.current_node == tar_node)
		return;

	page = vmc->pm_pages + vmc->__page_index;
	(vmc->__page_index)++;

	page->gfn = p.gfn;
	page->pfn = p.pfn;
	page->sptep = p.sptep;
	page->spte = p.spte;
	page->rc = 0;
	page->target_node = tar_node;
	page->current_node = p.current_node;
	return;
}

/**************************************************************************/

/*
ul* gfn_hash=NULL;
void gfn_to_pagenode(struct page_node* node,ul gfn){
        struct kvm_memory_slot* slot;
        struct kvm_rmap_head *rmap_head;
        ul pfn;
        ul *sptep;

        slot = gfn_to_memslot(kvm_global, gfn);
        if(slot==NULL)
                return;

        rmap_head = __gfn_to_rmap(gfn, slot);

        if (!rmap_head->val){
                sptep = NULL;
        }else if(!(rmap_head->val & 1)) {
                sptep = (ul *)rmap_head->val;
        }else{
                sptep = NULL;
        }
        if(sptep){

                pfn = (*sptep) & (((1ULL << 52) - 1) & ~(ul)(PAGE_SIZE-1));
                pfn = pfn>>12;
                node->gfn = gfn;
                node->pfn = pfn;
                node->sptep = (ul)sptep;
                node->spte = (ul)*sptep;
                node->rc = 0;
                node->target_node = 0;
                node->current_node = pfn_to_nid(pfn);
                if(node->target_node==node->current_node)
                        return;
                __page_index++;
        }

        return;
}

void get_vm_all_gfns(void){
         struct kvm_memslots * memslots;
        int i;
        int used_slots;
        struct kvm_memory_slot * slot;
	gfn_t start_gfn;
        gfn_t end_gfn;
        struct kvm_rmap_head *slot_rmap;
        struct kvm_rmap_head *slot_rmap_end;
	ul *sptep;	
	for(i=0; i<=1; ++i){
                memslots=kvm_global->memslots[i];
                for(used_slots = memslots->used_slots-1; used_slots>=0; used_slots--){
			slot = id_to_memslot(memslots, used_slots);
			start_gfn = slot->base_gfn;
        		end_gfn = slot->base_gfn + slot->npages - 1;
       			slot_rmap = __gfn_to_rmap(start_gfn, slot);
        		slot_rmap_end = __gfn_to_rmap(end_gfn, slot);			
			for(; !!slot_rmap; ){
				if (!slot_rmap->val)
					sptep = NULL;
        			else if (!(slot_rmap->val & 1)) {
        			        sptep = (ul *)slot_rmap->val;
        			}else{
         			       sptep = NULL;
         			       printk("desc loss\n");
        			}
       		 		if(sptep){
					if(start_gfn!=0&&!gfn_hash[start_gfn]){
                                        	gfn_hash[start_gfn]=1;                                
                                        	gfn_to_pagenode(&pages[__page_index],start_gfn);
					}      
                                }

				if(slot_rmap<slot_rmap_end){
                	        	++slot_rmap;
           				++start_gfn;
                		}else 
					break;
        		}
		}
         }

	printk("all vm pages num is %d\n",__page_index);

}

void migrate_sys_pages(void){
	int i;

//	add_pm_free_pages();
	init_pm_list();
	
	if(!gfn_hash)
		gfn_hash = (ul*)vmalloc(sizeof(ul)*PAGE_NUM);
	for(i=0;i<PAGE_NUM;i++)
		gfn_hash[i]=0;

	__page_index = 0;	
	get_vm_all_gfns();
	
	kthread_run(pm,NULL,"page migrateion");	

}

void get_gfns(struct kvm_vcpu* vcpu,ul buf_addr, ul max_index){
	int i,j;
        ul gfn;
        ul gptp;
        ul host_addr;
        ul addr;
        ul __user* _user;
        bool *temp = NULL;


	//add_pm_free_pages();
        init_pm_list();
        
	if(!gfn_hash)
                gfn_hash = (ul*)vmalloc(sizeof(ul)*PAGE_NUM);
        for(i=0;i<PAGE_NUM;i++)
                gfn_hash[i]=0;

        host_addr = kvm_vcpu_gfn_to_hva_prot(vcpu,buf_addr>>12,temp);
        for(i=0;i<max_index;i++){
                _user = (ul __user *)((void *)host_addr +(i<<3));
                if(__copy_from_user(&gptp, _user, sizeof(gptp))){
                       printk("error\n");
                       break;
                }
                //printk("gptp: %#lx\n",gptp);
                addr = kvm_vcpu_gfn_to_hva_prot(vcpu,gptp,temp);
                for(j=0;j<512;j++){
                        _user = (ul __user *)((void *)addr +(j<<3));
                        if(__copy_from_user(&gfn, _user, sizeof(gfn))){
                             printk("error\n");
                              break;
                        }
                        if((gfn&1)!=0){
                                gfn = gfn & (((1ULL << 52) - 1) & ~(u64)(PAGE_SIZE-1));
                                gfn = gfn>>12;
                                if(gfn!=0&&!gfn_hash[gfn]){
                                        gfn_hash[gfn]=1;
                                        gfn_to_pagenode(&pages[__page_index],gfn);
                                }
                        }

                }
        }
	printk("num is %d\n",__page_index);	
	kthread_run(pm,NULL,"page migrateion");	

}
*/
