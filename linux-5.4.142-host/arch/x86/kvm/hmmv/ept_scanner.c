#include "ss_work.h"
#include "lcd_ksm.h"
#include <linux/page_idle.h>

/*
#ifdef HMM_THP_FLAG
int l_hpage_level = 2;
#else
int l_hpage_level = 1;
#endif
*/
static void __rmap_clear_access_dirty_bits(struct kvm_rmap_head *rmap_head,
					   gfn_t gfn, struct vm_context *vmc)
{
	u64 *sptep;
	ul pfn;
	struct page_node *page;
	struct page *pp;

	if (!rmap_head->val) {
		sptep = NULL;
	} else if (!(rmap_head->val & 1)) {
		sptep = (u64 *)rmap_head->val;
	} else {
		sptep = NULL;
		printk("desc loss\n");
	}
	if (sptep) {
		pfn = *sptep & (((1ULL << 52) - 1) & ~(u64)(PAGE_SIZE - 1));
		pfn = pfn >> 12;
		if (vmc->l_hpage_level > 1) {
			pfn = (pfn >> 9) << 9;
		}
		if (pfn) {
			if (gfn > vmc->page_num) {
				printk("%#lx\n", gfn);
				return;
			}

			if (gfn < 0x500) {
				//printk("%#lx\n", gfn);
				return;
			}
			page = vmc->busy_pages + gfn;
			if (page->p == true &&
			    page->level != vmc->l_hpage_level) {
				printk("busy page level err!\n");
			}
			pp = NULL;
			page->p = true;
			pp = pfn_to_page(pfn);
			/*if(vmc->l_hpage_level==2){
				if(page_count(pp)!=1)printk("page_count : %d\n", page_count(pp));
			}*/
			if (!pp) {
				printk("page null\n");
				page->p = false;
			}
			page->sp = false;
			page->sp_h = false;
			page->sp_collapse_tag = false;
			page->level = vmc->l_hpage_level;
			page->gfn = gfn;
			page->pfn = pfn;
			page->spte = (ul)*sptep;
			page->actual_access = 0;
			page->current_node = pfn_to_nid(pfn);
			//printk("nid: %d\n", page->current_node);
			memset(page->acc_map, 0, SVM_TRACE_LEN * sizeof(int));
			page->acc_tb = 0;
			page->acc_type = 0;
			vmc->busy_page_num++;
			if (vmc->l_hpage_level == 1)
				vmc->spage_num++;
			else
				vmc->hpage_num++;
		}
		if (((*sptep) & ((1ull << 9) + (1ull << 8))) != 0) {
			(*sptep) = (*sptep) & (~((1ull << 9) + (1ull << 8)));
		}
	}
}

static void __rmap_log(struct kvm_rmap_head *rmap_head, gfn_t gfn,
		       struct vm_context *vmc)
{
	u64 *sptep;
	if (!rmap_head->val) {
		sptep = NULL;
	} else if (!(rmap_head->val & 1)) {
		sptep = (u64 *)rmap_head->val;
	} else {
		sptep = NULL;
		printk("desc loss\n");
	}
	if (!vmc->busy_pages[gfn].p)
		return;
	if (sptep) {
		if (((*sptep) & ((1ull << 9) + (1ull << 8))) != 0) {
			vmc->busy_pages[gfn].acc_tb++;
			if (((*sptep) & (1ull << 8)) != 0) {
				vmc->busy_pages[gfn].acc_map[vmc->time_counter] =
					1;
			}
			if (((*sptep) & (1ull << 9)) != 0) {
				vmc->busy_pages[gfn].acc_map[vmc->time_counter] =
					2;
			}

			if (vmc->l_hpage_level != vmc->busy_pages[gfn].level) {
				printk("rmap log level err! %d %d\n",
				       vmc->l_hpage_level,
				       vmc->busy_pages[gfn].level);
				vmc->busy_pages[gfn].level = vmc->l_hpage_level;
			}

			if (hash_find_gfn(vmc, gfn, vmc->l_hpage_level)) {
				(*sptep) = (*sptep) &
					   (~((1ull << 9) + (1ull << 8)));
			}
		}
	}
}

static struct kvm_rmap_head *__gfn_to_rmap(gfn_t gfn,
					   struct kvm_memory_slot *slot, int le)
{
	unsigned long idx;
	idx = gfn_to_index(gfn, slot->base_gfn, le);
	return &slot->arch.rmap[le - PT_PAGE_TABLE_LEVEL][idx];
}

static void slot_leaf_handle(struct kvm_memory_slot *slot,
			     void (*fn)(struct kvm_rmap_head *rmap_head,
					gfn_t gfn, struct vm_context *vmc),
			     struct vm_context *vmc)
{
	gfn_t start_gfn;
	gfn_t end_gfn;
	struct kvm_rmap_head *slot_rmap;
	struct kvm_rmap_head *slot_rmap_end;

	start_gfn = slot->base_gfn;
	if (vmc->l_hpage_level == 2 && (start_gfn & (1UL << 8))) {
		//start_gfn -= (1UL<<8);
		start_gfn = (start_gfn >> 9) << 9;
	}
	end_gfn = slot->base_gfn + slot->npages - 1;
	slot_rmap = __gfn_to_rmap(start_gfn, slot, vmc->l_hpage_level);
	slot_rmap_end = __gfn_to_rmap(end_gfn, slot, vmc->l_hpage_level);

	for (; !!slot_rmap;) {
		fn(slot_rmap, start_gfn, vmc);
		if (slot_rmap < slot_rmap_end) {
			++slot_rmap;
			start_gfn += (1UL << ((vmc->l_hpage_level - 1) * 9));
		} else
			break;
	}
}

static void getlog_ept_all_dirty_bits(struct vm_context *vmc)
{
	int i;
	struct kvm_memslots *memslots;
	int used_slots;
	struct kvm_memory_slot *slot;

	for (i = 0; i < KVM_ADDRESS_SPACE_NUM; ++i) {
		memslots = vmc->kvm->memslots[i];
		for (used_slots = memslots->used_slots - 1; used_slots >= 0;
		     used_slots--) {
			slot = id_to_memslot(memslots, used_slots);
			//spin_lock(&kvm->mmu_lock);
			slot_leaf_handle(slot, __rmap_log, vmc);
			//spin_unlock(&kvm->mmu_lock);
			flush_all_tlbs(vmc->kvm);
		}
	}
}

static void flush_ept_all_access_dirty_bits(struct vm_context *vmc)
{
	//struct kvm_vcpu* vcpu;
	int i;
	struct kvm_memslots *memslots;
	int used_slots;
	struct kvm_memory_slot *slot;

	for (i = 0; i < KVM_ADDRESS_SPACE_NUM; ++i) {
		memslots = vmc->kvm->memslots[i];
		for (used_slots = memslots->used_slots - 1; used_slots >= 0;
		     used_slots--) {
			slot = id_to_memslot(memslots, used_slots);
			//spin_lock(&kvm->mmu_lock);
			slot_leaf_handle(slot, __rmap_clear_access_dirty_bits,
					 vmc);
			//spin_unlock(&kvm->mmu_lock);
			flush_all_tlbs(vmc->kvm);
		}
	}
}

void ept_ins_insert(struct vm_context *vmc, u64 *sptep, int newid, u64 gfn)
{
	struct ept_instead *ei;
	int i;
	u64 *pt;

	ei = (struct ept_instead *)kmalloc(sizeof(struct ept_instead),
					   GFP_KERNEL);
	ei->sptep = sptep;
	ei->old = *sptep;
	ei->newid = newid;
	ei->gfn = gfn;
	ei->cc = 0;
	memset(ei->count, 0, sizeof(ei->count));
	ei->nxt = NULL;
	if (vmc->ept_ins_list_head == NULL)
		vmc->ept_ins_list_head = ei;
	else {
		ei->nxt = vmc->ept_ins_list_head;
		vmc->ept_ins_list_head = ei;
	}
	//printk("new sptep: %llx\n",__pa(ept_page_cache[newid]));

	// 0x507 : 010100000111
	*sptep = __pa(vmc->ept_page_cache[newid]) | (ei->old & 0x507);
	pt = (u64 *)(vmc->ept_page_cache[newid]);

	for (i = 0; i < 512; ++i) {
		pt[i] = (ei->old & ((1ull << 52) - 1));
		pt[i] += (i << 12);
		pt[i] &= (~((1ull << 9) + (1ull << 8) + (1ull << 7)));
	}
	gfn_to_eptsp_insert(vmc, gfn >> 9, ei);
}

