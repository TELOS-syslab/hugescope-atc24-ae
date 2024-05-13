#include <linux/kvm_host.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/timekeeping.h>
#include <linux/pagemap.h>
#include <linux/khugepaged.h>

#define KVM_HC_HANDLE_PT_BUFFER 12

#define BUFFER_ORDER 10
#define BUFFER_SIZE 512 * (1 << (BUFFER_ORDER - 1))

#define GPTP_IDX_BUF_SIZE 100000
#define PM_NUM (1 << 23)
#define Q_LEVEL 7
#define Q_SWITCH 1
#define SAMPLE_PERIOD 600 // sleeping interval (ms)
#define TRACE_LEN 10 // scan window
#define SVM_TRACE_LEN 60
#define SVM_DEFAULT_THREADHOLD 60

#define DRAM_NODE 0
#define NVM_NODE 3
#define DRAM 16
#define NVM 104	

#define DRAM_THP_NUM DRAM * 512
#define DEFAULT_DRAM_THP_NUM 12 * 512

#ifdef HMM_THP_FLAG
#define mb_shift(x) (x) * 2
#define DEFUALT_DRAM_POOL_SIZE 0 * 512
#else
#define mb_shift(x) (x) / 256
#define DEFUALT_DRAM_POOL_SIZE 10 * 1024 * 1024 / 4
#endif

#define DRAM_PAGE_NUM DRAM * 1024 * 1024 / 4
#define NVM_PAGE_NUM NVM * 1024 * 1024 / 4
#define MEM_SIZE (DRAM + NVM) * 3 / 2
#define PAGE_NUM MEM_SIZE * 1024 * 1024 / 4

#define PFNHASHMOD 9999997
typedef unsigned long ul;

#define HUGE_CRITICAL 1


#define SP_INTERVAL 3000 
#define GFNTOSPNUM 19999
#define HUGE_PAGE_MN ((DRAM + NVM) * 512)
#define SP_HOT_THRESHOLD 6
#define SP_SPLITE_THRESHOLD 70
#define SP_COLLAPSE_THRESHOLD 450
#define SP_COLLAPSE_THRESHOLD_LOWER 256

#define SP_SPLITE_UPPER 250
#define SP_SPLITE_LOWER 10

struct ept_instead {
	u64 *sptep;
	u64 old;
	u64 gfn;
	int cc;
	int newid;
	char count[512];
	struct ept_instead *nxt;
};

struct gfn_to_eptsp {
	u64 gfn;
	bool is_hot;
	struct ept_instead *sp;
	struct gfn_to_eptsp *nxt;
};

struct split_page_struct {
	u64 gfn;
	u64 pfn;
	u64 old;
	int cnt;
	struct split_page_struct *nxt;
};

//mem tracking: gptp
struct gptp_buf_entry {
	bool p;
	ul pfn;
	struct page *page;
	int pml; //标识是否在pml中出现
};

//page migration
struct page_node {
	bool p; // valid bit
	bool f; // free bit
	bool sp; //split in this phase
	bool sp_h; // split and hot pages
	bool sp_collapse_tag; 
	char level;
	ul pfn;
	ul gfn;
	ul new_pfn;
	ul spte;
	int target_node;
	int current_node;
	int actual_access;
	short acc_map[SVM_TRACE_LEN]; //for each entry: 0 is not access; 1 is read, 2 is write
	short acc_tb;
	short acc_type;
	int rc;
	bool finish;
	struct anon_vma *anon_vma;
};

struct free_mem_pool_node {
	ul pfn;
	struct free_mem_pool_node *nxt;
};

struct hash_node {
	u64 key;
	u64 val;
	struct hash_node *nxt;
	u64 change_time;
	u8 level;
	u8 page_level;
};

typedef struct problem_parameter {
	long **data_vector;
	long *label;
	int dimension;
	int size;
	long C;
	long tolerance;
	long *w;
	long *b;

	long *Erroi;
	long *alpha;

	int threadhold;
} problem_parameter;

struct vm_context {
	int id;
	bool tracking;
	ul dram_pages;
	ul nvm_pages;
	ul page_num;
	struct kvm *kvm;

	//mem tracking
	int q;
	bool enable_flag;
	bool enable_flag_ept_scanner;
	int sample_period;
	int monitor_timer;
	int time_counter; 
	struct hash_node **hash_table;
	struct gptp_buf_entry *gptp_buf;
	ul *gptp_idx_buf;
	int gptp_num;
	ul *free_page_idx_buf;
	int free_page_num;

	//hot_or_cold
	struct page_node *busy_pages; 
	int busy_page_num;
	long *buckets;
	int bucket_num;
	problem_parameter pp;
	int pre_pool_page_num;
	int pool_page_num;

	//page migration
	bool *pm_pml_buffer;
	bool migration_flag;
	bool use_free;
	struct page_node *pm_dram_pages;
	struct page_node *pm_nvm_pages;
	ul dram_page_index;
	ul nvm_page_index;
	ul dram_page_index_pages;
	ul nvm_page_index_pages;

	atomic_t v;

	//others
	ul start_time;
	ul end_time;

	//huge page
	int hpage_num;
	int spage_num;

	int l_hpage_level;

	bool shadow_sp_enable;
	struct ept_instead *ept_ins_list_head;
	struct gfn_to_eptsp **gfn2sp;
	void **ept_page_cache;
	u64 tdp1_c;
	u64 tdp2_c;
	int ept_pc_num;
	int ept_pc_num_max;
	int hs_count;

