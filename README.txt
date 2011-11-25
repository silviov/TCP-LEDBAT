How to debug

At the top of the source file for the module you can find some constant 
definitions. By setting their values the user can select the level of debugging
information printed by the module.

In order to see the messages, however, remember to set the debug level of the
kernel to DEBUG with the command
	
	sudo sysctl kernel.printk=8

Moreover since the module prints nearly every packet, you should kill klogd and listen to the kernel messages by yourself
	
	cat /proc/kmsg >dump.file
	tee dump.file </proc/kmsg

