#include "ss_work.h"
#include "hash.h"
#include <linux/mm.h>

#define TEST_ACCESS_BIT(X) (X & 0x20)
#define TEST_DIRTY_BIT(X) (X & 0x40)
#define TEST_A_D_BIT(X) (TEST_ACCESS_BIT(X) | TEST_DIRTY_BIT(X))

#define CLEAR_A_BIT(X) (X & ~0X20)
#define CLEAR_D_BIT(X) (X & ~0X40)
#define CLEAR_A_D_BIT(X) (X & ~0X60)

#define PAGE_SIZE (_AC(1, UL) << PAGE_SHIFT)
#define PT64_BASE_ADDR_MASK (((1ULL << 52) - 1) & ~(u64)(PAGE_SIZE - 1))
#define PT64_LEVEL_BITS 9
#define PT64_LVL_ADDR_MASK(level)                                              \
	(PT64_BASE_ADDR_MASK &                                                 \
	 ~((1ULL << (PAGE_SHIFT + (((level)-1) * PT64_LEVEL_BITS))) - 1))

ul gpte_to_gfn(ul gpte)
{
	return (gpte & PT64_LVL_ADDR_MASK(1)) >> PAGE_SHIFT;
}

static void record_log(struct vm_context *vmc, ul *gpte, int w, ul gfn)
{
	bool ret;
	/*analyze gpte to record data page access dirty */

	if (gfn < 0x100)
		return;

	ret = hash_find_gfn(vmc, gfn, 1);

	//读 1; 写 2
	vmc->busy_pages[gfn].acc_map[vmc->time_counter] = (w ? 2 : 1);

	if (!ret)
		return;

	if (w)
		*gpte = CLEAR_D_BIT(*gpte);

	*gpte = CLEAR_A_BIT(*gpte);

	return;
}

static void handle_gptp(struct vm_context *vmc, ul gptp_gfn)
{
	/*  handle VM pt, record data page access&dirty bits, clear gpte */
	int i;
	ul gfn;
	ul *gptes;
	struct page *page = NULL;
	struct gptp_buf_entry *gptp_buf;

	gptp_buf = vmc->gptp_buf;
	page = gptp_buf[gptp_gfn].page;

	if (page) {
		gptes = page_address(page);
		if (!gptes) {
			printk("gptes is NULL\n");
			vmc->enable_flag = false;
			return;
		}
		/*for (i = 0; i < 512; i++) {
			//check gpte
			if ((gptes[i] & 1) == 0) //|| ((gptes[i]>>36)&0xff)!=0)
				continue;
			gfn = gpte_to_gfn(gptes[i]);
			if (TEST_DIRTY_BIT(gptes[i])) {
				record_log(vmc, &(gptes[i]), 1, gfn);
			} else if (TEST_ACCESS_BIT(gptes[i])) {
				record_log(vmc, &(gptes[i]), 0, gfn);
			} else {
				continue;
			}
		}*/
		//flush ept D
		flush_ept_dirty_bit_by_gfn(vmc, gptp_gfn);
	}
}

static int handle_pml(struct vm_context *vmc)
{
	int ret;
	int idx;
	int i;
	int gptp_record;
	ul *gptp_idx_buf;
	struct gptp_buf_entry *gptp_buf;

	gptp_record = 0;
	gptp_idx_buf = vmc->gptp_idx_buf;
	gptp_buf = vmc->gptp_buf;

	if (!vmc->enable_flag)
		return 1;

	for (i = 0; i < vmc->gptp_num; i++) {
		idx = gptp_idx_buf[i];
		ret = gptp_buf[idx].pml;
		if (ret) {
			if (ret > 1) {
				//printk("than 1 between two handler %d\n",ret);
				if (gptp_buf[idx].page) {
					flush_ept_dirty_bit_by_gfn(vmc, idx);
				}
			} else {
				handle_gptp(vmc, idx);
				gptp_buf[idx].pml = 0;
				gptp_record++;
			}
		}
	}
	printk("gptp_num is %d\n",gptp_record);
	return ret;
}