	int tb_map[TRACE_LEN + 5];
	int tbs_map[TRACE_LEN + 5];
	int ls_monitor;
	int ls_split;
	int ls_tb;
	int huge_split_th;
	int huge_collapse_th;
	int split_limit_num;
	int control_num;
	int is_open_split;

	int ccc[15];
	int rpart_num;
	struct split_page_struct *split_lhead;

	int sp_collapse_nvm_num;
};

struct page_list {
	int id;
	int size;
	struct page_node *pages;
	struct vm_context *vmc;
};

struct pnode {
	ul gfn;
	int w;
	char level;
	struct pnode *next;
};

struct vm_context *get_vmc(struct kvm *);
void init_vm_context(struct kvm *kvm);
void destroy_vm_context(struct kvm *kvm);
struct page *get_pool_page(void);
void pre_insert_free_pool(struct vm_context *vmc, ul pfn);
void insert_pre_pool(struct vm_context *vmc);
void insert_pool_page(ul pfn);
void init_free_pool(void);
void put_free_pool(void);
void insert_pool_pages(struct vm_context *vmc);
/*gpt sacnner****************************/
void init_hash_table(struct vm_context *vmc);
void init_gptp_buffer(struct vm_context *vmc);
void init_gptp_idx_buffer(struct vm_context *vmc);
void init_free_page_idx_buffer(struct vm_context *vmc);
void open_mem_tracker(struct kvm_vcpu *vcpu, ul addr, ul max_index);
void close_mem_tracker(struct kvm_vcpu *);
inline int flush_ept_dirty_bit_by_gfn(struct vm_context *vmc, ul gfn);
void handle_pml_log_by_ss(struct kvm_vcpu *vcpu, ul gpa);
void flush_all_tlbs(struct kvm *);
void show_slots(void);
/*****************************************/

void init_busy_pages(struct vm_context *vmc);

int my_cmp(const void *a, const void *b);
void build_buckets(struct vm_context *vmc, int watch);
int get_bucket_num(struct vm_context *vmc, long w);
long cal_value(struct vm_context *vmc, struct page_node node);
void pre_migration(struct vm_context *vmc);
ul pm_gfn_to_pm_page(struct kvm *kvm, gfn_t gfn, struct page_node *);
ul pm_gfn_to_pfn(struct kvm *kvm, gfn_t gfn);
ul pm_gfn_to_nid(struct kvm *kvm, ul gfn);

/*********************************************/
void init_svm(problem_parameter *pp);
long learned_fun(long *w, long b, long *x, int);
int try_argmaxE1_E2(int i1, long E1, problem_parameter *prob_para);
int try_iter_non_boundary(int i1, problem_parameter *prob_para);
int try_iter_all(int i1, problem_parameter *prob_para);
int takeStep(int i1, int i2, problem_parameter *prob_para);
void *my_malloc(long t);
void svm_train_data(problem_parameter *, int, struct page_node *pm_pages,
		    int nums, int watch);
void svm_train(problem_parameter *prob_para);
int svm_test(problem_parameter *prob_para, struct page_node *pm_pages, int nums,
	     int watch, int *acc, int *tot);
bool accuracy_checker(struct vm_context *);
/****************************************/
void init_pm_pages(struct vm_context *vmc);
void init_page_migration(struct vm_context *vmc);
void close_page_migration(struct vm_context *vmc);
void get_pml_log_for_page_migration(struct vm_context *, ul gpa);
void check_pml_for_dirty_page(ul, struct page *, struct page *);
void begin_migration(struct vm_context *vmc);
void __begin_migration(struct vm_context *);
void print_migration_log(struct page_node *pages, int size);
void fill_epte(struct vm_context *, struct page_node page);
void unmap_and_mapping(struct page_node *pages, int size);
void gfn_to_pagenode(struct page_node *node, ul gfn);
int pm(void *data);
void migration(struct vm_context *vmc);

void insert_pm_page(struct vm_context *vmc, struct page_node page,
		    int tar_node);
void get_gfns(struct kvm_vcpu *vcpu, ul buf_addr, ul max_index);
//void migrate_sys_pages(void);

int add_pm_free_pages(void);
void del_pm_free_pages(void);
struct page *get_free_page_from_target_cache(int thread);

bool hash_find_gfn(struct vm_context *vmc, u64 pfn, u8 pl);
void hash_recycle(struct vm_context *);

void get_free_pages(struct vm_context *);
void vm_is_free(struct vm_context *vmc);

/*************************************************/
//thp
void open_mem_tracker_ept_scanner(struct kvm_vcpu *, int);
void pre_migration_for_single_vm(struct vm_context *vmc);
void pre_migration_thp_for_mutil_vm(struct vm_context *vmc);

void pre_migration_fix_threshold(struct vm_context *vmc);
void pre_migration_for_single_vm_huge_and_small(struct vm_context *vmc);

// 混合页面

void gfn_to_eptsp_init(struct vm_context *vmc);
void gfn_to_eptsp_tag(struct vm_context *vmc, u64 gfn);
void gfn_to_eptsp_insert(struct vm_context *vmc, u64 gfn,
			 struct ept_instead *ei);
struct gfn_to_eptsp *gfn_to_eptsp_find(struct vm_context *vmc, u64 gfn);
void gfn_to_eptsp_destory(struct vm_context *vmc);

void tdp_sp_int(struct ept_instead *ei);
void tdp_sp_rec(struct vm_context *vmc, struct ept_instead *ei);

void init_sp_all(struct vm_context *vmc);

void split_linsert(struct vm_context *vmc, u64 gfn, u64 pfn, u64 old);
void pg_cache_alloc(struct vm_context *vmc);

long cal_value_gfn(struct vm_context *vmc, u64 gfn);

struct vm_context *get_vmc_id(int i);