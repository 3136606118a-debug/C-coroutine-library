#ifndef _COROUTINE_H_
#define _COROUTINE_H_

#include <iostream>     
#include <memory>       
#include <atomic>       
#include <functional>   
#include <cassert>      
#include <ucontext.h>   
#include <unistd.h>
#include <mutex>

// 👉 第一步：添加共享栈需要的头文件
#include <vector>
#include <cstring>

namespace sylar {

// 👉 第二步：添加共享栈管理结构（放在Fiber类外）
// 单个共享栈：整个线程只有几个这样的共享栈，所有协程轮流复用
struct SharedStack {
    char*  bottom;  // 共享栈的栈底（Linux栈从高地址往低地址生长）
    size_t size;    // 共享栈总大小
    bool   is_used; // 当前是否被协程占用
};

// 每个线程独立的共享栈池：加对齐避免缓存伪共享，不需要锁
class alignas(64) SharedStackPool {
public:
    // 默认创建8个16KB的共享栈，数量和CPU核心匹配
    SharedStackPool(int stack_num = 32, size_t stack_size = (1 << 16));
    ~SharedStackPool();

    // 获取空闲共享栈
    SharedStack* get_free();
    // 释放用完的共享栈
    void release(SharedStack* s);

private:
    std::vector<SharedStack> m_pools;
};

// 线程局部变量：每个线程独一份，完全无锁
extern thread_local SharedStackPool g_shared_stack_pool;


class Fiber : public std::enable_shared_from_this<Fiber>
{
public:
	// 协程状态
	enum State
	{
		READY, 
		RUNNING, 
		TERM 
	};

private:
	// 仅由GetThis()调用 -> 私有 -> 创建主协程  
	Fiber();

public:
	Fiber(std::function<void()> cb, size_t stacksize = 0, bool run_in_scheduler = true);
	~Fiber();

	// 重用一个协程
	void reset(std::function<void()> cb);

	// 任务线程恢复执行
	void resume();
	// 任务线程让出执行权
	void yield();

	uint64_t getId() const {return m_id;}
	State getState() const {return m_state;}

public:
	// 设置当前运行的协程
	static void SetThis(Fiber *f);

	// 得到当前运行的协程 
	static std::shared_ptr<Fiber> GetThis();

	// 设置调度协程（默认为主协程）
	static void SetSchedulerFiber(Fiber* f);
	
	// 得到当前运行的协程id
	static uint64_t GetFiberId();

	// 协程函数
	static void MainFunc();	

private:
	// id
	uint64_t m_id = 0;
	// 栈大小
	uint32_t m_stacksize = 0;
	// 协程状态
	State m_state = READY;
	// 协程上下文
	ucontext_t m_ctx;

	// 👉 第三步：修改原来的独立栈成员为共享栈的保存区
	// 原独立栈写法（注释掉，保留原来代码方便回退）
	// // 协程栈指针
	// void* m_stack = nullptr;

	// 共享栈修改：协程只需要保存自己实际用到的栈数据
	void* m_save_stack = nullptr; // 切出时保存协程的栈数据
	size_t m_save_cap = 0;        // 保存区的容量
	size_t m_save_len = 0;        // 实际使用的大小
	// 修改结束

	// 协程函数
	std::function<void()> m_cb;
	// 是否让出执行权交给调度协程
	bool m_runInScheduler;

public:
	std::mutex m_mutex;
};

}

#endif