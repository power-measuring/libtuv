You can find project details in [wiki](https://github.com/pando-project/libtuv/wiki)
gem5-se branch is intended for running iot.js on gem5 simulator in syscall emulation mode.
Since gem5 lacks support for epoll, we replace the epoll in unix/linux-core.c with poll implementation in nuttx port.
