#include "ss_work.h"
#include <linux/spinlock.h>
#define VM_NUM 8

struct vm_context *vmc_list[VM_NUM];
extern bool mmu_spte_update(ul *, ul);
struct kvm *kvm_global; //为了配合旧的ept scanner
EXPORT_SYMBOL(kvm_global);

atomic_t pool_page_num;

void init_vm_context(struct kvm *kvm)
{
	int i = 0;
	struct vm_context *vmc = (struct vm_context *)kmalloc(
		sizeof(struct vm_context), GFP_KERNEL);
	for (i = 0; i < VM_NUM; i++) {
		if (!vmc_list[i])
			break;
	}

    
	vmc->id = i;
	vmc->tracking = false;
	vmc->kvm = kvm;
	vmc->dram_pages = DRAM_PAGE_NUM;
	vmc->nvm_pages = NVM_PAGE_NUM;
	vmc->page_num = PAGE_NUM;
	vmc->enable_flag = false;
	vmc->monitor_timer = 0;
	vmc->sample_period = SAMPLE_PERIOD;
	vmc->pm_dram_pages = NULL;
	vmc->pm_nvm_pages = NULL;
	vmc->gptp_buf = NULL;
	vmc->gptp_idx_buf = NULL;
	vmc->hash_table = NULL;
	vmc->bucket_num = 0;
	vmc->hpage_num = 0;
	vmc->spage_num = 0;
	vmc->shadow_sp_enable = false;
	vmc->ls_monitor=1;
	vmc->ls_split=1;
	vmc->huge_split_th = SP_SPLITE_THRESHOLD;
	vmc->huge_collapse_th = SP_COLLAPSE_THRESHOLD;
	vmc->ls_tb=0;
	vmc->control_num=0;

	vmc->ept_ins_list_head = NULL;
	vmc->gfn2sp = NULL;
	vmc->ept_page_cache = NULL;
	vmc->split_lhead = NULL;
	vmc->tdp1_c = 0;
	vmc->tdp2_c = 0;
	pg_cache_alloc(vmc);

	vmc->pre_pool_page_num = 0;
	vmc->pool_page_num = 0;
	init_svm(&(vmc->pp));

	vmc_list[vmc->id] = vmc;
	kvm->vmc_id = vmc->id;
	printk("init vm context %d\n", vmc->id);

	kvm_global = kvm;
	return;
}

void destroy_vm_context(struct kvm *kvm)
{
	struct vm_context *vmc;

	vmc = vmc_list[kvm->vmc_id];
	if (!vmc)
		return;

	vfree(vmc->gptp_buf);
	vmc->gptp_buf = NULL;
	vfree(vmc->gptp_idx_buf);
	vmc->gptp_idx_buf = NULL;
	vfree(vmc->busy_pages);
	vfree(vmc->free_page_idx_buf);
	vmc->free_page_idx_buf = NULL;
	vmc->busy_pages = NULL;
	vfree(vmc->pm_dram_pages);
	vfree(vmc->pm_nvm_pages);
	vmc->pm_dram_pages = NULL;
	vmc->pm_nvm_pages = NULL;
	vfree(vmc->pm_pml_buffer);
	vmc->pm_pml_buffer = NULL;

	//当前虚拟机关闭，就应该归还可能占据的pool中的页
	if (vmc->pool_page_num > 0)
		atomic_add(vmc->pool_page_num, &pool_page_num);
	printk("current pool page num is %d MB", mb_shift(atomic_read(&pool_page_num)));
	vmc_list[kvm->vmc_id] = NULL;
	return;
}

struct vm_context *get_vmc(struct kvm *kvm)
{
	return vmc_list[kvm->vmc_id];
}

struct vm_context *get_vmc_id(int i){
	return vmc_list[i];
}

void init_gptp_buffer(struct vm_context *vmc)
{
	ul i;
	struct gptp_buf_entry *gptp_buf;
	if (!vmc->gptp_buf)
		vmc->gptp_buf = (struct gptp_buf_entry *)vmalloc_node(
			sizeof(struct gptp_buf_entry) * (vmc->page_num),0);
	gptp_buf = vmc->gptp_buf;
	for (i = 0; i < vmc->page_num; i++) {
		gptp_buf[i].p = false;
		gptp_buf[i].pfn = 0;
		gptp_buf[i].page = NULL;
		gptp_buf[i].pml = 0;
	}
	vmc->gptp_num = 0;

	return;
}

void init_free_page_idx_buffer(struct vm_context *vmc)
{
	int i;
	if (!vmc->free_page_idx_buf)
		vmc->free_page_idx_buf =
			(ul *)vmalloc(sizeof(ul) * vmc->page_num);
	for (i = 0; i < vmc->dram_pages; i++)
		vmc->free_page_idx_buf[i] = 0;
	vmc->free_page_num = 0;
	return;
}

void init_gptp_idx_buffer(struct vm_context *vmc)
{
	int i;
	if (!vmc->gptp_idx_buf)
		vmc->gptp_idx_buf =
			(ul *)vmalloc(sizeof(ul) * GPTP_IDX_BUF_SIZE);
	for (i = 0; i < GPTP_IDX_BUF_SIZE; i++)
		vmc->gptp_idx_buf[i] = 0;
	return;
}
void init_hash_table(struct vm_context *vmc)
{
	vmc->hash_table = (struct hash_node **)vmalloc_node(
		sizeof(struct hash_node *) * (PFNHASHMOD + 3), 0);
	return;
}