void __rmap_ept_huge(struct kvm_rmap_head *rmap_head, gfn_t gfn,
		     struct vm_context *vmc)
{
	u64 *sptep;
	ul t;
	//hpa_t shadow_addr;

	if (!rmap_head->val)
		sptep = NULL;
	else if (!(rmap_head->val & 1)) {
		sptep = (u64 *)rmap_head->val;
	} else {
		sptep = NULL;
		printk("desc loss\n");
	}
	if (sptep) {
		if ((*sptep) & (1ull << 7)) {
			vmc->hs_count++;
			if (gfn_to_eptsp_find(vmc, gfn >> 9) != NULL) {
				// sampling
				// t = ktime_get_real_ns();
				// t %= 100;
				// if (t < 5) {
				if (vmc->ept_pc_num < vmc->ept_pc_num_max) {
					ept_ins_insert(vmc, sptep,
						       vmc->ept_pc_num, gfn);
					vmc->ept_pc_num++;
				} else {
					printk("ept_pc_num full!\n");
				}
				//}
			}
		}
	}
}

void sp_flush_ept_scan(struct vm_context *vmc)
{
	struct kvm_memslots *memslots;
	int i;
	int used_slots;
	struct kvm_memory_slot *slot;

	spin_lock(&(vmc->kvm->mmu_lock));
	vmc->shadow_sp_enable = 1;

	for (i = 0; i < KVM_ADDRESS_SPACE_NUM; ++i) {
		memslots = vmc->kvm->memslots[i];
		for (used_slots = memslots->used_slots - 1; used_slots >= 0;
		     used_slots--) {
			//kvm_mmu_slot_leaf_clear_dirty(kvm_global,id_to_memslot(memslots, used_slots));
			slot = id_to_memslot(memslots, used_slots);
			//spin_lock(&kvm_global->mmu_lock);
			slot_leaf_handle(slot, __rmap_ept_huge, vmc);
			//spin_unlock(&kvm_global->mmu_lock);
			flush_all_tlbs(vmc->kvm);
		}
	}

	spin_unlock(&(vmc->kvm->mmu_lock));
}

void ept_recover(struct vm_context *vmc)
{
	struct ept_instead *p;
	p = vmc->ept_ins_list_head;
	spin_lock(&(vmc->kvm->mmu_lock));
	while (p != NULL) {
		if (p->old != 0) {
			(*(p->sptep)) = p->old;
			//print_check_spage_ad(p->newid);
		}
		p = p->nxt;
	}

	vmc->shadow_sp_enable = 0;
	spin_unlock(&(vmc->kvm->mmu_lock));
}

void sp_split_inherit(struct vm_context *vmc, struct ept_instead *p)
{
	int i;
	u64 ngfn;
	u64 base_gfn;
	base_gfn = p->gfn;

	if (vmc->busy_pages[base_gfn].level != 2) {
		printk("inherit err??\n");
		return;
	}

	for (i = 0; i < 512; ++i) {
		ngfn = base_gfn + i;
		vmc->busy_pages[ngfn].p = true;
		vmc->busy_pages[ngfn].sp = true;
		vmc->busy_pages[ngfn].sp_h =
			((p->count[i] == 0) ? false : true);
		vmc->busy_pages[ngfn].level = 1;
		vmc->busy_pages[ngfn].gfn = ngfn;
		vmc->busy_pages[ngfn].pfn = vmc->busy_pages[base_gfn].pfn + i;
		vmc->busy_pages[ngfn].spte = vmc->busy_pages[base_gfn].spte;
		vmc->busy_pages[ngfn].current_node =
			vmc->busy_pages[base_gfn].current_node;
	}
}

void sp_do_split(struct vm_context *vmc, struct ept_instead *p)
{
	int i, cnt = 0;
	hpa_t shadow_addr;
	struct page *page;
	struct page_node arg;
	int rc;
	if (p->old == 0)
		return;

	/*for(i=0;i<512;++i){
                if(p->count[i]){
                   //printk("%d ",p->count[i]);
                   cnt++;
                }
        }*/
	cnt = p->cc;
	if (cnt <= vmc->huge_split_th) {
		vmc->ls_split += (512 - cnt);
		//spin_lock(&(vmc->kvm->mmu_lock));
		if (p->old != *(p->sptep)) {
			//printk("inconformity!\n");
			//spin_unlock(&(vmc->kvm->mmu_lock));
			return;
		}
		shadow_addr =
			(p->old) & (((1ULL << 52) - 1) & ~(u64)(PAGE_SIZE - 1));
		shadow_addr = shadow_addr >> 12;
		page = pfn_to_page(shadow_addr);

		//printk("");
		if (!trylock_page(page))
			return;
		get_page(page);
		rc = split_huge_page_to_list(page, NULL);
		if (!rc) {
			for (i = 0; i < 512; i++) {
				arg.gfn = p->gfn + i;
				arg.new_pfn = shadow_addr + i;
				arg.level = 1;
				arg.spte = p->old;
				fill_epte(vmc, arg);
			}
		}
		unlock_page(page);
		put_page(page);
		if (rc) {
			printk("split err\n");
		} else if (p->old != *(p->sptep)) {
			vmc->rpart_num++;
			split_linsert(vmc, p->gfn, shadow_addr, p->old);
			sp_split_inherit(vmc, p);
		}
		//spin_unlock(&(vmc->kvm->mmu_lock));
	}
	//vmc->ccc[cnt/50]++;
	//printk("sp cnt: %d\n", cnt);
}

void pg_cache_free(struct vm_context *vmc)
{
	int i, st;
	struct ept_instead *p;
	struct ept_instead *t1;
	int tmp[520];
	int sum;

	//ept_pc_num=0;
	for (i = 0; i < vmc->ept_pc_num; ++i) {
		memset((vmc->ept_page_cache[i]), 0, PAGE_SIZE);
		//free_page((unsigned long)(vmc->ept_page_cache[i]));
	}
	t1 = vmc->ept_ins_list_head;

	memset(tmp, 0, sizeof(tmp));
	while (t1 != NULL) {
		vmc->ccc[t1->cc / 50]++;
		tmp[t1->cc]++;
		t1 = t1->nxt;
	}
	sum = 0;
	for (i = 1; i <= vmc->huge_split_th; ++i) {
		sum += tmp[i];
		if (sum >= vmc->split_limit_num)
			break;
	}

	st = vmc->huge_split_th;

	if (i < st) {
		vmc->huge_split_th = i;
	}

	// fixed threshold test
	//vmc->huge_split_th = 10;
	//vmc->split_limit_num = 100000;

	printk("small ccc 0 : %d\n", tmp[0]);
	printk("split th actually: %d\n", vmc->huge_split_th);
	while (vmc->ept_ins_list_head != NULL) {
		p = vmc->ept_ins_list_head;
		vmc->ept_ins_list_head = vmc->ept_ins_list_head->nxt;
		vmc->busy_pages[p->gfn].actual_access = p->cc;
		if (vmc->rpart_num < vmc->split_limit_num && p->cc != 0)
			sp_do_split(vmc, p);
		kfree(p);
	}
	if (i < st) {
		vmc->huge_split_th = st;
	}
}

void sp_end_all(struct vm_context *vmc)
{
	int i;
	ept_recover(vmc);
	gfn_to_eptsp_destory(vmc);
	memset(vmc->ccc, 0, sizeof(vmc->ccc));
	vmc->rpart_num = 0;
	pg_cache_free(vmc);
	for (i = 0; i <= 10; i++) {
		printk("%d~%d : %d\n", i * 50, (i + 1) * 50, vmc->ccc[i]);
	}
	printk("split page num: %d\n", vmc->rpart_num);
	printk("ept_pc_num: %d  max %d\n", vmc->ept_pc_num,
	       vmc->ept_pc_num_max);
	printk("untracked tdp fault: %lld, tracked tdp fault: %lld\n",
	       vmc->tdp1_c, vmc->tdp2_c);
}

