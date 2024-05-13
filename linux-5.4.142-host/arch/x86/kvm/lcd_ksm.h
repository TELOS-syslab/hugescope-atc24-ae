#include <linux/rbtree.h>

// 注意node 的清零
struct lcd_ksm_item{
    int level;
    u64 gfn;
    u64 pfn;
    struct lcd_ksm_item* nxt;
    struct rb_node node;
};
struct lcd_ksm_item* item_root;

struct linux_ksm_split_list{
    u64 gfn;
    u64 pfn;
    u64 spte;
    struct linux_ksm_split_list* nxt;
};
struct linux_ksm_split_list* linux_ksm_split_list_root;

struct rb_root one_ksm_tree_spage[1] = { RB_ROOT };
struct rb_root one_ksm_tree_hpage[1] = { RB_ROOT };

struct rb_root* lcd_ksm_tree_root = one_ksm_tree_spage;
struct rb_root* lcd_ksm_tree_root_huge = one_ksm_tree_hpage;

int spage_ksm_cnt;
int hpage_ksm_cnt;

int spage_rbnode_tot;
int hpage_rbnode_tot;

int tot_spages;
int tot_hpages;

int linux_ksm_flag;

int ksm_huge_scanner(void *data);
int memcmp_page(struct page *page1, struct page *page2, int pgs);
int lcd_ksm_tree_spage_search_insert(struct lcd_ksm_item* lcd_item,
										struct page *page);
int lcd_ksm_tree_hpage_search_insert(struct lcd_ksm_item* lcd_item,
										struct page *page);
void ksm_init(void);

int ksm_split(void *data);
int ksm_linux_split(void *data);

int ksm_ours(void *data);
int ksm_ingens(void *data);
int ksm_zero(void *data);

int memcmp_page(struct page *page1, struct page *page2, int pgs);