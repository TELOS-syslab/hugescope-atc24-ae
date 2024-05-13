#ifndef __PFN_HASH_H__
#define __PFN_HASH_H__

#include <asm/uaccess.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>

#define PFNHASHMOD 9999997

struct file *fp;
loff_t pos;
mm_segment_t fs;

int hash_check(u64 x)
{
	int ret = 0;
	ret = (x) % PFNHASHMOD;
	return ret;
}

void hash_node_init(struct hash_node *t, u64 _key, u64 _val,
		    struct hash_node *_nxt, u64 _change_time, u8 _level, u8 _pagel)
{
	t->key = _key;
	t->val = _val;
	t->nxt = _nxt;
	t->change_time = _change_time;
	t->level = _level;
	t->page_level = _pagel;
}

bool hash_find_gfn(struct vm_context *vmc, u64 gfn, u8 pl)
{
	int h;
	struct hash_node *p;
	struct hash_node *fp;
	struct hash_node *node;
	u64 ct;
	int time_counter;

	time_counter = vmc->time_counter;

	h = hash_check(gfn);

	if (vmc->hash_table[h] == NULL) {
		node = (struct hash_node *)kmalloc(sizeof(struct hash_node),
						   GFP_KERNEL);
		hash_node_init(node, gfn, 1, NULL, time_counter, 0, pl);
		vmc->hash_table[h] = node;
		return true;
	}

	p = vmc->hash_table[h];

	while (p != NULL) {
		if (p->key == gfn) {
			p->val += 1;
			ct = p->change_time +
			     (p->level == 0 ? 0 : (1 << (p->level - 1)));
			if (!Q_SWITCH)
				return true;
			if (time_counter == ct)
				return true;
			else if (time_counter < ct)
				return false;
			else {
				if (time_counter == ct + 1) {
					p->level++;
					if (p->level > Q_LEVEL)
						p->level = Q_LEVEL;
					p->change_time = time_counter;
					return false;
				} else {
					if (p->level != 0)
						p->level--;
					p->change_time = time_counter;

					if (p->level == 0)
						return true;
					return false;
				}
			}
		}
		fp = p;
		p = p->nxt;
	}

	node = (struct hash_node *)kmalloc(sizeof(struct hash_node),
					   GFP_KERNEL);
	hash_node_init(node, gfn, 1, NULL, vmc->time_counter, 0, pl);
	fp->nxt = node;
	return true;
}

int write_trace_file_init(void)
{
	fp = filp_open("/root/trace.out", O_RDWR | O_CREAT, 0644);
	pos = 0;
	if (IS_ERR(fp)) {
		printk("create file error\n");
		return -1;
	}
	return 1;
}

int ul_to_str(u64 gfn, char *a, int hex)
{
	char c;
	int w;
	int tmp, i;

	w = 0;
	while (gfn) {
		tmp = gfn % hex;
		gfn = gfn / hex;
		if (tmp >= 10) {
			a[w] = 'a' + tmp - 10;
		} else {
			a[w] = '0' + tmp;
		}
		w++;
	}
	a[w] = '\n';

	for (i = 0; i < w / 2; i++) {
		c = a[i];
		a[i] = a[w - i - 1];
		a[w - i - 1] = c;
	}
	return w;
}

void write_trace_to_file(u64 gfn, int val)
{
	char *a;
	char *b;
	int a_size, b_size;
	a = (char *)kmalloc(sizeof(char) * 20, GFP_KERNEL);
	b = (char *)kmalloc(sizeof(char) * 20, GFP_KERNEL);

	a_size = ul_to_str(gfn, a, 16);
	a[a_size] = ' ';
	kernel_write(fp, a, a_size + 1, &pos);

	b_size = ul_to_str(val, b, 10);
	kernel_write(fp, b, b_size + 1, &pos);
	kfree(a);
	kfree(b);
	return;
}

void hash_recycle(struct vm_context *vmc)
{
	int i;
	struct hash_node *h;
	struct hash_node *ph;
	u64 sum_hgfn = 0;
	u64 sum_sgfn = 0;
	//write_trace_file_init();
	for (i = 0; i < PFNHASHMOD; i++) {
		if (vmc->hash_table[i] != NULL) {
			h = vmc->hash_table[i];
			while (h != NULL) {
				ph = h;
				//write_trace_to_file(h->key, h->val);
				h = h->nxt;
				if(ph->page_level==1)sum_sgfn++;
				else sum_hgfn++;
#ifdef HMM_HSPAGE_FLAG
				/*if(ph->page_level == 2 && ph->val>=1){
					//printk("tag success.\n");
					gfn_to_eptsp_tag(vmc, (ph->key)>>9);
				}*/
				if(ph->page_level == 2)vmc->tb_map[ph->val]++;
				else vmc->tbs_map[ph->val]++;
#endif
				kfree(ph);
			}
			vmc->hash_table[i] = NULL;
		}
	}
	printk("hash: the sum of huge gfns is %lld  %lld M, small gfns is %lld  %lld M\n",
		 sum_hgfn, sum_hgfn*2, sum_sgfn, sum_sgfn/256);
}
#endif