void sp_all_clear_ad_bits(struct vm_context *vmc)
{
	struct ept_instead *p;
	int i;
	u64 *pg;

	p = vmc->ept_ins_list_head;
	while (p != NULL) {
		if (p->old != 0) {
			pg = vmc->ept_page_cache[p->newid];
			for (i = 0; i < 512; ++i) {
				if ((pg[i] & ((1ull << 9) + (1ull << 8))) !=
				    0) {
					p->count[i]++;
					p->cc++;
					//pg[i]=pg[i]&(~((1ull << 9) + (1ull << 8)));
				}
			}
		}
		p = p->nxt;
	}
}

bool sp_check_collapse(struct vm_context *vmc, u64 gfn, int *par1,
		       struct split_page_struct *p)
{
	// 这轮刚被拆分的页面不合并，不满足热度要求的不合并
	int i;
	int j;
	int t;
	int cnt;
	u64 ngfn;

	if (vmc->busy_pages[gfn].sp)
		return false;

	cnt = p->cnt;
	/*for(i=0; i<512; ++i){
		ngfn = gfn+i;
		//printk("pfn : %#lx\n", vmc->busy_pages[ngfn].pfn);
		if(vmc->busy_pages[ngfn].p == false)return false;
		if(vmc->busy_pages[ngfn].level!=1)return false;
		//if(vmc->busy_pages[ngfn].pfn != )

		for(j=0;j<TRACE_LEN;++j){
			if(vmc->busy_pages[ngfn].acc_map[j]!=0){
				cnt++;
				break;
			}
		}

		//t = cal_value_gfn(vmc, ngfn);
		//if(t)cnt++;
	}*/
	//printk("check collapse : %d\n", cnt);
	*par1 = cnt;
	if (cnt >= vmc->huge_collapse_th)
		return true;

	return false;
}

void sp_collapse_state(struct vm_context *vmc, u64 gfn)
{
	int i;
	for (i = 0; i < 512; ++i) {
		vmc->busy_pages[gfn + i].p = false;
		if (vmc->busy_pages[gfn + i].current_node == NVM_NODE)
			vmc->sp_collapse_nvm_num++;
	}
}

static inline bool mmget_still_valid(struct mm_struct *mm)
{
	return likely(!mm->core_state);
}

static inline int khugepaged_test_exit(struct mm_struct *mm)
{
	return atomic_read(&mm->mm_users) == 0 || !mmget_still_valid(mm);
}

static inline int is_swap_pte(pte_t pte)
{
	return !pte_none(pte) && !pte_present(pte);
}

extern pmd_t *mm_find_pmd(struct mm_struct *mm, unsigned long address);

int collapse_huge_page_scan(struct vm_context *vmc, struct mm_struct *mm,
			    unsigned long address, u64 gfn)
{
	struct vm_area_struct *vma;

	pmd_t *pmd;
	pte_t *pte, *_pte;
	int ret = 0, none_or_zero = 0, result = 0, referenced = 0;
	struct page *page = NULL;
	unsigned long _address;
	spinlock_t *ptl;
	int node = NUMA_NO_NODE, unmapped = 0;
	bool writable = false;
	int i = 0;
	int j = 0;
	u64 ngfn;

	vma = NULL;
	if (likely(!khugepaged_test_exit(mm)))
		vma = find_vma(mm, address);
	if (vma == NULL)
		return -1;

	pmd = mm_find_pmd(mm, address);
	if (!pmd) {
		return -1;
	}

	pte = pte_offset_map_lock(mm, pmd, address, &ptl);

	for (_address = address, _pte = pte; _pte < pte + HPAGE_PMD_NR;
	     _pte++, _address += PAGE_SIZE) {
		pte_t pteval = *_pte;
		ngfn = gfn + i;
		if (vmc->busy_pages[ngfn].pfn != pte_pfn(pteval)) {
			printk("pfn dont march--------- %#lx  %#lx\n", gfn,
			       ngfn);

			printk("gfn hva : %#lx  ngfn hva: %#lx\n",
			       gfn_to_hva(vmc->kvm, gfn),
			       gfn_to_hva(vmc->kvm, ngfn));
			printk("ngfn rmap pfn : %#lx, pte pfn : %#lx\n",
			       vmc->busy_pages[ngfn].pfn, pte_pfn(pteval));

			ret = -55;
			goto out_unmap;
		}
		i++;
		if (is_swap_pte(pteval)) {
			ret = -2;
			goto out_unmap;
		}
		if (pte_none(pteval) || is_zero_pfn(pte_pfn(pteval))) {
			ret = -3;
			goto out_unmap;
		}
		if (!pte_present(pteval)) {
			ret = -4;
			goto out_unmap;
		}
		if (pte_write(pteval))
			writable = true;

		page = vm_normal_page(vma, _address, pteval);
		if (unlikely(!page)) {
			ret = -5;
			goto out_unmap;
		}

		if (!PageLRU(page)) {
			ret = -6;
			goto out_unmap;
		}
		if (PageLocked(page)) {
			ret = -7;
			goto out_unmap;
		}
		if (!PageAnon(page)) {
			ret = -8;
			goto out_unmap;
		}

		if (page_count(page) != 1 + PageSwapCache(page)) {
			ret = -9;
			goto out_unmap;
		}
		/*if (pte_young(pteval) ||
		    page_is_young(page) || PageReferenced(page) ||
		    mmu_notifier_test_young(vma->vm_mm, address))
			referenced++;*/
	}
	if (writable) {
		ret = HPAGE_PMD_NR;
		/*if (referenced) {
			ret = referenced;
		} else {
			ret = -10;
		}*/
	} else {
		ret = -11;
	}
out_unmap:
	pte_unmap_unlock(pte, ptl);
	return ret;
}

u64 lcd_hva_to_pfn(struct mm_struct *mm, u64 hva)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd = NULL;
	pmd_t pmde;

	pgd = pgd_offset(mm, hva);
	if (!pgd_present(*pgd))
		return 0;

	p4d = p4d_offset(pgd, hva);
	if (!p4d_present(*p4d))
		return 0;

	pud = pud_offset(p4d, hva);
	if (!pud_present(*pud))
		return 0;

	pmd = pmd_offset(pud, hva);

	pmde = *pmd;
	barrier();
	if (!pmd_present(pmde))
		return 0;
	return pmd_pfn(pmde);
}
EXPORT_SYMBOL(lcd_hva_to_pfn);

//change collapse threshold according to DRAM size
void collapse_th_change(struct vm_context *vmc, int need_split)
{
	int i, j, hline, s, tot_d, tot_tb;
	int dram_thpn = DRAM_THP_NUM * 512, ht;
	int hp;
	struct split_page_struct *p;
	int cobuckt[600];
	int cnt;
	u64 ngfn;
	s = 0;
	tot_tb = 0;
	ht = 0;

	vmc->huge_collapse_th = SP_COLLAPSE_THRESHOLD;
	if (need_split) {
		return;
	} else {
		for (i = TRACE_LEN; i >= 0; i--) {
			tot_tb += vmc->tb_map[i] * 512;
			tot_tb += vmc->tbs_map[i];
		}
		dram_thpn -= tot_tb;
		//dram_thpn/=512;
		if (dram_thpn <= 0)
			return;
		//printk("ht : %d , dram_thpn : %d\n",ht, dram_thpn);
		//printk("tot_tb : %d, DRAM : %d\n", tot_tb/512, DRAM_THP_NUM);

		memset(cobuckt, 0, sizeof(cobuckt));
		p = vmc->split_lhead;
		while (p) {
			cnt = 0;
			for (i = 0; i < 512; ++i) {
				ngfn = p->gfn + i;
				//printk("pfn : %#lx\n", vmc->busy_pages[ngfn].pfn);
				if (vmc->busy_pages[ngfn].p == false)
					break;
				if (vmc->busy_pages[ngfn].level != 1)
					break;
				//if(vmc->busy_pages[ngfn].pfn != )

				for (j = 0; j < TRACE_LEN; ++j) {
					if (vmc->busy_pages[ngfn].acc_map[j] !=
					    0) {
						cnt++;
						break;
					}
				}
				/*t = cal_value_gfn(vmc, ngfn);
				if(t)cnt++;*/
			}
			p->cnt = cnt;
			cobuckt[cnt]++;
			p = p->nxt;
		}
		for (i = 512; i >= SP_COLLAPSE_THRESHOLD_LOWER; i--) {
			dram_thpn -= 512 * cobuckt[i];
			if (dram_thpn < 0)
				break;
		}
		vmc->huge_collapse_th = i;
	}
}

