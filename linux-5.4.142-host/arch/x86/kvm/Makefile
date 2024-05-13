# SPDX-License-Identifier: GPL-2.0

ccflags-y += -Iarch/x86/kvm
#ccflags-y += -DHMM_SVM_FLAG
ccflags-y += -DHMM_THP_FLAG
#ccflags-y += -DHMM_RECORD_EXIT

ccflags-y += -DHMM_HSPAGE_FLAG

KVM := ../../../virt/kvm

kvm-y			+= $(KVM)/kvm_main.o $(KVM)/coalesced_mmio.o \
				$(KVM)/eventfd.o $(KVM)/irqchip.o $(KVM)/vfio.o
kvm-$(CONFIG_KVM_ASYNC_PF)	+= $(KVM)/async_pf.o

kvm-y			+= x86.o mmu.o emulate.o i8259.o irq.o lapic.o \
			   i8254.o ioapic.o irq_comm.o cpuid.o pmu.o mtrr.o \
			   hyperv.o page_track.o debugfs.o lcd_work.o 

kvm-y			+= vmx/vmx.o vmx/vmenter.o vmx/pmu_intel.o vmx/vmcs12.o vmx/evmcs.o vmx/nested.o
kvm-y			+= hmmv/init_mm.o hmmv/mem_track.o hmmv/hot_or_cold.o hmmv/page_migration.o hmmv/svm.o hmmv/ept_scanner.o hmmv/hot_or_cold_thp.o 
obj-$(CONFIG_KVM)	+= kvm.o