static ul mark_busy_page(struct kvm *kvm, gfn_t gfn, struct page_node *page)
{
	ul idx;
	ul pfn;
	ul *sptep;
	struct kvm_rmap_head *rmap_head;
	struct kvm_memory_slot *slot;
	int level;
#ifdef HMM_THP_FLAG
	level = 2;
#else
	level = 1;
#endif

	if (page->p)
		return 0;

	if (!gfn)
		return 0;

	slot = gfn_to_memslot(kvm, gfn);
	if (!slot) {
		return 0;
	}
	idx = gfn_to_index(gfn, slot->base_gfn, level);
	rmap_head = &slot->arch.rmap[level-PT_PAGE_TABLE_LEVEL][idx];

	if (!rmap_head->val) {
		sptep = NULL;
	} else if (!(rmap_head->val & 1)) {
		sptep = (ul *)rmap_head->val;
	} else {
		sptep = NULL;
		printk("desc loss\n");
	}
	if (sptep) {
		pfn = *sptep & (((1ULL << 52) - 1) & ~(u64)(PAGE_SIZE - 1));
		pfn = pfn >> 12;
		if (pfn) {
			page->p = true;
			page->gfn = gfn;
			page->pfn = pfn;
			page->spte = (ul)*sptep;
			page->current_node = pfn_to_nid(pfn);
			memset(page->acc_map, 0, SVM_TRACE_LEN * sizeof(short));
		}

		return pfn;
	}

	return 0;
}

static int c1 = 0, c2 = 0, c3 = 0, c4 = 0;
static void init_flush(struct vm_context *vmc, ul gptp_gfn)
{
	/*  handle VM pt, record data page access&dirty bits, clear gpte */
	ul gfn;
	ul *gptes;
	int i;
	int ret;
	struct page *page;
	struct gptp_buf_entry *gptp_buf;
	gptp_buf = vmc->gptp_buf;
	page = gptp_buf[gptp_gfn].page;
	//printk("gptp gfn %#lx\n",gptp_gfn);
	if (page) {
		gptes = page_address(page);
		if (gptes == NULL) {
			printk("gptes is NULL\n");
			vmc->enable_flag = false;
			return;
		}/*
		for (i = 0; i < 512; i++) {
			if ((gptes[i] & 1) == 0) {
				c1++;
				continue;
			}
			gfn = gpte_to_gfn(gptes[i]);
			if (gfn < 0x100)
				continue;
			if (gfn < vmc->page_num) { //未处理
				ret = mark_busy_page(vmc->kvm, gfn,
						     (vmc->busy_pages + gfn));
				if (ret != 0) {
					vmc->busy_page_num++;
					gptes[i] = CLEAR_A_D_BIT(gptes[i]);
					c2++;

				} else {
					c3++;
				}
			} else {
				c4++;
			}
		}*/
		//flush ept D
		flush_ept_dirty_bit_by_gfn(vmc, gptp_gfn);
	}
}
static void get_slot_free(struct vm_context *vmc, struct kvm_memory_slot *slot)
{
	int ret;
	gfn_t start_gfn;
	gfn_t end_gfn;
	struct kvm_rmap_head *slot_rmap;
	struct kvm_rmap_head *slot_rmap_end;
	int level;
#ifdef HMM_THP_FLAG
	level = 2;
#else
	level = 1;
#endif

	start_gfn = slot->base_gfn;
	end_gfn = slot->base_gfn + slot->npages - 1;
	slot_rmap = &slot->arch.rmap[level - PT_PAGE_TABLE_LEVEL][gfn_to_index(start_gfn, slot->base_gfn,
						     level)];
	slot_rmap_end = &slot->arch.rmap[level - PT_PAGE_TABLE_LEVEL][gfn_to_index(
		end_gfn, slot->base_gfn, level)];
	for (; !!slot_rmap;) {
		if (!vmc->busy_pages[start_gfn].p) {
			ret = mark_busy_page(vmc->kvm, start_gfn,
					     vmc->busy_pages + start_gfn);
			vmc->busy_pages[start_gfn].p = false;
			if (ret != 0 &&
			    vmc->busy_pages[start_gfn].current_node ==
				    DRAM_NODE &&
			    !vmc->gptp_buf[start_gfn].p) {
				vmc->busy_pages[start_gfn].f = true;
				vmc->free_page_idx_buf[vmc->free_page_num] =
					start_gfn;
				vmc->free_page_num++;
			}
		}
		if (slot_rmap < slot_rmap_end) {
			++slot_rmap;
			start_gfn += (1UL << ((level - 1) * 9));
		} else
			break;
	}
}