void sp_collapse_all(struct vm_context *vmc, int need_split)
{
	struct split_page_struct *p;
	struct split_page_struct *pp;
	struct mm_struct *mm = vmc->kvm->mm;
	struct page_node arg;
	u64 hva;
	int success = 0;
	int passit = 0;
	int i;
	int nr;
	int par1;

	collapse_th_change(vmc, need_split);
	p = vmc->split_lhead;
	pp = NULL;
	vmc->sp_collapse_nvm_num = 0;
	while (p) {
		struct page *hpage = NULL;
		if (vmc->busy_pages[p->gfn].level == 2) {
			printk("collapse page level warn. \n");
			goto out;
		}
		par1 = 0;
		if (!sp_check_collapse(vmc, p->gfn, &par1, p)) {
			goto out;
		}
		//printk("--------------------------------\n");
		passit++;
		//hpage = NULL;
		hva = gfn_to_hva(vmc->kvm, p->gfn);
		//printk("all hva : %#lx  gfn: %#lx\n", hva, p->gfn);

		if (unlikely(!down_read_trylock(&(mm->mmap_sem))))
			goto out;

		nr = HPAGE_PMD_NR;
		//nr = collapse_huge_page_scan(vmc, mm, hva, p->gfn);

		if (nr < 0) {
			up_read(&(mm->mmap_sem));
			printk("collapse gfn %#lx\n", p->gfn);
			printk("collapse failed %d\n", nr);
			goto out;
		}

		collapse_huge_page(mm, hva, &hpage, 0, nr);

		//printk("hpage %#lx\n", hpage);
		if (!IS_ERR_OR_NULL(hpage)) {
			put_page(hpage);
			goto out;
		}
		success++;

		arg.gfn = p->gfn;
		arg.new_pfn = lcd_hva_to_pfn(mm, hva);
		arg.level = 2;
		arg.spte = p->old;
		fill_epte(vmc, arg);

		sp_collapse_state(vmc, p->gfn);

		vmc->busy_pages[p->gfn].sp_collapse_tag = true;
		vmc->busy_pages[p->gfn].current_node = DRAM_NODE;
		vmc->busy_pages[p->gfn].actual_access = par1;

		if (pp == NULL) {
			vmc->split_lhead = p->nxt;
			kfree(p);
			p = vmc->split_lhead;
			continue;
		} else {
			pp->nxt = p->nxt;
			kfree(p);
			p = pp->nxt;
			continue;
		}
		//if(success) break;
		//up_read(&(kvm_global->mm->mmap_sem));
	out:
		pp = p;
		p = p->nxt;
	}
	printk("success collapse: %d pass %d\n", success, passit);
	printk("collapse nvm pages: %d\n", vmc->sp_collapse_nvm_num);
}

int huge_page_split_select(struct vm_context *vmc)
{
	int i, hline, s, tot_d, tot_tb;
	int dram_thpn = DRAM_THP_NUM * 512, ht;
	int hp;
	s = 0;
	tot_tb = 0;
	ht = 0;
	vmc->split_limit_num = 0;

	for (i = TRACE_LEN; i >= 0; i--) {
		tot_tb += vmc->tb_map[i] * 512;
		tot_tb += vmc->tbs_map[i];
		if (DRAM_THP_NUM * 512 - tot_tb <= 0 && ht == 0) {
			ht = i;
			hp = tot_tb;
		}
	}

	for (i = TRACE_LEN; i >= ht; i--) {
		dram_thpn -= vmc->tbs_map[i];
	}
	dram_thpn /= 512;
	printk("ht : %d , dram_thpn : %d\n", ht, dram_thpn);
	printk("tot_tb : %d, DRAM : %d\n", tot_tb / 512, DRAM_THP_NUM);

	if (tot_tb < DRAM_THP_NUM * 512) {
		//vmc->huge_split_th = SP_SPLITE_THRESHOLD;
		return 0;
	}

	hp += vmc->tb_map[ht - 1] * 512;

	// adjust threshold according to the DRAM limit.
	if (vmc->ls_tb != 0 && hp > (DRAM_THP_NUM * 512 * 6) / 5) {
		if (vmc->huge_split_th < SP_SPLITE_UPPER)
			vmc->huge_split_th += 20;
	} else if (vmc->ls_tb != 0 && hp < (DRAM_THP_NUM * 512 * 21) / 20) {
		if (vmc->huge_split_th > SP_SPLITE_LOWER)
			vmc->huge_split_th -= 20;
	}
	//vmc->huge_split_th = 100; 
	vmc->huge_collapse_th = vmc->huge_split_th + 50;
	//vmc->huge_split_th=30;

	printk("hp: %d, split threshold : %d\n", hp, vmc->huge_split_th);
	vmc->ls_tb = tot_tb;

	/*
	if(vmc->ls_split >= 50)	tot_d = dram_thpn * vmc->ls_monitor / vmc->ls_split;
	else tot_d = dram_thpn*6/5;
	
	for (hline=TRACE_LEN; hline>=ht-1; hline--){
		if(hline<=0)break;
		s+=vmc->tb_map[hline];
		if(s>tot_d)break;
	}
*/
	hline = ht - 3;
	printk("hline : %d, tot_d : %d\n", hline, tot_d);
	if (hline < 1)
		hline = 1; 
	vmc->ls_monitor = 0;
	vmc->ls_split = 0;
	for (i = 0; i < vmc->page_num; i++) {
		if (!vmc->busy_pages[i].p) {
			continue;
		}
		if (vmc->busy_pages[i].level == 1)
			continue;

		if (vmc->busy_pages[i].acc_tb >= hline) {
			vmc->ls_monitor++;
			gfn_to_eptsp_tag(vmc, (i) >> 9);
		}
	}

	if (vmc->ls_monitor > dram_thpn)
		vmc->split_limit_num =
			(vmc->ls_monitor * 512 - dram_thpn * 512) /
			(512 - vmc->huge_split_th);
	else
		vmc->split_limit_num = 500;

	if (vmc->split_limit_num < 500)
		vmc->split_limit_num = 500;

	printk("split limit num : %d\n", vmc->split_limit_num);
	return 1;
}

int ept_scanner(void *data)
{
	struct vm_context *vmc;
	int i;
	int need_split;
	vmc = (struct vm_context *)data;

	vmc->start_time = ktime_get_real_ns();
	printk("begin ept scanning\n");

	init_hash_table(vmc);
	init_busy_pages(vmc);
	init_pm_pages(vmc);
	vmc->hpage_num = 0;
	vmc->spage_num = 0;

#ifdef HMM_HSPAGE_FLAG
	init_sp_all(vmc);
#endif

	vmc->l_hpage_level = 2;
	flush_ept_all_access_dirty_bits(vmc);
	vmc->l_hpage_level = 1;
	flush_ept_all_access_dirty_bits(vmc);

	vmc->time_counter = 0;
	while (vmc->enable_flag_ept_scanner) {
		msleep(vmc->sample_period);
		//msleep(6000);

		vmc->l_hpage_level = 2;
		getlog_ept_all_dirty_bits(vmc);
		vmc->l_hpage_level = 1;
		getlog_ept_all_dirty_bits(vmc);

		vmc->time_counter++;
		if (vmc->time_counter == TRACE_LEN) {
			//if (vmc->time_counter == 1) {
			vmc->enable_flag_ept_scanner = false;
			break;
		}
	}
	printk("all pages %d, huge pages %d, small pages %d\n",
	       vmc->busy_page_num, vmc->hpage_num, vmc->spage_num);

	memset(vmc->tb_map, 0, sizeof(vmc->tb_map));
	memset(vmc->tbs_map, 0, sizeof(vmc->tbs_map));
	hash_recycle(vmc);

	for (i = 0; i <= 10; ++i) {
		printk("tb_map : %d, times huge: %d, small %d\n", i,
		       vmc->tb_map[i], vmc->tbs_map[i]);
	}

	vmc->end_time = ktime_get_real_ns();
	printk("mem_track time is %lu s\n",
	       (vmc->end_time - vmc->start_time) >> 30);
	//vmc->tracking = false;
	//pre_migration_thp_for_mutil_vm(vmc);
	//pre_migration_for_single_vm(vmc);
	//pre_migration_fix_threshold(vmc);
	pre_migration_for_single_vm_huge_and_small(vmc);
	return 0;
}

int VM_NUM = 8;

