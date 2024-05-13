echo always> /sys/kernel/mm/transparent_hugepage/defrag
echo always> /sys/kernel/mm/transparent_hugepage/enabled

echo N > /sys/module/kvm/parameters/nx_huge_pages