#include "fiber.h"

static bool debug = false;

namespace sylar {

// -------------------------- 共享栈池实现（新增部分） --------------------------
// 实现头文件声明的线程局部共享栈池
thread_local SharedStackPool g_shared_stack_pool;

// SharedStackPool构造：初始化N个共享栈
SharedStackPool::SharedStackPool(int stack_num, size_t stack_size) {
    m_pools.resize(stack_num);
    for (auto& s : m_pools) {
        s.bottom = new char[stack_size];
        s.size = stack_size;
        s.is_used = false;
    }
}

// SharedStackPool析构
SharedStackPool::~SharedStackPool() {
    for (auto& s : m_pools) {
        delete[] s.bottom;
    }
}

// 获取空闲共享栈，没找到就动态扩容
SharedStack* SharedStackPool::get_free() {
    for (auto& s : m_pools) {
        if (!s.is_used) {
            s.is_used = true;
            return &s;
        }
    }
    // 动态扩容：不够用自动加新的共享栈
    SharedStack new_stack;
    new_stack.bottom = new char[m_pools[0].size];
    new_stack.size = m_pools[0].size;
    new_stack.is_used = true;
    m_pools.push_back(new_stack);
    return &m_pools.back();
}

// 释放共享栈回池复用
void SharedStackPool::release(SharedStack* s) {
    s->is_used = false;
}

// -------------------------- 原有逻辑，仅修改栈管理相关 --------------------------
// 当前线程上的协程控制信息
// 正在运行的协程
static thread_local Fiber* t_fiber = nullptr;
// 主协程
static thread_local std::shared_ptr<Fiber> t_thread_fiber = nullptr;
// 调度协程
static thread_local Fiber* t_scheduler_fiber = nullptr;

// 协程计数器
static std::atomic<uint64_t> s_fiber_id{0};
// 协程id
static std::atomic<uint64_t> s_fiber_count{0};

void Fiber::SetThis(Fiber *f)
{
	t_fiber = f;
}

// 首先运行该函数创建主协程
std::shared_ptr<Fiber> Fiber::GetThis()
{
	if(t_fiber)
	{	
		return t_fiber->shared_from_this();
	}

	std::shared_ptr<Fiber> main_fiber(new Fiber());
	t_thread_fiber = main_fiber;
	t_scheduler_fiber = main_fiber.get(); // 除非主动设置 主协程默认为调度协程
	
	assert(t_fiber == main_fiber.get());
	return t_fiber->shared_from_this();
}

void Fiber::SetSchedulerFiber(Fiber* f)
{
	t_scheduler_fiber = f;
}

uint64_t Fiber::GetFiberId()
{
	if(t_fiber)
	{
		return t_fiber->getId();
	}
	return (uint64_t)-1;
}

// 主协程构造无需修改（用系统默认栈）
Fiber::Fiber()
{
	SetThis(this);
	m_state = RUNNING;
	
	if(getcontext(&m_ctx))
	{
		std::cerr << "Fiber() failed\n";
		pthread_exit(NULL);
	}
	
	m_id = s_fiber_id++;
	s_fiber_count ++;
	if(debug) std::cout << "Fiber(): main id = " << m_id << std::endl;
}

// 子协程构造：改造为共享栈
Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler):
m_cb(cb), m_runInScheduler(run_in_scheduler)
{
	m_state = READY;
	// 共享栈改造：不再给每个协程预分配完整独立栈
	m_stacksize = stacksize ? stacksize : 128000;
	m_save_stack = nullptr;
	m_save_cap = 0;
	m_save_len = 0;

	if(getcontext(&m_ctx))
	{
		std::cerr << "Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler) failed\n";
		pthread_exit(NULL);
	}
	
	m_ctx.uc_link = nullptr;
	makecontext(&m_ctx, &Fiber::MainFunc, 0);
	
	m_id = s_fiber_id++;
	s_fiber_count ++;
	if(debug) std::cout << "Fiber(): child id = " << m_id << std::endl;
}

// 析构：改造释放逻辑
Fiber::~Fiber()
{
	s_fiber_count --;
	// 共享栈改造：只释放协程自己的保存区
	if(m_save_stack) {
        delete[] (char*)m_save_stack;
    }
	if(debug) std::cout << "~Fiber(): id = " << m_id << std::endl;	
}

