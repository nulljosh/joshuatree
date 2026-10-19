Gate: none known. Post as a text post, flair it if the sub asks. Say plainly how it was built if a commenter asks; r/osdev cares about that.

Title: Joshua Tree: my from-scratch OS now boots on a Raspberry Pi 4, with accounts and sudo

Body:
Joshua Tree started as an i386 kernel in C with no libc: my own scheduler, paging, FAT16/32 driver, a network stack with DHCP and TCP, and a windowing desktop with about 30 apps. It boots in the browser at https://joshuatree.jaybulb.com.

The newer half is an ARM64 port that runs on a real Pi 4 from one flashed SD card. This week it got user accounts (PBKDF2-HMAC-SHA256 hashes over BearSSL in a USERS.TXT on the FAT partition), a login screen and sudo with a five-minute remembered password.

The part I'm proudest of is the testing. Every change boots the kernel in QEMU in CI and drives it with keys and clicks, and each feature has a mutation that has to make its check fail. Next up is a power-cut sweep for the FAT writes: cut after every sector write and want fsck clean afterwards.

Source, Apache 2.0: https://github.com/nulljosh/joshuatree. I'd love feedback from people who have done a Pi 4 bring-up, especially on the SD card driver.

Post: https://old.reddit.com/r/osdev/comments/1x9ml07/joshua_tree_my_fromscratch_os_now_boots_on_a/