int hs_ept_scanner(void *data)
{
	struct vm_context *vmc;
	int i;
	int need_split;
	vmc = (struct vm_context *)data;

	vmc->start_time = ktime_get_real_ns();
	printk("begin ept scanning\n");

	init_hash_table(vmc);
	init_busy_pages(vmc);
	init_pm_pages(vmc);
	vmc->hpage_num = 0;
	vmc->spage_num = 0;

#ifdef HMM_HSPAGE_FLAG
	init_sp_all(vmc);
#endif

	vmc->l_hpage_level = 2;
	flush_ept_all_access_dirty_bits(vmc);
	vmc->l_hpage_level = 1;
	flush_ept_all_access_dirty_bits(vmc);

	vmc->time_counter = 0;
	while (vmc->enable_flag_ept_scanner) {
		msleep(vmc->sample_period);
		//msleep(6000);

		vmc->l_hpage_level = 2;
		getlog_ept_all_dirty_bits(vmc);
		vmc->l_hpage_level = 1;
		getlog_ept_all_dirty_bits(vmc);

		vmc->time_counter++;
		if (vmc->time_counter == TRACE_LEN) {
			//if (vmc->time_counter == 1) {
			vmc->enable_flag_ept_scanner = false;
			break;
		}
	}
	printk("all pages %d, huge pages %d, small pages %d\n",
	       vmc->busy_page_num, vmc->hpage_num, vmc->spage_num);

	memset(vmc->tb_map, 0, sizeof(vmc->tb_map));
	memset(vmc->tbs_map, 0, sizeof(vmc->tbs_map));
	hash_recycle(vmc);

	for (i = 0; i <= 10; ++i) {
		printk("tb_map : %d, times huge: %d, small %d\n", i,
		       vmc->tb_map[i], vmc->tbs_map[i]);
	}

	// evaluation: base DRAM counting
	/*	int ts = 0;
	for (i = 0; i < vmc->page_num; i++) {
		if (!vmc->busy_pages[i].p){
    		continue;
        }
		if (vmc->busy_pages[i].current_node!=0)continue;
		if(vmc->busy_pages[i].acc_tb!=0)ts++;
	}
	printk("====!!DRAM small page access : %d %d Mb\n",ts, ts/256);
*/

	// evaluation: huge DRAM counting
	/*
	for (i = 0; i < vmc->page_num; i++) {
		if (!vmc->busy_pages[i].p){
    		continue;
        }
		if(vmc->busy_pages[i].level == 1) continue;

		gfn_to_eptsp_tag(vmc, (i)>>9);
		
	}
	sp_flush_ept_scan(vmc);
	msleep(SP_INTERVAL);

	sp_all_clear_ad_bits(vmc);

	sp_end_all(vmc);
*/
	vmc->end_time = ktime_get_real_ns();
	printk("mem_track time is %lu s\n",
	       (vmc->end_time - vmc->start_time) >> 30);

	// page split
	printk("begin sp tracking..\n");
	vmc->start_time = ktime_get_real_ns();

	need_split = huge_page_split_select(vmc);
	// 
	// if(HUGE_CRITICAL){
	// 	if(vmc->control_num==1){
	// 		need_split = 0;
	// 	}
	// 	else if(need_split){
	// 		vmc->split_limit_num=500;
	// 	}

	// }
	//vmc->split_limit_num = 1000000;
	if (need_split == 1) {
		vmc->l_hpage_level = 2;

		sp_flush_ept_scan(vmc);
		msleep(SP_INTERVAL);

		sp_all_clear_ad_bits(vmc);

		sp_end_all(vmc);
		vmc->ls_split = (vmc->ls_monitor * 512 - vmc->ls_split) / 512;

		printk("ls_monitor : %d, ls_split: %d\n", vmc->ls_monitor,
		       vmc->ls_split);
	} else {
		printk("dont need split\n");
	}

	vmc->end_time = ktime_get_real_ns();
	printk("sp split time is %lu s\n",
	       (vmc->end_time - vmc->start_time) >> 30);

	//msleep(5000);

	// page collapse
	printk("begin collapse pages.\n");
	vmc->start_time = ktime_get_real_ns();

	sp_collapse_all(vmc, need_split);

	vmc->end_time = ktime_get_real_ns();
	printk("sp collapse time is %lu s\n",
	       (vmc->end_time - vmc->start_time) >> 30);
	
	//vmc->tracking = false;
	//pre_migration_thp_for_mutil_vm(vmc);
	//pre_migration_for_single_vm(vmc);
	//pre_migration_fix_threshold(vmc);
	pre_migration_for_single_vm_huge_and_small(vmc);
	return 0;
}

void hs_tmm_start(void){
	int i;
	struct vm_context *vmc;
	for (i = 0; i < VM_NUM; ++i) {
		vmc = get_vmc_id(i);
		if (vmc == NULL)
			continue;
		vmc->control_num++;
		if (vmc->tracking) {
			printk("current vm is tracking\n");
			return;
		} else {
			vmc->tracking = true;
		}

		if (vmc->q != 0) {
			vmc->q--;
			vmc->tracking = false;
			printk("vmc q is not zero\n");
			return;
		}

		vmc->enable_flag_ept_scanner = true;
		kthread_run(hs_ept_scanner, (void *)vmc, "hs_ept_scanner");
	}
}

void vtmm_start(void){
	int i;
	struct vm_context *vmc;
	for (i = 0; i < VM_NUM; ++i) {
		vmc = get_vmc_id(i);
		if (vmc == NULL)
			continue;
		vmc->control_num++;
		if (vmc->tracking) {
			printk("current vm is tracking\n");
			return;
		} else {
			vmc->tracking = true;
		}

		if (vmc->q != 0) {
			vmc->q--;
			vmc->tracking = false;
			printk("vmc q is not zero\n");
			return;
		}

		vmc->enable_flag_ept_scanner = true;
		kthread_run(ept_scanner, (void *)vmc, "ept_scanner");
	}
}

/// Hypervisor interfaces to start hugescope cases.
void start_hugescope_test(int selector){
	switch(selector){
		// HS-tmm
		case 1:
			printk("HS-TMM start!\n");
			hs_tmm_start();
			break;
		// vtmm
		case 2:
			printk("vTMM start!\n");
			vtmm_start();
			break;
		// HS-share
		case 3:
			printk("HS-Share start!\n");
			kthread_run(ksm_ours, NULL, "ksm_ours");
			break;
		// ksm_huge
		case 4:
			printk("Huge-share start!\n");
			kthread_run(ksm_huge_scanner, NULL, "ksm_huge_scanner");
			break;
		// ksm_split
		case 5:
			printk("split-share start!\n");
			kthread_run(ksm_linux_split, NULL, "ksm_linux_split");
			break;
		// ingens
		case 6:
			printk("ingens start!\n");
			kthread_run(ksm_ingens, NULL, "ksm_ingens");
			break;
		// zero_scan
		case 7:
			printk("zero-sharing start!\n");
			kthread_run(ksm_zero, NULL, "ksm_zero");
			break;
	}
}
EXPORT_SYMBOL(start_hugescope_test);

void open_mem_tracker_ept_scanner(struct kvm_vcpu *vcpu, int num)
{
	// 异构内存
	struct vm_context *vmc;
	vmc = get_vmc(vcpu->kvm);
	printk("open_mem_track_with_ept_scanner\n");

	vmc->control_num++;
	if (vmc->tracking) {
		printk("current vm is tracking\n");
		return;
	} else {
		vmc->tracking = true;
	}

	if (vmc->q != 0) {
		vmc->q--;
		vmc->tracking = false;
		printk("vmc q is not zero\n");
		return;
	}

	vmc->enable_flag_ept_scanner = true;
	kthread_run(ept_scanner, (void *)vmc, "ept_scanner");
	return;
}

#define KSM_HOT_TH 6
#define KSM_SPLIT_TH 100

int split_from_cold, split_from_half;
int ksm_sp_collect(struct vm_context *vmc)
{
	int i;
	struct ept_instead *p;
	int cnt;

	ept_recover(vmc);
	gfn_to_eptsp_destory(vmc);
	memset(vmc->ccc, 0, sizeof(vmc->ccc));
	vmc->rpart_num = 0;

	for (i = 0; i < vmc->ept_pc_num; ++i) {
		memset((vmc->ept_page_cache[i]), 0, PAGE_SIZE);
		//free_page((unsigned long)(vmc->ept_page_cache[i]));
	}
	cnt = 0;
	while (vmc->ept_ins_list_head != NULL) {
		p = vmc->ept_ins_list_head;
		vmc->ept_ins_list_head = vmc->ept_ins_list_head->nxt;
		if (p->cc <= KSM_SPLIT_TH) {
			//hotbloat huge pages
			vmc->busy_pages[p->gfn].acc_type = 2;
			cnt++;
		}
		//print_sp_count(p);
		kfree(p);
	}
	return cnt;
}