// reset：改造复用逻辑
void Fiber::reset(std::function<void()> cb)
{
	assert(m_state == TERM);
	m_state = READY;
	m_cb = cb;

	// 共享栈改造：清空保存区即可
	m_save_len = 0;
	if(m_save_cap > 0) {
        memset(m_save_stack, 0, m_save_cap);
    }

	if(getcontext(&m_ctx))
	{
		std::cerr << "reset() failed\n";
		pthread_exit(NULL);
	}

	m_ctx.uc_link = nullptr;
	makecontext(&m_ctx, &Fiber::MainFunc, 0);
}

// resume：改造：恢复协程时加载共享栈
void Fiber::resume()
{
	assert(m_state==READY);
	m_state = RUNNING;

    // 共享栈改造：获取空闲共享栈 + 恢复栈数据
    SharedStack* shared_stack = g_shared_stack_pool.get_free();
    if(m_save_len > 0) {
        // Linux栈从高地址向低地址生长，计算正确偏移
        char* target_pos = shared_stack->bottom + shared_stack->size - m_save_len;
        memcpy(target_pos, (char*)m_save_stack, m_save_len);
        // 上下文绑定到当前共享栈
        m_ctx.uc_stack.ss_sp = shared_stack->bottom;
        m_ctx.uc_stack.ss_size = shared_stack->size;
    }

	if(m_runInScheduler)
	{
		SetThis(this);
		if(swapcontext(&(t_scheduler_fiber->m_ctx), &m_ctx))
		{
			std::cerr << "resume() to t_scheduler_fiber failed\n";
			pthread_exit(NULL);
		}		
	}
	else
	{
		SetThis(this);
		if(swapcontext(&(t_thread_fiber->m_ctx), &m_ctx))
		{
			std::cerr << "resume() to t_thread_fiber failed\n";
			pthread_exit(NULL);
		}	
	}
}

// yield：改造：让出协程时保存栈数据
void Fiber::yield()
{
	assert(m_state==RUNNING || m_state==TERM);

    // 共享栈改造：保存当前栈数据到协程缓冲区 + 释放共享栈
    char* current_fp = (char*)__builtin_frame_address(0);
    // 找到当前正在使用的共享栈
    SharedStack* cur_stack = nullptr;
    for (auto& s : g_shared_stack_pool.m_pools) {
        if (s.is_used && current_fp >= s.bottom && current_fp <= s.bottom + s.size) {
            cur_stack = &s;
            break;
        }
    }
    // 计算实际使用的栈大小
    size_t used_len = cur_stack->bottom + cur_stack->size - current_fp;

    // 保存区容量不足则2倍动态扩容
    if (used_len > m_save_cap) {
        char* new_save = new char[used_len * 2];
        if (m_save_stack) {
            memcpy(new_save, (char*)m_save_stack, m_save_len);
            delete[] (char*)m_save_stack;
        }
        m_save_stack = new_save;
        m_save_cap = used_len * 2;
    }

    // 拷贝栈数据到协程私有保存区
    memcpy((char*)m_save_stack, current_fp, used_len);
    m_save_len = used_len;
    // 释放共享栈回池复用
    g_shared_stack_pool.release(cur_stack);

    // 原有上下文切换逻辑不变
	if(m_state!=TERM) {
        m_state = READY;
    }

	if(m_runInScheduler)
	{
		SetThis(t_scheduler_fiber);
		if(swapcontext(&m_ctx, &(t_scheduler_fiber->m_ctx)))
		{
			std::cerr << "yield() to to t_scheduler_fiber failed\n";
			pthread_exit(NULL);
		}		
	}
	else
	{
		SetThis(t_thread_fiber.get());
		if(swapcontext(&m_ctx, &(t_thread_fiber->m_ctx)))
		{
			std::cerr << "yield() to t_thread_fiber failed\n";
			pthread_exit(NULL);
		}	
	}	
}

void Fiber::MainFunc()
{
	std::shared_ptr<Fiber> curr = GetThis();
	assert(curr!=nullptr);

	curr->m_cb(); 
	curr->m_cb = nullptr;
	curr->m_state = TERM;

	// 运行完毕 -> 让出执行权
	auto raw_ptr = curr.get();
	curr.reset(); 
	raw_ptr->yield(); 
}

}