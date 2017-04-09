# Assignment_2

This is a block device driver which allocates 512KB of RAM as a block device and creates 3 logical and 3 primary partitions.

To make the module
$make

To insert the module
$sudo insmod ./dor.ko

To check for partitions
$ ls -l /dev/mydisk*

To write to and read from the partition first root mode - sudo su
$ cat > /dev/mydisk1

To read the data from partition
$xxd /dev/mydisk1


