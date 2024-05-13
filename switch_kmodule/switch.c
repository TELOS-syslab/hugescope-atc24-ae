#include<linux/module.h>
#include<linux/init.h>
#include<linux/moduleparam.h>

MODULE_AUTHOR("HugeScope");
MODULE_LICENSE("GPL");


static int test_selector = 0;
module_param(test_selector, int, S_IRUGO);

extern void start_hugescope_test(int selector);

static int switch_init(void){
        start_hugescope_test(test_selector);
        return 0;
}
static void switch_exit(void){
        printk("module exit\n");
}
module_init(switch_init);
module_exit(switch_exit);