void get_free_pages(struct vm_context *vmc)
{
	struct kvm_memslots *memslots;
	int used_slots;
	struct kvm_memory_slot *slot;

	memslots = vmc->kvm->memslots[0];
	for (used_slots = 0; used_slots < memslots->used_slots; ++used_slots) {
		slot = id_to_memslot(memslots, used_slots);
		get_slot_free(vmc, slot);
	}
}

int gpt_scanner(void *data)
{
	int i;
	struct vm_context *vmc;

	vmc = (struct vm_context *)data;
	//flush all pages
	c1 = c2 = c3 = c4 = 0;
	for (i = 0; i < vmc->gptp_num; i++) {
		init_flush(vmc, vmc->gptp_idx_buf[i]);
	}
	printk("c1 %d c2 %d c3 %d c4 %d", c1, c2, c3, c4);
	flush_all_tlbs(vmc->kvm);
	get_free_pages(vmc);

	printk("begin loop\n");
	vmc->time_counter = 0;
	while (vmc->enable_flag) {
		//msleep(vmc->sample_period);
		msleep(vmc->sample_period*2);
		handle_pml(vmc);
		flush_all_tlbs(vmc->kvm);
		vmc->time_counter++;

		//if (vmc->time_counter == TRACE_LEN) {
		if (vmc->time_counter == 1) {
			vmc->enable_flag = false;
			break;
		}
	}

	vmc->end_time = ktime_get_real_ns();
	printk("mem_track time is %lu s\n",
	       (vmc->end_time - vmc->start_time) >> 30);

	hash_recycle(vmc);
	//pre_migration(vmc);
    pre_migration_for_single_vm(vmc);
	return 1;
}

static ul gfn_to_pfn_ss(struct kvm *kvm, ul gfn)
{
	ul idx;
	ul pfn;
	ul *sptep;
	struct kvm_rmap_head *rmap_head;
	struct kvm_memory_slot *slot;
	int level;
#ifdef HMM_THP_FLAG
	level = 2;
#else
	level = 1;
#endif

	if (!gfn)
		return 0;
	slot = gfn_to_memslot(kvm, gfn);
	if (!slot){
		printk("error\n");
		return 0;
	}
	idx = gfn_to_index(gfn, slot->base_gfn, level);
	rmap_head = &slot->arch.rmap[level-PT_PAGE_TABLE_LEVEL][idx];
	if (!rmap_head->val) {
		sptep = NULL;
	} else if (!(rmap_head->val & 1)) {
		sptep = (ul *)rmap_head->val;
	} else {
		sptep = NULL;
		printk("desc loss\n");
	}
	if (sptep) {
		pfn = *sptep & (((1ULL << 52) - 1) & ~(u64)(PAGE_SIZE - 1));
		pfn = pfn >> 12;
		return pfn;
	}

	return 0;
}


