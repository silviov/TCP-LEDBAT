# TCP-LEDBAT kernel module

This is an implementation of the LEDBAT congestion control algorithm over TCP
using the Linux kernel modular congestion control framework.

The current version is updated to compile under Linux kernel 4.9.x versions. A version
working with kernels up to 3.3.x is tagged with a "3.3" tag. The module has not been tested
with version in between.

The module currently loads properly but it has *not* been tested thoroughly.

For more information also visit http://perso.telecom-paristech.fr/~drossi/index.php?n=Software.LEDBAT

## How to build

* clone the this repository

```shell
$ git clone https://github.com/silviov/TCP-LEDBAT.git
```

* build the module

```shell
$ cd TCP-LEDBAT/src
$ make
```

* load the module

```shell
$ sudo make install
```

## How to use the module

After the module has been loaded check "ledbat" should appear among the available
congestion control algorithms. You should see something like

``` shell
$ cat /proc/sys/net/ipv4/tcp_available_congestion_control
cubic reno ledbat
```

Then you can either use it for all flows by doing

```shell
$ sudo sysctl -w net.ipv4.tcp_congestion_control=ledbat
```

or you can do it on specific sockets via the `setsockopt` option. You can see an
example in the `utils/client.c` file.

## How to debug

At the top of the source file for the module you can find some constant 
definitions. By setting their values you can select the level of debugging
information printed by the module.

In order to see the messages, however, remember to set the debug level of the
kernel to DEBUG with the command

```shell
$ sudo sysctl kernel.printk=8
```