int ksm_split(void *data)
{
	struct vm_context *vmc;
	int i;

	int hotnum, coldnum, halfnum;
	hotnum = 0;
	coldnum = 0;
	halfnum = 0;
	vmc = (struct vm_context *)data;

	//vmc->start_time = ktime_get_real_ns();
	printk("begin ept scanning\n");

	init_hash_table(vmc);
	init_busy_pages(vmc);
	init_pm_pages(vmc);

#ifdef HMM_HSPAGE_FLAG
	init_sp_all(vmc);
#endif

	vmc->l_hpage_level = 2;
	flush_ept_all_access_dirty_bits(vmc);
	vmc->l_hpage_level = 1;
	flush_ept_all_access_dirty_bits(vmc);

	vmc->time_counter = 0;
	vmc->enable_flag_ept_scanner = true;
	while (vmc->enable_flag_ept_scanner) {
		msleep(vmc->sample_period);

		vmc->l_hpage_level = 2;
		getlog_ept_all_dirty_bits(vmc);
		vmc->l_hpage_level = 1;
		getlog_ept_all_dirty_bits(vmc);

		vmc->time_counter++;
		if (vmc->time_counter == TRACE_LEN) {
			vmc->enable_flag_ept_scanner = false;
			break;
		}
	}
	printk("all pages %d, huge pages %d, small pages %d\n",
	       vmc->busy_page_num, vmc->hpage_num, vmc->spage_num);
	hash_recycle(vmc);

	for (i = 0; i < vmc->page_num; i++) {
		if (!vmc->busy_pages[i].p) {
			continue;
		}
		if (vmc->busy_pages[i].level == 1)
			continue;

		if (vmc->busy_pages[i].acc_tb >= KSM_HOT_TH) {
			// hot pages
			vmc->busy_pages[i].acc_type = 3;
			hotnum++;
			if (vmc->is_open_split) {
				gfn_to_eptsp_tag(vmc, (i) >> 9);
			}
		} else {
			// cold pages
			vmc->busy_pages[i].acc_type = 1;
			coldnum++;
		}
	}
	//vmc->end_time = ktime_get_real_ns();
	//printk("mem_track time is %lu s\n",
	//       (vmc->end_time - vmc->start_time) >> 30);

	// split pages
	if (vmc->is_open_split) {
		printk("begin sp tracking..\n");

		vmc->l_hpage_level = 2;

		sp_flush_ept_scan(vmc);
		msleep(5000);

		sp_all_clear_ad_bits(vmc);

		halfnum = ksm_sp_collect(vmc);
		hotnum -= halfnum;
	}

	printk("hot num: %d, cold num: %d, half num: %d\n", hotnum, coldnum,
	       halfnum);
	return 0;
}

void lcd_ksm_item_init(struct lcd_ksm_item *lcd_item, u64 gfn, u64 pfn,
		       int level)
{
	memset(lcd_item, 0, sizeof(lcd_item));
	if (!item_root) {
		lcd_item->nxt = NULL;
		item_root = lcd_item;
	} else {
		lcd_item->nxt = item_root;
		item_root = lcd_item;
	}
	lcd_item->gfn = gfn;
	lcd_item->pfn = pfn;
	lcd_item->level = level;
}

void __rmap_add_rbtree(struct kvm_rmap_head *rmap_head, gfn_t gfn,
		       struct vm_context *vmc)
{
	u64 *sptep;
	struct lcd_ksm_item *lcd_item;
	u64 pfn;
	int i;
	int ret;
	struct linux_ksm_split_list *sl;
	struct page *pp;
	//hpa_t shadow_addr;

	if (!rmap_head->val)
		sptep = NULL;
	else if (!(rmap_head->val & 1)) {
		sptep = (u64 *)rmap_head->val;
	} else {
		sptep = NULL;
		printk("desc loss\n");
	}

	/*if(!vmc->busy_pages[gfn].p){
		return;
		//printk("p bit err!\n");
	}*/
	if (sptep) {
		pfn = *sptep & (((1ULL << 52) - 1) & ~(u64)(PAGE_SIZE - 1));
		pfn = pfn >> 12;
		if (vmc->l_hpage_level > 1) {
			pfn = (pfn >> 9) << 9;
		}
		if (pfn) {
			if (vmc->l_hpage_level > 1) {
				if (!((*sptep) & (1ull << 7)))
					return;

				if (linux_ksm_flag == 1) {
					pp = pfn_to_page(pfn);
					if (!PageCompound(pp)) {
						printk("struct page is not compound\n");
						return;
					}
					if (compound_nr(pp) != 512) {
						printk("struct page order warn %d\n",
						       compound_nr(pp));
						return;
					}
					ret = 0;
					if (vmc->busy_pages[gfn].acc_type <=
					    2) {
						for (i = 1; i < 512; i++) {
							lcd_item = (struct lcd_ksm_item
									    *)
								kmalloc(sizeof(struct lcd_ksm_item),
									GFP_KERNEL);
							lcd_ksm_item_init(
								lcd_item,
								gfn + i,
								pfn + i, 3);
							ret += lcd_ksm_tree_spage_search_insert(
								lcd_item,
								pp + i);
							/*if(i>3){
								ret = memcmp_page(pp+i, pp+i-1, PAGE_SIZE);
								if(i==100)printk("memcmp %d\n", ret);
								if(ret == 0) tot_hpages++;
								ret = 0;	
							}*/
						}
					}
					if (ret != 0 &&
					    vmc->busy_pages[gfn].acc_type <=
						    2) {
						if (vmc->busy_pages[gfn]
							    .acc_type == 1)
							split_from_cold++;
						if (vmc->busy_pages[gfn]
							    .acc_type == 2)
							split_from_half++;
						//printk("split add\n");
						sl = (struct linux_ksm_split_list
							      *)
							kmalloc(sizeof(struct linux_ksm_split_list),
								GFP_KERNEL);
						sl->gfn = gfn;
						sl->pfn = pfn;
						sl->spte = (*sptep);
						sl->nxt = NULL;
						if (linux_ksm_split_list_root !=
						    NULL) {
							sl->nxt =
								linux_ksm_split_list_root;
						}
						linux_ksm_split_list_root = sl;
						vmc->rpart_num++;
					}
				}

				else {
					lcd_item = (struct lcd_ksm_item *)kmalloc(
						sizeof(struct lcd_ksm_item),
						GFP_KERNEL);
					lcd_ksm_item_init(lcd_item, gfn, pfn,
							  2);
					lcd_ksm_tree_hpage_search_insert(
						lcd_item, pfn_to_page(pfn));
				}
			} else {
				lcd_item = (struct lcd_ksm_item *)kmalloc(
					sizeof(struct lcd_ksm_item),
					GFP_KERNEL);
				lcd_ksm_item_init(lcd_item, gfn, pfn, 1);
				lcd_ksm_tree_spage_search_insert(
					lcd_item, pfn_to_page(pfn));
			}
		}
	}
}

void add_to_rbtree(struct vm_context *vmc)
{
	//struct kvm_vcpu* vcpu;
	int i;
	struct kvm_memslots *memslots;
	int used_slots;
	struct kvm_memory_slot *slot;

	for (i = 0; i < KVM_ADDRESS_SPACE_NUM; ++i) {
		memslots = vmc->kvm->memslots[i];
		for (used_slots = memslots->used_slots - 1; used_slots >= 0;
		     used_slots--) {
			slot = id_to_memslot(memslots, used_slots);
			//spin_lock(&kvm->mmu_lock);
			slot_leaf_handle(slot, __rmap_add_rbtree, vmc);
			//spin_unlock(&kvm->mmu_lock);
			//flush_all_tlbs(vmc->kvm);
		}
	}
}

void ksm_init(void)
{
	lcd_ksm_tree_root[0] = RB_ROOT;
	lcd_ksm_tree_root_huge[0] = RB_ROOT;

	spage_ksm_cnt = 0;
	hpage_ksm_cnt = 0;

	spage_rbnode_tot = 0;
	hpage_rbnode_tot = 0;

	tot_spages = 0;
	tot_hpages = 0;

	item_root = NULL;
}