void open_mem_tracker(struct kvm_vcpu *vcpu, ul buf_addr, ul max_index)
{
	ul i;
	ul gptp_gfn;
	ul host_addr;
	ul __user *_user;
	struct vm_context *vmc;
	bool *temp = NULL;
	ul ret;
	struct gptp_buf_entry *gptp_buf;
	ul *gptp_idx_buf;

	vmc = get_vmc(vcpu->kvm);
	printk("-----------------------------------------------------------\n");
	printk("vm:%d mem_track begin. %d\n", vmc->id, vmc->monitor_timer++);

	if (vmc == NULL)
		printk("vmc is not inited\n");

	if (vmc->tracking) {
		printk("current vm is tracking\n");
		return;
	}

	if (vmc->q != 0) {
		vmc->q--;
		printk("vmc q is not zero\n");
		return;
	}

	vmc->tracking = true;


	vmc->start_time = ktime_get_real_ns();

	init_gptp_buffer(vmc);
	init_gptp_idx_buffer(vmc);
	init_hash_table(vmc);
	init_busy_pages(vmc);
	init_free_page_idx_buffer(vmc);
	init_pm_pages(vmc);

	gptp_buf = vmc->gptp_buf;
	gptp_idx_buf = vmc->gptp_idx_buf;

	if (max_index == 0) {
		vm_is_free(vmc);
		return;
	}
	//step1: guest页表页注入vmm_pt_buf
	host_addr = kvm_vcpu_gfn_to_hva_prot(vcpu, buf_addr >> 12, temp);
	for (i = 0; i < max_index; i++) {
		_user = (ul __user *)((void *)host_addr + (i << 3));
		if (__copy_from_user(&gptp_gfn, _user, sizeof(gptp_gfn))) {
			printk("error\n");
			break;
		}
		if (gptp_gfn < vmc->page_num) {
			if (gptp_gfn < 100)
				continue;
			ret = gfn_to_pfn_ss(vcpu->kvm, gptp_gfn);
			//printk("error: %#lx gfn_to_pfn error, ret %#lx\n",gptp_gfn,ret);
			if (!ret) {
				//printk("error: %#lx gfn_to_pfn error, ret %d\n",gptp_gfn,ret);
			} else {
				gptp_buf[gptp_gfn].p = true;
				gptp_buf[gptp_gfn].page = pfn_to_page(ret);
				gptp_idx_buf[vmc->gptp_num++] = gptp_gfn;
			}
		} else {
			printk("vmm_pt_buf is too small\n");
		}
	}

	vmc->enable_flag = true;
	kthread_run(gpt_scanner, (void *)vmc, "gpt_scanner");
	return;
}

void close_mem_tracker(struct kvm_vcpu *vcpu)
{
	struct vm_context *vmc;
	vmc = get_vmc(vcpu->kvm);
	vmc->q = 0;
	vmc->tracking = false;
	vmc->enable_flag = false;
	printk("close mem_tracker\n");
}

void handle_pml_log_by_ss(struct kvm_vcpu *vcpu, ul gpa)
{
	ul gfn;
	struct vm_context *vmc;
	struct gptp_buf_entry *gptp_buf;

	gfn = gpa >> 12;
	gfn = (gfn >> 9)<<9;
	vmc = get_vmc(vcpu->kvm);
	gptp_buf = vmc->gptp_buf;

	if (vmc->enable_flag && gptp_buf[gfn].p) {
		gptp_buf[gfn].pml += 1;
		printk("gpt addr : %#lx\n", gpa);
		if(gptp_buf[gfn].pml > 1)printk("two addr\n");
		return;
	}
	kvm_vcpu_mark_page_dirty(vcpu, gpa >> PAGE_SHIFT);
}
EXPORT_SYMBOL(handle_pml_log_by_ss);

/*
void show_slots(void){
        ul i;
        ul size=0;
	int used_slots;
        struct kvm_memslots * memslots;
        struct kvm_memory_slot * slot;
	printk("show slot\n");
        for(i=0; i<=1; ++i){
                memslots=kvm_global->memslots[i];
                for(used_slots = memslots->used_slots-1; used_slots>=0; used_slots--){
                        slot = id_to_memslot(memslots, used_slots);
                	size+=slot->npages; 
		       	printk("slot %d begin %#llx end %#llx\n",used_slots,slot->base_gfn,slot->base_gfn+slot->npages);
                }
        }
	printk("page num %ld\n",size);
}
*/
