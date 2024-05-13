#include <linux/kvm_host.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>

#define GFNTOSPNUM 19999

#define HUGE_PAGE_MN 20000       //影子小页的最大数量

#define HSAMPLE_PERIOD 600      //大页的采样间隔
#define HTRACE_LEN 10 //采样次数

#define SSAMPLE_PERIOD 500      //小页的采样间隔
#define STRACE_LEN 5            //小页的采样次数

#define HOT_THRESHOLD 6         //大页的热页阈值

struct ept_instead{
        u64* sptep;
        u64 old;
        u64 gfn;
        int newid;
        char count[512];
        struct ept_instead* nxt;
};


struct gfn_to_eptsp{
        u64 gfn;
        bool is_hot;
        struct ept_instead* sp;
        struct gfn_to_eptsp* nxt;
};

void gfn_to_eptsp_init(void);
void gfn_to_eptsp_tag(u64 gfn);
void gfn_to_eptsp_insert(u64 gfn, struct ept_instead* ei);
struct gfn_to_eptsp* gfn_to_eptsp_find(u64 gfn);
void gfn_to_eptsp_destory(void);

void tdp_sp_int(struct ept_instead* ei);
void tdp_sp_rec(struct ept_instead* ei);

/*old lcd_work.c*/
//#include "lcd_work.h"

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


void gfn_to_eptsp_init(void){
    int i;
    gfn2sp = (struct gfn_to_eptsp **)vmalloc_node(
		sizeof(struct gfn_to_eptsp *) * (GFNTOSPNUM + 3), 0);
    for(i=0;i<GFNTOSPNUM;++i){
        gfn2sp[i]=NULL;
    }    
}
EXPORT_SYMBOL(gfn_to_eptsp_init);

void gfn_to_eptsp_construct(struct gfn_to_eptsp* node, u64 gfn, struct ept_instead* e, bool is_hot){
    node->gfn=gfn;
    node->sp=e;
    node->nxt=NULL;
    node->is_hot=is_hot;
}


void gfn_to_eptsp_tag(u64 gfn){
    int id;
    struct gfn_to_eptsp* node;

    id = gfn%GFNTOSPNUM;
    node = (struct gfn_to_eptsp*)kmalloc(sizeof(struct gfn_to_eptsp),GFP_KERNEL);
    gfn_to_eptsp_construct(node,gfn,NULL,true);

    if(gfn2sp[id]==NULL){
        gfn2sp[id] = node;
    }
    else{
        node->nxt=gfn2sp[id];
        gfn2sp[id]=node;
    }
}
EXPORT_SYMBOL(gfn_to_eptsp_tag);

void gfn_to_eptsp_insert(u64 gfn, struct ept_instead* ei){
    int id;
    struct gfn_to_eptsp* node;

    id = gfn%GFNTOSPNUM;

    node = gfn2sp[id];
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

        if(gfn2sp[id]==NULL){
            gfn2sp[id] = node;
        }
        else{
            node->nxt=gfn2sp[id];
            gfn2sp[id]=node;
        }
    }
}
EXPORT_SYMBOL(gfn_to_eptsp_insert);

struct gfn_to_eptsp* gfn_to_eptsp_find(u64 gfn){
    int id;
    struct gfn_to_eptsp* p;

    id = gfn%GFNTOSPNUM;
    if(gfn2sp[id]==NULL)return NULL;
    else{
        p = gfn2sp[id];
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


void gfn_to_eptsp_destory(void){
    int i;
    struct gfn_to_eptsp* p;
    struct gfn_to_eptsp* pp;
    for(i=0;i<GFNTOSPNUM;++i){
        if(gfn2sp[i]!=NULL){
            p = gfn2sp[i];
            while(p!=NULL){
                pp = p;
                p = p->nxt;
                kfree(pp);  
            }
            gfn2sp[i]=NULL;
        }
    }
    if(gfn2sp)vfree(gfn2sp);
}
EXPORT_SYMBOL(gfn_to_eptsp_destory);


void tdp_sp_int(struct ept_instead* ei){
    *(ei->sptep) = ei->old;
}
EXPORT_SYMBOL(tdp_sp_int);


void tdp_sp_rec(struct ept_instead* ei){
    int i;
    u64* pt;

    ei->old=*(ei->sptep);
    if(~(ei->old & ((1ull << 7)))){
        printk("thp changed!\n");
        // 如果这个页已经变成了页表页，那么影子小页应该失效，old = 0作为标记
        ei->old = 0;
        return;
    }
    
    *(ei->sptep) = __pa(ept_page_cache[ei->newid]) | (ei->old&0x507);
    pt = (u64*)ept_page_cache[ei->newid];

    for(i=0;i<512;++i){
            pt[i]=(ei->old & ((1ull<<52)-1));
            pt[i]+=(i<<12);
            pt[i]&=(~((1ull << 9) + (1ull << 8) + (1ull << 7)));
    }
}
EXPORT_SYMBOL(tdp_sp_rec);

// 需要 mm_struct 以及对应合并的 hva
void collapse_pages(void){
        struct split_page_struct* p;
        struct page *hpage = NULL;
	struct mm_struct *mm = kvm_global->mm;
        u64 hva;
        int success=0;

        p = split_lhead;
        while(p){
                hpage = NULL;
                hva = gfn_to_hva(kvm_global, p->gfn);

                if (unlikely(!down_read_trylock(&(mm->mmap_sem))))
		        goto out;

                collapse_huge_page(mm,hva,&hpage,0,HPAGE_PMD_NR);
                
                if (!IS_ERR_OR_NULL(hpage))
		        put_page(hpage);

                success++;
                //if(success) break;
                //up_read(&(kvm_global->mm->mmap_sem));
out:
                p=p->nxt;
        }
        printk("success collapse: %d\n", success);
}