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

extern int isolate_lru_page(struct page *page);
extern int putback_lru_page(struct page *page);
extern void migrate_page_copy(struct page *, struct page *);
extern bool try_to_unmap(struct page *, enum ttu_flags flags);
extern void remove_migration_ptes(struct page *old, struct page *new,
				  bool locked);
extern void fill_epte(struct vm_context *, struct page_node page);

#include "linux_pm.h"
bool linux_pm = true;
//bool linux_pm = false;

void get_pml_log_for_page_migration(struct vm_context *vmc, ul gpa)
{
	ul gfn;

	if (!vmc->migration_flag)
		return;
	if (gpa == 0)
		return;
	gfn = gpa >> 12;
/*
#ifdef HMM_THP_FLAG
	gfn = (gfn >> 9) << 9;
#endif
*/
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
	struct vm_context *vmc;
	u64 *pml_buf;
	u16 pml_idx;
	u64 gpa;

	vmx = to_vmx(vcpu);
	pml_buf = page_address(vmx->pml_pg);
	vmc = get_vmc(vcpu->kvm);

	for (pml_idx = 0; pml_idx < 512; pml_idx++) {
		gpa = pml_buf[pml_idx];
		WARN_ON(gpa & (PAGE_SIZE - 1));
		get_pml_log_for_page_migration(vmc, gpa);
	}
}