void init_busy_pages(struct vm_context *vmc)
{
	ul i;

	if (!vmc->busy_pages)
		vmc->busy_pages = (struct page_node *)vmalloc_node(
			sizeof(struct page_node) * vmc->page_num,0);
	for (i = 0; i < vmc->page_num; i++) {
		vmc->busy_pages[i].p = false;
		vmc->busy_pages[i].f = false;
		vmc->busy_pages[i].sp = false;
		vmc->busy_pages[i].sp_h = false;
		vmc->busy_pages[i].acc_type=0;
	}
}

void init_pm_pages(struct vm_context *vmc)
{
	if (!vmc->pm_dram_pages)
		vmc->pm_dram_pages = (struct page_node *)vmalloc_node(
			sizeof(struct page_node) * PM_NUM,0);
	vmc->dram_page_index = 0;
	vmc->dram_page_index_pages = 0;

    if (!vmc->pm_nvm_pages)
        vmc->pm_nvm_pages = (struct page_node *)vmalloc_node(
            sizeof(struct page_node) * PM_NUM,0);
    vmc->nvm_page_index = 0;
	vmc->nvm_page_index_pages = 0;
}

void init_page_migration(struct vm_context *vmc)
{
	int i;

	if (vmc->pm_pml_buffer == NULL)
		vmc->pm_pml_buffer =
			(bool *)vmalloc_node(sizeof(bool) * vmc->page_num, 0);

	for (i = 0; i < vmc->page_num; i++)
		vmc->pm_pml_buffer[i] = false;

	return;
}

void flush_all_tlbs(struct kvm *kvm)
{
	kvm_flush_remote_tlbs(kvm);
}

inline int flush_ept_dirty_bit_by_gfn(struct vm_context *vmc, ul gfn)
{
	ul idx;
	ul *sptep;
	int level;
	struct kvm *kvm;
	struct kvm_rmap_head *rmap_head;
	struct kvm_memory_slot *slot;

	kvm = vmc->kvm;
	
	level = vmc->busy_pages[gfn].level;
/*
#ifdef HMM_THP_FLAG
	level = 2;
#else
	level = 1;
#endif
*/
	slot = gfn_to_memslot(kvm, gfn);

	if (slot == NULL) {
		printk("slot is null\n");
		return -1;
	}
	idx = gfn_to_index(gfn, slot->base_gfn, level);
	rmap_head = &slot->arch.rmap[level - 1][idx];
	sptep = (ul *)(rmap_head->val);

	if (sptep) {
		if (((*sptep) & (1ull << 9)) != 0) {
			(*sptep) = (*sptep) & (~(1ull << 9));
		}
		return 1;
	}

	return 0;
}

void insert_pm_page(struct vm_context *vmc, struct page_node p, int tar_node)
{
	struct page_node *page;
	int pagesize;
	if(p.level==1)pagesize=1;
	else pagesize=512;

	if (vmc->dram_page_index >= PM_NUM||vmc->nvm_page_index >= PM_NUM) {
		printk("pm_pages is too small\n");
		return;
	}

	if (p.gfn==0 || p.current_node == tar_node)
		return;

    if(tar_node == DRAM_NODE){
        page = vmc->pm_dram_pages + vmc->dram_page_index;
	    (vmc->dram_page_index)++;
		(vmc->dram_page_index_pages)+=pagesize;
    }else{
        page = vmc->pm_nvm_pages + vmc->nvm_page_index;
	    (vmc->nvm_page_index)++;
		(vmc->nvm_page_index_pages)+=pagesize;
    }

	page->gfn = p.gfn;
	page->pfn = p.pfn;
	page->level = p.level;
	page->spte = p.spte;
	page->rc = 0;
	page->target_node = tar_node;
	page->current_node = p.current_node;
    return;
}

void insert_pool_pages(struct vm_context *vmc)
{
	atomic_add(vmc->pre_pool_page_num, &pool_page_num);
	printk("insert pool page. size is %d MB\n",
	       mb_shift(vmc->pre_pool_page_num));
	vmc->pre_pool_page_num = 0;
	vmc->pool_page_num -= vmc->pre_pool_page_num;
}

void init_free_pool(void)
{
	atomic_set(&pool_page_num, DEFUALT_DRAM_POOL_SIZE);
	printk("init dram free pool. size is %d MB\n",
	       mb_shift(DEFUALT_DRAM_POOL_SIZE));
	return;
}

void put_free_pool(void)
{
	printk("put dram free pool. size is %d MB\n",
	       mb_shift(atomic_read(&pool_page_num)));
	atomic_set(&pool_page_num, 0);
	return;
}

void vm_is_free(struct vm_context *vmc)
{
	int i;
	get_free_pages(vmc);
	vmc->free_page_num =
		vmc->free_page_num > 12800 ? vmc->free_page_num - 12800 : 0;

	for (i = 0; vmc->free_page_num > DRAM_PAGE_NUM; i++) {
		insert_pm_page(vmc, vmc->busy_pages[vmc->free_page_idx_buf[i]],
			       -2);
		vmc->pre_pool_page_num++;
		vmc->free_page_num--;
	}

    vmc->tracking = false;
	migration(vmc);
}
