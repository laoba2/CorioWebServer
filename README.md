# A C++ High Performance Web Server


## Introduction  

本项目为C++11编写的Web服务器，参考了传统epoll形式的webserver修改之后，使用更高星的协程和io_uring，减少系统调用的开销  
全异步 I/O：基于 Linux io_uring，实现零/低系统调用的异步网络 I/O
C++20 协程：使用 Task<T> 类型以同步风格编写异步代码，避免回调地狱
Acceptor + Worker 多线程模型：连接接入与请求处理分离，充分利用多核 CPU
线程局部内存池：协程帧通过 std::pmr::unsynchronized_pool_resource 分配，减少锁竞争和内存碎片
可选的 mimalloc 支持：上游分配器可替换为高性能 mimalloc
异步日志系统：基于无锁 SPSC 环形队列，后台线程异步刷盘
HTTP/1.1 支持：GET/HEAD/POST、Keep-Alive、静态文件服务
请求超时控制：利用 io_uring linked timeout 实现请求级超时

## Envoirment  
* OS: Ubuntu 14.04
* Complier: g++ 4.8
* CMake 3.16+
* GCC 10+ 

## Build

	git clone https://github.com/yourname/cws.git
	cd cws
	mkdir build && cd build
	cmake ..
	make