void unmap_and_mapping(struct page_node *pages, int size)
{
	int i;
	int expected_count;
	struct page *new;
	struct page *old;
	struct address_space *mapping;

	//unmap
	for (i = 0; i < size; i++) {
		if (pages[i].rc != 0)
			continue;

		old = pfn_to_page(pages[i].pfn);
		new = pfn_to_page(pages[i].new_pfn);

		//unmap the old page
		try_to_unmap(old, TTU_MIGRATION | TTU_IGNORE_MLOCK |
					  TTU_IGNORE_ACCESS);
		//printk("pagecount unmap %d\n", page_count(old));

		mapping = page_mapping(old);
		if (likely(!__PageMovable(old))) {
			if (!mapping) {
				expected_count =
					1 + is_device_private_page(old);
				if (page_count(old) != expected_count) {
					pages[i].rc = -3;
					continue;
				} else {
					//将旧页在address_space的基树结点中的数据替换为新页
					new->index = old->index;
					new->mapping = old->mapping;
					if (PageSwapBacked(old))
						__SetPageSwapBacked(new);
					pages[i].rc = 1;
				}
			} else if (mapping->a_ops->migratepage) {
				pages[i].rc = -3;
				printk("mapping->a_ops\n");
			} else {
				pages[i].rc = -3;
				//printk("mapping is null\n");
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
	int j;
	int rc;
	int size;
	int node;
	int dirty_num;
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

	for (i = 0; i < size; i++) {
		pages[i].finish = false;

		old = pfn_to_page(pages[i].pfn);
		//printk("pagecount before %d\n", page_count(old));
		if(pages[i].level==1){
			isolate_lru_page(compound_head(old));
		}
		else {
			old = compound_head(old);
			isolate_lru_page(old);
		}
		//printk("pagecount isolate %d\n", page_count(old));
/*
#ifdef HMM_THP_FLAG
		old = compound_head(old);
		isolate_lru_page(old);
#else
		isolate_lru_page(compound_head(old));
#endif
*/
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
		//lock old page
		if (!trylock_page(old)) {
			//printk("cur node %d tar node %d\n",pages[i].current_node, node);
			pages[i].rc = -1;
			continue;
		}

		node = pages[i].target_node;
		//alloc a new page
		if (node == -1 || node == DRAM_NODE) {
			new = alloc_new_node_page(old, DRAM_NODE);
		} else {
			new = alloc_new_node_page(old, NVM_NODE);
		}
		if (!new) {
			pages[i].rc = -2;
			continue;
		} else {
			pages[i].new_pfn = page_to_pfn(new);
			trylock_page(new);
		}

		//page write back
		if (PageWriteback(old))
			wait_on_page_writeback(old);

		//delays freeing anon_vma pointer until the end of migration.
		if (PageAnon(old) && !PageKsm(old))
			pages[i].anon_vma = page_get_anon_vma(old);
	}
	//printk("before flush ept\n");
	for (i = 0; i < size; i++) {
		//clear EPT D bits of pages needed to migrate
		if (pages[i].rc < 0)
			continue;
		flush_ept_dirty_bit_by_gfn(vmc, pages[i].gfn);
	}
	//printk("flush tlbs\n");
	flush_all_tlbs(vmc->kvm);

	//printk("id %d begin copy data\n", p->id);

	for (i = 0; i < size; i++) {
		//copy data form old page to new page
		if (pages[i].rc < 0)
			continue;
		migrate_page_copy(pfn_to_page(pages[i].new_pfn),
				  pfn_to_page(pages[i].pfn));
	}
	//printk("before unmapping\n");

	//printk("id %d begin unmap pages\n", p->id);
	unmap_and_mapping(pages, size);

	kvm_for_each_vcpu (i, vcpu, vmc->kvm)
		check_pml_buffer(vcpu);

	dirty_num = 0;

	//printk("id %d begin handle dirty pages\n", p->id);
	j = 2;
	while (j--) {
		for (i = 0; i < size; i++) {
			old = pfn_to_page(pages[i].pfn);
			rc = pages[i].rc;

			if (pages[i].finish || rc == -1) { //pagecount==1
				continue;
			} else if (rc == -2) { //new 申请失败，old 还在 lock
				goto unlock_old;
			} else if (rc == -3) { //cannot lock again
			    new = pfn_to_page(pages[i].new_pfn);
				goto fail;
			}
			new = pfn_to_page(pages[i].new_pfn);

			if (vmc->pm_pml_buffer[pages[i].gfn]) {
				//dirty page: copy again
				if (j == 1)
					continue;
				migrate_page_copy(new, old);
				dirty_num++;
			}
			if (rc == 1) {
				if (__PageMovable(old))
					__ClearPageIsolated(old);
				if (!PageMappingFlags(old))
					old->mapping = NULL;
				if (likely(!is_zone_device_page(new)))
					flush_dcache_page(new);
			}
		fail:
			remove_migration_ptes(old, rc == 1 ? new : old, false);
			//recover the map in EPT
			if (rc == 1) {
				fill_epte(vmc, pages[i]);
			}
			if (pages[i].anon_vma)
				put_anon_vma(pages[i].anon_vma);
			unlock_page(new);
		unlock_old:
			unlock_page(old);
			if (rc == 1) {
				put_page(old);
				if (unlikely(__PageMovable(old)))
					put_page(new);
				else
					putback_lru_page(new);
			} else {	
				if(rc==-3&&pages[i].level==1)
					put_page(new);
/*				
#ifndef HMM_THP_FLAG
                if(rc==-3)
				    put_page(new);
#endif
*/
			}
			pages[i].finish = true;
		}
	}
	//printk("dirty num is %d\n", dirty_num);
	atomic_inc(&(vmc->v));
	return 0;
}

void pm_children(struct vm_context *vmc, int index, int size, struct page_node *pages)
{
	int i;
	int num; //the page num for each thread
	int thread_num; //thread num
	int tsize;
	struct page_list *pl;
	int THREADS;
	int tt;
	int nt;

	thread_num = 0;
	atomic_set(&(vmc->v), 0);

	//THREADS = mb_shift(size) < 1024 ? 1 : 4; //小于1GB，单线程处理
	THREADS = (size/256) < 1024 ? 1 : 4;

	tsize = size / THREADS;

	printk("chilidren: size %d threads %d\n",size,THREADS);
	num = 0;
	for (i = 0; i < THREADS; i++) {
		pl = (struct page_list *)kmalloc(sizeof(struct page_list),
						 GFP_KERNEL);
		pl->id = thread_num;
		pl->pages = pages + num;
		pl->vmc = vmc;

		if (i != (THREADS - 1)) {
			tt = 0;
			nt = 0;
			while(tt<tsize){
				if(pages[num].level==1)tt++;
				else tt +=512;
				num++;nt++;
			}
			pl->size = nt;
		} else {
			pl->size = index-num;
		}
		kthread_run(copy_pages, (void *)pl, "child-%d", thread_num);
		printk("child thread %d size %d\n", thread_num, pl->size);
		thread_num++;
	}
	//阻塞等待
	while (true) {
		if (atomic_read(&(vmc->v)) == thread_num)
			break;
	}
}
void pm_to_dram(struct vm_context *vmc)
{
	/*printk("begin_migrateion to dram. page num is %d MB\n",
	       mb_shift(vmc->dram_page_index));*/
	printk("begin_migrateion to dram. page num is %d, %d MB \n",
	       (vmc->dram_page_index), (vmc->dram_page_index_pages)/256);
	pm_children(vmc, vmc->dram_page_index, vmc->dram_page_index_pages, vmc->pm_dram_pages);
	print_migration_log(vmc->pm_dram_pages, vmc->dram_page_index);
}

void pm_to_nvm(struct vm_context *vmc)
{
	/*printk("begin_migrateion to nvm. page num is %d MB\n",
	       mb_shift(vmc->nvm_page_index));*/
	printk("begin_migrateion to nvm. page num is %d, %d MB \n",
	       (vmc->nvm_page_index), (vmc->nvm_page_index_pages)/256);
	pm_children(vmc, vmc->nvm_page_index, vmc->dram_page_index_pages, vmc->pm_nvm_pages);
	print_migration_log(vmc->pm_nvm_pages, vmc->nvm_page_index);
}

void thp_pm(struct vm_context *vmc){
	int i,j,sized,sizen,indexd,indexn;
	int sizet, st;	
	int pagesize;
	int i_d,i_n;
	struct page_node *pages;
	struct page_node *pages1;
	

	sizet = 4*1024*256;  //还是每次处理4G

	sized = vmc->dram_page_index_pages;
	if(sized > 12*1024*256)
		sized = 12*1024*256;
	pages = vmc->pm_dram_pages;

	sizen = vmc->nvm_page_index_pages;
	if(sizen > 12*1024*256)
		sizen = 12*1024*256;
	pages1 = vmc->pm_nvm_pages;

	i_d = vmc->dram_page_index;
	i_n = vmc->nvm_page_index;
	while(true){
		if(sized >= sizet){
			st = 0;
			indexd = 0;
			while(st < sizet){
				if(pages[indexd].level==1)pagesize=1;
				else pagesize=512;
				if(st+pagesize>sizet)break;
				indexd++;
				st += pagesize;
			}
			pm_children(vmc, indexd, st, pages);
			print_migration_log(pages, indexd);
			pages = pages + indexd;
			sized -= st;
			i_d -= indexd;
		}
		else{
			pm_children(vmc, i_d, sized, pages);
			print_migration_log(pages, i_d);
			sized = 0;
		}

		if(sizen >= sizet){
			st = 0;
			indexn = 0;
			while(st < sizet){
				if(pages1[indexn].level==1)pagesize=1;
				else pagesize=512;
				if(st+pagesize>sizet)break;
				indexn++;
				st += pagesize;
			}
			pm_children(vmc, indexn, st, pages1);
			print_migration_log(pages1, indexn);
			pages1 = pages1 + indexn;
			sizen -= st;
			i_n -= indexn;
		}
		else{
			pm_children(vmc, i_n, sizen, pages1);
			print_migration_log(pages1, i_n);
			sizen = 0;
		}
		if (sized == 0 && sizen == 0)
			break;
	}

}

void begin_migration(struct vm_context *vmc)
{
	int pm_num;
	struct page_node *pages;
	struct page_node *pages1;
	int size;
	int size1;
	int i,j;
	int num;

	//printk("begin migration 1\n");
	//init
	init_page_migration(vmc);
	migrate_prep();

	//printk("begin migration 2\n");
	vmc->migration_flag = true;

#ifndef HMM_THP_FLAG
/*
    if(vmc->dram_page_index<8*1024*256){
        pm_to_dram(vmc);
        pm_to_nvm(vmc);
    }else{
        pm_to_nvm(vmc);
        pm_to_dram(vmc);
    }
*/
	//printk("begin migration 3\n");
	pm_to_dram(vmc);
    pm_to_nvm(vmc);
#else
	thp_pm(vmc);
/*
	i = 0;
    j = 0;
	num = 4 * 512; //每次处理4GB
	size = vmc->dram_page_index;
	if (size > 12 * 512)
		size = 12 * 512;
	pages = vmc->pm_dram_pages;


	size1 = vmc->nvm_page_index;
	if (size1 > 12 * 512)
		size1 = 12 * 512;
	pages1 = vmc->pm_nvm_pages;

	while (true) {
		if (size > 0 && size < num) {
			pm_children(vmc, size, size, pages + (num * i));
			size = 0;
		} else if (size >= num) {
			pm_children(vmc, num, num, pages + (num * i));
			size -= num;
            i++;
		}

		if (size1 > 0 && size1 < num) {
			pm_children(vmc, size1, size1, pages1 + (num * j));
			size1 = 0;
		} else if (size1 >= num) {
			pm_children(vmc, num, num, pages1 + (num * j));
			size1 -= num;
            j++;
		}
		if (size == 0 && size1 == 0)
			break;
	}
*/
	vmc->migration_flag = false;
	vfree(vmc->pm_dram_pages);
	vmc->pm_dram_pages = NULL;
	vfree(vmc->pm_nvm_pages);
	vmc->pm_nvm_pages = NULL;
	vfree(vmc->pm_pml_buffer);
	vmc->pm_pml_buffer = NULL;
#endif
}
EXPORT_SYMBOL_GPL(begin_migration);

void print_migration_log(struct page_node *pages, int size)
{
	int i;
	int err1, err2, err3;
	int sp,hp;

	sp = hp =0;
	err1 = err2 = err3 = 0;
	for (i = 0; i < size; i++) {

		if(pages[i].level==1)sp++;
		else hp++;

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
		default:
			break;
		};
	}
	printk("migration sp: %d, hp: %d\n", sp, hp);
	printk("finish page migration %d\n", size - (err1 + err2 + err3));
	if (err1 + err2 + err3 == 0)
		return;
	printk("fail page num %d\n", err1 + err2 + err3);
    if(err1!=0)
	printk("err: page count is one or cannot lock old page %d\n", err1);
    if(err2!=0)
	printk("err: cannot get new page %d\n", err2);
    if(err3!=0)
	printk("err: cannot move mapping %d\n", err3);
}

int pm(void *data)
{
	ul start, end;
	struct vm_context *vmc = (struct vm_context *)data;
	start = ktime_get_real_ns();
	if (linux_pm)
		__begin_migration(vmc);
	else
		begin_migration(vmc);
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
