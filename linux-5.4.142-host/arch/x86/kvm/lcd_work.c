//#include "lcd_work.h"
#include "ss_work.h"
/*
bool shadow_sp_enable;
EXPORT_SYMBOL(shadow_sp_enable);

struct ept_instead* ept_ins_list_head;
EXPORT_SYMBOL(ept_ins_list_head);

struct gfn_to_eptsp** gfn2sp;
EXPORT_SYMBOL(gfn2sp);

void** ept_page_cache;
EXPORT_SYMBOL(ept_page_cache);

u64 tdp1_c;
EXPORT_SYMBOL(tdp1_c);

u64 tdp2_c;
EXPORT_SYMBOL(tdp2_c);
*/

void gfn_to_eptsp_init(struct vm_context *vmc){
    int i;

    vmc->gfn2sp = (struct gfn_to_eptsp **)vmalloc_node(
		sizeof(struct gfn_to_eptsp *) * (GFNTOSPNUM + 3), 0);
    for(i=0;i<GFNTOSPNUM;++i){
        vmc->gfn2sp[i]=NULL;
    }    
}
EXPORT_SYMBOL(gfn_to_eptsp_init);

void gfn_to_eptsp_construct(struct gfn_to_eptsp* node, u64 gfn, struct ept_instead* e, bool is_hot){
    node->gfn=gfn;
    node->sp=e;
    node->nxt=NULL;
    node->is_hot=is_hot;
}


void gfn_to_eptsp_tag(struct vm_context *vmc, u64 gfn){
    int id;
    struct gfn_to_eptsp* node;

    id = gfn%GFNTOSPNUM;
    node = (struct gfn_to_eptsp*)kmalloc(sizeof(struct gfn_to_eptsp),GFP_KERNEL);
    gfn_to_eptsp_construct(node,gfn,NULL,true);

    if(vmc->gfn2sp[id]==NULL){
        vmc->gfn2sp[id] = node;
    }
    else{
        node->nxt=vmc->gfn2sp[id];
        vmc->gfn2sp[id]=node;
    }
}
EXPORT_SYMBOL(gfn_to_eptsp_tag);

void gfn_to_eptsp_insert(struct vm_context *vmc, u64 gfn, struct ept_instead* ei){
    int id;
    struct gfn_to_eptsp* node;

    id = gfn%GFNTOSPNUM;

    node = vmc->gfn2sp[id];
    while(node!=NULL){
        if(node->gfn==gfn){
            node->sp=ei;
            break;
        }
        node=node->nxt;
    }
    if(node==NULL){
        //printk("only open sp can reach.\n");
        node = (struct gfn_to_eptsp*)kmalloc(sizeof(struct gfn_to_eptsp),GFP_KERNEL);
        gfn_to_eptsp_construct(node,gfn,ei,true);

        if(vmc->gfn2sp[id]==NULL){
            vmc->gfn2sp[id] = node;
        }
        else{
            node->nxt=vmc->gfn2sp[id];
            vmc->gfn2sp[id]=node;
        }
    }
}
EXPORT_SYMBOL(gfn_to_eptsp_insert);

struct gfn_to_eptsp* gfn_to_eptsp_find(struct vm_context *vmc, u64 gfn){
    int id;
    struct gfn_to_eptsp* p;

    id = gfn%GFNTOSPNUM;
    if(vmc->gfn2sp[id]==NULL)return NULL;
    else{
        p = vmc->gfn2sp[id];
        while(p!=NULL){
            if(p->gfn==gfn){
                return p;
            }
            p=p->nxt;
        }
    }
    return NULL;
}
EXPORT_SYMBOL(gfn_to_eptsp_find);


