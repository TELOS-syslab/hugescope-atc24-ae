
#/bin/bash
taskset -c 48-55 qemu-system-x86_64 -name guest=vm0,debug-threads=off \
    -machine pc-i440fx-2.9 \
    -cpu host \
    -m 120G \
    -enable-kvm\
    -overcommit mem-lock=off \
    -smp 8 \
    -object memory-backend-ram,size=120G,host-nodes=0,policy=bind,prealloc=no,id=m0 \
    -numa node,nodeid=0,memdev=m0 \
    -uuid 9bc02bdb-58b3-4bb0-b00e-313bdae0ac81 \
    -device ich9-usb-ehci1,id=usb,bus=pci.0,addr=0x5.0x7 \
    -device virtio-serial-pci,id=virtio-serial0,bus=pci.0,addr=0x6 \
    -drive file=path/to/vm.qcow2,format=qcow2,if=none,id=drive-ide0-0-0 \
    -device ide-hd,bus=ide.0,unit=0,drive=drive-ide0-0-0,id=ide0-0-0,bootindex=1 \
    -drive if=none,id=drive-ide0-0-1,readonly=on \
    -device ide-cd,bus=ide.0,unit=1,drive=drive-ide0-0-1,id=ide0-0-1 \
    -device virtio-balloon-pci,id=balloon0,bus=pci.0,addr=0x7 \
    -netdev user,id=ndev.0,hostfwd=tcp::5555-:22 \
    -device e1000,netdev=ndev.0 \
    -nographic \
    -append "root=/dev/sda1 console=ttyS0"