void ksm_end(void)
{
	struct lcd_ksm_item *p;
	p = item_root;
	while (item_root != NULL) {
		p = item_root;
		item_root = item_root->nxt;
		if (p->level == 1 || p->level == 3)
			rb_erase(&p->node, lcd_ksm_tree_root);
		else
			rb_erase(&p->node, lcd_ksm_tree_root_huge);
		kfree(p);
	}
}

int ksm_huge_scanner(void *data)
{
	struct vm_context *vmc;
	int i;

	printk("ksm init.\n");
	ksm_init();
	printk("ksm init end.\n");

	linux_ksm_flag = 0;
	for (i = 0; i < VM_NUM; ++i) {
		vmc = get_vmc_id(i);
		if (vmc == NULL)
			continue;

		printk("ksm begin %d.\n", i);
		init_busy_pages(vmc);

		vmc->l_hpage_level = 2;
		printk("ksm huge add.\n");
		add_to_rbtree(vmc);
		vmc->l_hpage_level = 1;
		printk("ksm small add.\n");
		add_to_rbtree(vmc);

		printk("small page same %d %d MB, small rbnodes %d %d MB\n",
		       spage_ksm_cnt, spage_ksm_cnt / 256, spage_rbnode_tot,
		       spage_rbnode_tot / 256);
		printk("huge page same %d %d MB, huge rbnodes %d %d MB\n",
		       hpage_ksm_cnt, hpage_ksm_cnt * 2, hpage_rbnode_tot,
		       hpage_rbnode_tot * 2);

		printk("small page total %d %d MB, huge page total %d %d MB\n",
		       tot_spages, tot_spages / 256, tot_hpages,
		       tot_hpages * 2);
		printk("==============================\n");
	}

	ksm_end();
	printk("ksm end.\n");
	return 0;
}

void ksm_list_split(struct vm_context *vmc)
{
	struct linux_ksm_split_list *pp;
	struct page *page;
	struct page_node arg;
	int i;
	int splitnum = 0;
	int rc;

	while (linux_ksm_split_list_root != NULL) {
		page = pfn_to_page(linux_ksm_split_list_root->pfn);

		if (!trylock_page(page))
			return;
		get_page(page);
		rc = split_huge_page_to_list(page, NULL);
		if (!rc) {
			for (i = 0; i < 512; i++) {
				arg.gfn = linux_ksm_split_list_root->gfn + i;
				arg.new_pfn =
					linux_ksm_split_list_root->pfn + i;
				arg.level = 1;
				arg.spte = linux_ksm_split_list_root->spte;
				fill_epte(vmc, arg);
			}
		}
		unlock_page(page);
		put_page(page);
		if (rc) {
			printk("split err\n");
		} else {
			splitnum++;
		}

		pp = linux_ksm_split_list_root;
		linux_ksm_split_list_root = linux_ksm_split_list_root->nxt;
		kfree(pp);
	}
	printk("linux ksm split huge page: %d\n", splitnum);
}

int ksm_linux_split(void *data)
{
	struct vm_context *vmc;
	int i;

	printk("ksm linux split!!!!!!\n");
	printk("ksm init.\n");
	ksm_init();
	printk("ksm init end.\n");

	linux_ksm_flag = 1;
	linux_ksm_split_list_root = NULL;
	for (i = 0; i < VM_NUM; ++i) {
		vmc = get_vmc_id(i);
		if (vmc == NULL)
			continue;
		init_busy_pages(vmc);

		vmc->rpart_num = 0;
		printk("ksm begin %d.\n", i);

		vmc->l_hpage_level = 2;
		printk("ksm huge add.\n");
		add_to_rbtree(vmc);
		vmc->l_hpage_level = 1;
		printk("ksm small add.\n");
		add_to_rbtree(vmc);

		printk("small page same %d %d MB, small rbnodes %d %d MB\n",
		       spage_ksm_cnt, spage_ksm_cnt / 256, spage_rbnode_tot,
		       spage_rbnode_tot / 256);
		printk("huge page same %d %d MB, huge rbnodes %d %d MB\n",
		       hpage_ksm_cnt, hpage_ksm_cnt * 2, hpage_rbnode_tot,
		       hpage_rbnode_tot * 2);

		printk("small page total %d %d MB, huge page total %d %d MB\n",
		       tot_spages, tot_spages / 256, tot_hpages,
		       tot_hpages * 2);
		printk("rpart num : %d, %d MB\n", vmc->rpart_num,
		       vmc->rpart_num * 2);
		ksm_list_split(vmc);
		printk("==============================\n");
	}

	ksm_end();

	printk("ksm end.\n");

	return 0;
}

int memcmp_page(struct page *page1, struct page *page2, int pgs)
{
	char *addr1, *addr2;
	int ret;

	addr1 = kmap_atomic(page1);
	addr2 = kmap_atomic(page2);
	ret = memcmp(addr1, addr2, pgs);
	kunmap_atomic(addr2);
	kunmap_atomic(addr1);
	return ret;
}

int lcd_ksm_tree_spage_search_insert(struct lcd_ksm_item *lcd_item,
				     struct page *page)
{
	struct rb_node **new;
	struct rb_root *root;
	struct rb_node *parent = NULL;
	int pgs;

	pgs = PAGE_SIZE;

	tot_spages++;
	root = lcd_ksm_tree_root;
	new = &root->rb_node;
	//printk("----------> into spage insert\n");
	while (*new) {
		struct lcd_ksm_item *tree_lcd_item;
		struct page *tree_page;
		int ret;

		tree_lcd_item = rb_entry(*new, struct lcd_ksm_item, node);

		if (tree_lcd_item->level == 1) {
			tree_page = pfn_to_page(tree_lcd_item->pfn);
		} else {
			tree_page =
				(pfn_to_page((tree_lcd_item->pfn >> 9) << 9) +
				 (tree_lcd_item->pfn & 511));
		}

		if (!tree_page)
			return 0;

		ret = memcmp_page(page, tree_page, pgs);
		//printk("tree_pfn %#lx, pfn %#lx, ret %d\n", tree_lcd_item->pfn, lcd_item->pfn, ret);

		parent = *new;
		if (ret < 0) {
			new = &parent->rb_left;
		} else if (ret > 0) {
			new = &parent->rb_right;
		} else {
			spage_ksm_cnt++;
			return 1;
		}
	}

	rb_link_node(&lcd_item->node, parent, new);
	rb_insert_color(&lcd_item->node, root);

	spage_rbnode_tot++;

	return 0;
}

int lcd_ksm_tree_hpage_search_insert(struct lcd_ksm_item *lcd_item,
				     struct page *page)
{
	struct rb_node **new;
	struct rb_root *root;
	struct rb_node *parent = NULL;
	int pgs;

	pgs = HPAGE_SIZE;

	tot_hpages++;
	root = lcd_ksm_tree_root_huge;
	new = &root->rb_node;

	while (*new) {
		struct lcd_ksm_item *tree_lcd_item;
		struct page *tree_page;
		int ret;

		tree_lcd_item = rb_entry(*new, struct lcd_ksm_item, node);
		tree_page = pfn_to_page(tree_lcd_item->pfn);

		if (!tree_page)
			return 0;

		ret = memcmp_page(page, tree_page, pgs);

		parent = *new;
		if (ret < 0) {
			new = &parent->rb_left;
		} else if (ret > 0) {
			new = &parent->rb_right;
		} else {
			hpage_ksm_cnt++;
			return 1;
		}
	}

	rb_link_node(&lcd_item->node, parent, new);
	rb_insert_color(&lcd_item->node, root);

	hpage_rbnode_tot++;

	return 0;
}

