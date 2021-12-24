### What new in this BusyBox repo
Already compiled and built ```initramfs.cpio.gz``` for X86_64 (built on ubuntu)

You can try it out using following command. I have also checked-in a linux binary (bzImage) I compiled (I have modified 1-2 lines of debug)
```shell
qemu-system-x86_64 -kernel bzImage -initrd initramfs.cpio.gz -hda /dev/zero -append "root=/dev/zero console=ttyS0" -serial stdio -display none 
```

NOTE - you can just download "bzImage" and "initramfs.cpio.gz" files. You don't need anything else (if you are not planning to edit the code)