void gfn_to_eptsp_destory(struct vm_context *vmc){
    int i;
    struct gfn_to_eptsp* p;
    struct gfn_to_eptsp* pp;
    for(i=0;i<GFNTOSPNUM;++i){
        if(vmc->gfn2sp[i]!=NULL){
            p = vmc->gfn2sp[i];
            while(p!=NULL){
                pp = p;
                p = p->nxt;
                kfree(pp);  
            }
            vmc->gfn2sp[i]=NULL;
        }
    }
    if(vmc->gfn2sp)vfree(vmc->gfn2sp);
}
EXPORT_SYMBOL(gfn_to_eptsp_destory);


void tdp_sp_int(struct ept_instead* ei){
    *(ei->sptep) = ei->old;
}
EXPORT_SYMBOL(tdp_sp_int);


void tdp_sp_rec(struct vm_context *vmc, struct ept_instead* ei){
    int i;
    u64* pt;

    ei->old=*(ei->sptep);
    if(~(ei->old & ((1ull << 7)))){
        printk("thp changed!\n");
        // 如果这个页已经变成了页表页，那么影子小页应该失效，old = 0作为标记
        ei->old = 0;
        return;
    }
    
    *(ei->sptep) = __pa(vmc->ept_page_cache[ei->newid]) | (ei->old&0x507);
    pt = (u64*)(vmc->ept_page_cache[ei->newid]);

    for(i=0;i<512;++i){
            pt[i]=(ei->old & ((1ull<<52)-1));
            pt[i]+=(i<<12);
            pt[i]&=(~((1ull << 9) + (1ull << 8) + (1ull << 7)));
    }
}
EXPORT_SYMBOL(tdp_sp_rec);

void pg_cache_alloc(struct vm_context *vmc){
    void *page;
    int i;
    //vmc->ept_pc_num=0;
    vmc->ept_page_cache = (void **)kmalloc(sizeof(void*) * HUGE_PAGE_MN,GFP_KERNEL);

    for(i=0;i<HUGE_PAGE_MN;++i){
            page = (void*)__get_free_page(GFP_KERNEL);
            if(!page)break;
            memset((page), 0, PAGE_SIZE);
            vmc->ept_page_cache[i] = page;
    }
    vmc->ept_pc_num_max = i;
    printk("pg_cache_alloc: num: %d\n", vmc->ept_pc_num_max);
}
EXPORT_SYMBOL(pg_cache_alloc);

void init_sp_all(struct vm_context *vmc){
    vmc->ept_ins_list_head=NULL;
    vmc->ept_pc_num = 0;
    vmc->hs_count=0;
    vmc->tdp1_c=0;
    vmc->tdp2_c=0;
    //pg_cache_alloc(vmc);  放到最前面去，避免每次都重新alloc page
    gfn_to_eptsp_init(vmc);
}
EXPORT_SYMBOL(init_sp_all);

void split_linsert(struct vm_context *vmc, u64 gfn, u64 pfn, u64 old){
        struct split_page_struct* p;
        p = (struct split_page_struct*)kmalloc(sizeof(struct split_page_struct),GFP_KERNEL);
        p->gfn=gfn;
        p->pfn=pfn;
        p->old=old;
        p->cnt=0;
        p->nxt=NULL;
        if(vmc->split_lhead!=NULL){
                p->nxt=vmc->split_lhead;
        }
        vmc->split_lhead=p;
}
EXPORT_SYMBOL(split_linsert);


long cal_value_gfn(struct vm_context *vmc, u64 gfn)
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

	if(vmc->busy_pages[gfn].sp){
		if(vmc->busy_pages[gfn].sp_h){
			//如果是大页拆分的热小页，直接继承原来大页的acc map
			//node = vmc->busy_pages[(node.gfn>>9)<<9];
            gfn = (gfn>>9)<<9;
		}
		else{
			//如果是从大页拆分出来的冷小页，感觉直接返回0有点极端了，当作读了一次
			return 0;  
		}
	}

	for (i = 0; i < TRACE_LEN; i++) {
		switch (vmc->busy_pages[gfn].acc_map[i]) {
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
EXPORT_SYMBOL(cal_value_gfn);




