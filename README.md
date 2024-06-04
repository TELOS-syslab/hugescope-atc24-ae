# Artifact Evaluation Submission for HugeScope [ATC '24] 

This repository contains the artifact for reproducing our ATC'24 paper "Taming Hot Bloat Under Virtualization with HugeScope". 

# Table of Contents
* [Overview](#overview)
* [Setup](#setup)
* [Running experiments](#running-experiments)
* [Validation of the main claims](#validation-of-the-main-claims)
* [Known Issues](#known-issues)
* [Authors](#authors)

# Overview 

### Structure:

```
|---- linux-5.4.142-host    (source code of kernel with HugeScope)
|---- benchmakrks           (benchmarks run in Guest OS)
|---- switch_kmodule        (kernel module to disable/enable HS-TMM and HS-Share)
|---- dep.sh                (scripts to install dependency)
|---- disable_thp.sh        (script to disable huge page)
|---- enable_thp.sh         (script to enable huge page)
```

### Environment: 

Our artifact should run on any Linux distribution. The current scripts are developed for **Ubuntu 20.04.6 LTS**. The `dep.sh` contains some dependencies that the compilation kernel may need.

The `HS-TMM` requires a machine equipped with Intel Optane persistent memory. We run our evaluation on a dual-socket Intel Cascade Lake-SP system running at 2.2 GHz with 24 cores/48 threads per socket. The system enables the APP direct mode and mounts PMem as a NUMA node. If you don't have a NVM environment, please see [Use DRAM as the slow memory](#0-use-dram-as-the-slow-memory).

HugeScope does not need to modify the Guest OS, so it supports running any virtual machine image. Here we provide some open source benchmark and qemu startup scripts running in the virtual machine in the benchmarks directory.

# Setup 

### 0. Use DRAM as the slow memory:

If you don't have NVM hardware, you can use remote DRAM as slow memory to test HS-TMM.

However, because our implementation is bound to an PMEM in node 3, it needs to be modified in the following two places.

The implementation code of HS-TMM needs to modify the Slow memory node to the numa node id of your remote DRAM.  
```
$ cd linux-5.4.142-host/arch/x86/kvm
$ vim ss_work.h
# Modify the NVM_NODE of 25 lines to the node of the remote DRAM.
# You can also modify the size (GB) of fast memory (26L) and slow memory (27L) here.
$ make
$ rmmod kvm
$ insmod kvm

```

The VM startup script also needs to be modified.
```
$ vim benchmarks/run_vm_for_hs_tmm.sh
# Modify the host_nodes of 11 lines to the node of the remote DRAM.
```

### 1. Install the dependencies:
```
$ ./dep.sh 
```

### 2. Install the 5.4.142 Linux kernel for host
```
$ cd linux-5.4.142-host
$ cp config .config
$ make oldconfig             (update the config with the provided .config file)
```

Next, please use your favorite way to compile and install the kernel. The below step is just for reference. The installation takes around 30 minutes on our machines. 

For Ubuntu:
```
$ make -j8              
$ make -j8 modules 
$ sudo make modules_install
$ sudo make install
```
Reboot the machine to the installed 5.4.142 kernel. 

Enable huge pages.
```
$ ./enable_thp.sh
```

### 3. Run VM
HugeScope does not impose any restrictions on virtual machines. The benchmarks directory includes benchmarks used in the paper and qemu startup scripts we use.

HS-TMM will require Host to have NVM backed NUMA node. In our machine, node0 is DRAM and node3 is NVM. Please modify it according to your configuration.

The VM also need to enable huge pages, you can use enable_thp.sh if your VM is also Ubuntu.

Run Redis in VM.
```
$ cd benchmarks/ycsb
$ ./load_redis.sh
$ ./run_redis.sh
```

Run gapbs in VM.
```
$ cd benchmarks/gapbs
$ ./run.sh

```

Run graph500
```
$ cd benchmarks/graph/graph500-newreference/src
$ ./run_19GB_8core.sh

```
### 4. Install the switch kernel module
```
$ cd switch_kmodule
$ make
```

Now you can control the enabling of different subsystems by loading the switch module. Note that the corresponding test will only be performed once after the module is loaded.

```
# Switch module will test different subsystems according to test_selector.
# 1 : HS-TMM
# 2 : vTMM
# 3 : HS-share
# 4 : ksm_huge
# 5 : ksm
# 6 : Ingens
# 7 : zero_scan
$ insmod switch.ko test_selector=x
$ rmmod switch
```

# Known issues 

1. The parameters of the current HS-TMM are coupled with the system. If it needs to be modified, please modify Linux-5.4.142-host/arch/x86/KVM/ss_work.h.

2. Because in our test, the influence of page sharing itself on performance is excluded and we only focus on observing the influence of different page splitting strategies on performance, all page sharing mechanisms will not really share pages but only count relevant data.

3. NX huge pages should be disabled
# Authors

Chuandong Li (PKU)

Sai Sha (PKU) 

Diyu Zhou (EPFL) 

Yangqing Zeng  (PKU) 

Xiran Yang (PKU) 

Yingwei Luo (PKU)

Xiaolin Wang (PKU)

Zhenlin Wang (Michigan Tech)

# License

Trio is licensed under Apache License 2.0.
