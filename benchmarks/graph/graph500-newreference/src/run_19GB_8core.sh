time -p numactl --preferred=0 taskset -c 16-23 mpirun --allow-run-as-root -np 8 ./graph500_reference_bfs_sssp 25