int ksm_ingens(void *data)
{
	struct vm_context *vmc;
	int i;

	printk("ksm ingens split!!!!!!\n");
	printk("ksm init.\n");
	ksm_init();
	printk("ksm init end.\n");

	// tag pages
	for (i = 0; i < VM_NUM; ++i) {
		vmc = get_vmc_id(i);
		if (vmc == NULL)
			continue;
		vmc->is_open_split = 0;
		printk("monitor %d\n", i);
		//ksm_split(vmc);
		kthread_run(ksm_split, (void *)vmc, "ksm_eptscan%d", i);
	}
	msleep(20000);
	//kthread_stop();

	linux_ksm_flag = 1;
	linux_ksm_split_list_root = NULL;
	for (i = 0; i < VM_NUM; ++i) {
		vmc = get_vmc_id(i);
		if (vmc == NULL)
			continue;

		split_from_cold = 0;
		split_from_half = 0;
		vmc->rpart_num = 0;
		printk("ksm begin %d.\n", i);

		vmc->l_hpage_level = 2;
		printk("ksm huge add.\n");
		add_to_rbtree(vmc);
		vmc->l_hpage_level = 1;
		printk("ksm small add.\n");
		add_to_rbtree(vmc);

		printk("small page same %d %d MB, small rbnodes %d %d MB\n",
		       spage_ksm_cnt, spage_ksm_cnt / 256, spage_rbnode_tot,
		       spage_rbnode_tot / 256);
		printk("huge page same %d %d MB, huge rbnodes %d %d MB\n",
		       hpage_ksm_cnt, hpage_ksm_cnt * 2, hpage_rbnode_tot,
		       hpage_rbnode_tot * 2);

		printk("small page total %d %d MB, huge page total %d %d MB\n",
		       tot_spages, tot_spages / 256, tot_hpages,
		       tot_hpages * 2);
		printk("rpart num : %d, %d MB\n", vmc->rpart_num,
		       vmc->rpart_num * 2);
		printk("split from cold %d, half %d\n", split_from_cold,
		       split_from_half);
		ksm_list_split(vmc);
		printk("==============================\n");
	}

	ksm_end();

	printk("ksm end.\n");

	return 0;
}

// tag hot, cold, hotbloat pages, split cold and hotbloat pages.
int ksm_ours(void *data)
{
	struct vm_context *vmc;
	int i;

	printk("ksm ours split!!!!!!\n");
	printk("ksm init.\n");
	ksm_init();
	printk("ksm init end.\n");

	for (i = 0; i < VM_NUM; ++i) {
		vmc = get_vmc_id(i);
		if (vmc == NULL)
			continue;
		vmc->is_open_split = 1;
		kthread_run(ksm_split, (void *)vmc, "ksm_eptscan%d", i);
	}
	msleep(20000);

	linux_ksm_flag = 1;
	linux_ksm_split_list_root = NULL;
	for (i = 0; i < VM_NUM; ++i) {
		vmc = get_vmc_id(i);
		if (vmc == NULL)
			continue;

		split_from_cold = 0;
		split_from_half = 0;
		vmc->rpart_num = 0;
		printk("ksm begin %d.\n", i);

		vmc->l_hpage_level = 2;
		printk("ksm huge add.\n");
		add_to_rbtree(vmc);
		vmc->l_hpage_level = 1;
		printk("ksm small add.\n");
		add_to_rbtree(vmc);

		printk("small page same %d %d MB, small rbnodes %d %d MB\n",
		       spage_ksm_cnt, spage_ksm_cnt / 256, spage_rbnode_tot,
		       spage_rbnode_tot / 256);
		printk("huge page same %d %d MB, huge rbnodes %d %d MB\n",
		       hpage_ksm_cnt, hpage_ksm_cnt * 2, hpage_rbnode_tot,
		       hpage_rbnode_tot * 2);

		printk("small page total %d %d MB, huge page total %d %d MB\n",
		       tot_spages, tot_spages / 256, tot_hpages,
		       tot_hpages * 2);
		printk("rpart num : %d, %d MB\n", vmc->rpart_num,
		       vmc->rpart_num * 2);
		printk("split from cold %d, half %d\n", split_from_cold,
		       split_from_half);
		ksm_list_split(vmc);
		printk("==============================\n");
	}

	ksm_end();

	printk("ksm end.\n");

	return 0;
}

void __rmap_check_zero(struct kvm_rmap_head *rmap_head, gfn_t gfn,
		       struct vm_context *vmc)
{
	u64 *sptep;
	u64 pfn;
	struct page *pp;
	void *kaddr;
	int tmp;
	int i;
	int nr_zero_page;
	struct linux_ksm_split_list *sl;
	//hpa_t shadow_addr;

	if (!rmap_head->val)
		sptep = NULL;
	else if (!(rmap_head->val & 1)) {
		sptep = (u64 *)rmap_head->val;
	} else {
		sptep = NULL;
		printk("desc loss\n");
	}
	if (sptep) {
		if (!((*sptep) & (1ull << 7)))
			return;

		pfn = *sptep & (((1ULL << 52) - 1) & ~(u64)(PAGE_SIZE - 1));
		pfn = pfn >> 12;
		if (vmc->l_hpage_level > 1) {
			pfn = (pfn >> 9) << 9;
		}
		if (pfn) {
			pp = pfn_to_page(pfn);
			nr_zero_page = 0;

			for (i = 0; i < 512; ++i) {
				kaddr = kmap_atomic(pp + i);

				if (!memchr_inv(kaddr, 0, PAGE_SIZE)) {
					nr_zero_page++;
					//hash_find_pfn(gfn+i);
				}

				kunmap_atomic(kaddr);

				if (vmc->l_hpage_level == 1)
					break;
			}

			spage_ksm_cnt += nr_zero_page;

			if (vmc->l_hpage_level == 2 && nr_zero_page) {
				sl = (struct linux_ksm_split_list *)kmalloc(
					sizeof(struct linux_ksm_split_list),
					GFP_KERNEL);
				sl->gfn = gfn;
				sl->pfn = pfn;
				sl->spte = (*sptep);
				sl->nxt = NULL;
				if (linux_ksm_split_list_root != NULL) {
					sl->nxt = linux_ksm_split_list_root;
				}
				linux_ksm_split_list_root = sl;
				vmc->rpart_num++;
			}
		}
	}
}

void ept_zero_scan(struct vm_context *vmc)
{
	//struct kvm_vcpu* vcpu;
	struct kvm_memslots *memslots;
	int i;
	int used_slots;
	struct kvm_memory_slot *slot;

	spin_lock(&(vmc->kvm->mmu_lock));
	//shadow_sp_enable = 1;

	for (i = 0; i < KVM_ADDRESS_SPACE_NUM; ++i) {
		memslots = vmc->kvm->memslots[i];
		for (used_slots = memslots->used_slots - 1; used_slots >= 0;
		     used_slots--) {
			//kvm_mmu_slot_leaf_clear_dirty(kvm_global,id_to_memslot(memslots, used_slots));
			slot = id_to_memslot(memslots, used_slots);
			//spin_lock(&kvm_global->mmu_lock);
			slot_leaf_handle(slot, __rmap_check_zero, vmc);
			//spin_unlock(&kvm_global->mmu_lock);
			//kvm_flush_remote_tlbs_with_slot(slot->base_gfn, slot->npages);
		}
	}

	spin_unlock(&(vmc->kvm->mmu_lock));
}

int ksm_zero(void *data)
{
	struct vm_context *vmc;
	int i;
	printk("ksm zero split!!!!!!\n");

	ksm_init();

	for (i = 0; i < VM_NUM; ++i) {
		vmc = get_vmc_id(i);
		if (vmc == NULL)
			continue;

		vmc->rpart_num = 0;
		printk("ksm begin %d.\n", i);

		vmc->l_hpage_level = 2;
		printk("ksm huge add.\n");
		ept_zero_scan(vmc);
		vmc->l_hpage_level = 1;
		printk("ksm small add.\n");
		ept_zero_scan(vmc);

		printk("small page same %d %d MB, small rbnodes %d %d MB\n",
		       spage_ksm_cnt, spage_ksm_cnt / 256, spage_rbnode_tot,
		       spage_rbnode_tot / 256);
		printk("huge page same %d %d MB, huge rbnodes %d %d MB\n",
		       hpage_ksm_cnt, hpage_ksm_cnt * 2, hpage_rbnode_tot,
		       hpage_rbnode_tot * 2);

		printk("small page total %d %d MB, huge page total %d %d MB\n",
		       tot_spages, tot_spages / 256, tot_hpages,
		       tot_hpages * 2);
		printk("rpart num : %d, %d MB\n", vmc->rpart_num,
		       vmc->rpart_num * 2);
		printk("split from cold %d, half %d\n", split_from_cold,
		       split_from_half);
		ksm_list_split(vmc);
		printk("==============================\n");
	}

	printk("ksm end.\n");
	return 0;
}
