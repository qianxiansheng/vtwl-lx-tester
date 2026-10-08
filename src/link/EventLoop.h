#pragma once
#include <cstdint>
#include <functional>

namespace Lx::Link {

// 业务线程事件循环的抽象：链路层/工位控制器只在业务线程上运行，用它投递任务和定时器。
class IEventLoop {
public:
	using Task = std::function<void()>;
	using TimerId = uint64_t;

	virtual ~IEventLoop() = default;

	// 任意线程可调用：把任务投递到业务线程
	virtual void post(Task t) = 0;

	// 仅业务线程调用：延时执行；返回的 id 可用于取消
	virtual TimerId after(uint32_t ms, Task t) = 0;
	virtual void cancel(TimerId id) = 0;
};

} // namespace Lx::Link
