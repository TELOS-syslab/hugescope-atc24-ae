#/bin/bash
#numactl --preferred=0 ./XSBench -s XL -p 20000000 -t 16 >>result.txt
#numactl --preferred=0 ./XSBench -s XL -p 50000000 -t 16 >>result.txt

numactl --preferred=0 ./pr -g 27 -n 8 >>result.txt
numactl --preferred=0 ./bc -g 27 -n 8 >>result.txt

#numactl --preferred=0 ./pr -g 28 -n 8 >>result.txt
#numactl --preferred=0 ./bc -g 28 -n 8 >>result.txt

#numactl --preferred=0  ./pr -g 29 -n 8 >>result.txt
#numactl --preferred=0  ./bc -g 29 -n 8 >>result.txt



