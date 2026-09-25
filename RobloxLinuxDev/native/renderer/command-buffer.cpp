#include <indium/command-buffer.private.hpp>
#include <indium/command-queue.private.hpp>
#include <indium/device.private.hpp>
#include <indium/render-command-encoder.private.hpp>
#include <indium/texture.private.hpp>
#include <indium/drawable.hpp>
#include <indium/blit-command-encoder.private.hpp>
#include <indium/compute-command-encoder.private.hpp>
#include <indium/dynamic-vk.hpp>
#include <indium/uploads.hpp>
#include <indium/tracked-blit.hpp>
#include <indium/gpu-timing.hpp>
#include "presentation-semaphore.hpp"

#include <condition_variable>
#include <map>
#ifdef DARLING
uint64_t trackb_next_present_serial();
void trackb_present_drawable(std::shared_ptr<Indium::Drawable> drawable, uint64_t serial = 0);
#endif

extern "C" void* dispatch_queue_create(const char*, void*);
extern "C" void* dispatch_get_global_queue(long, unsigned long);
extern "C" void dispatch_async_f(void*, void*, void (*)(void*));
namespace {
bool usesGraphicsQueueOnly(std::shared_ptr<Indium::Texture> texture) {
	while (auto parent = texture->parentTexture()) texture = std::move(parent);
	// Concrete textures (including ViewportFrame targets) and their uploads
	// use graphicsQueue(). Drawables can also be used by GL/the present queue.
	return dynamic_cast<Indium::ConcreteTexture*>(texture.get()) != nullptr;
}

void synchronizeGraphicsQueue(VkCommandBuffer command) {
	// Replace per-texture semaphore pairs with one dependency on earlier queue
	// submissions. Include WAR ordering as well as visibility of prior writes.
	// ponytail: whole-queue barrier; narrow to resource hazards if this becomes a bottleneck.
	VkMemoryBarrier barrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	Indium::DynamicVK::vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void scheduleMetalHandlers(std::function<void()> work, bool completed = false) {
	// Application scheduled handlers can block. Keep them off the GPU polling
	// thread so unrelated command and drawable completions continue to drain.
	static void* queue = dispatch_queue_create("RobloxLinux.MetalScheduled", nullptr);
	// Completion handlers for independent commands must not hold up each other
	// or the GPU polling thread. Each command's own handlers remain ordered.
	dispatch_async_f(completed ? dispatch_get_global_queue(0, 0) : queue,
		new std::function<void()>(std::move(work)), [](void* context) {
		std::unique_ptr<std::function<void()>> callback(static_cast<std::function<void()>*>(context));
		(*callback)();
	});
}
}

Indium::CommandBuffer::~CommandBuffer() {};

void Indium::setRetainedReferences(CommandBuffer& commandBuffer, bool retains) {
	// Every Indium command buffer is a PrivateCommandBuffer.
	static_cast<PrivateCommandBuffer&>(commandBuffer).setRetainsReferences(retains);
}

std::shared_ptr<Indium::CommandQueue> Indium::PrivateCommandBuffer::commandQueue() {
	return _privateCommandQueue;
};
std::shared_ptr<Indium::Device> Indium::PrivateCommandBuffer::device() {
	return _privateDevice;
};

Indium::PrivateCommandBuffer::PrivateCommandBuffer(std::shared_ptr<PrivateCommandQueue> commandQueue):
	_privateCommandQueue(commandQueue)
{
	RbxProfiler::DiagnosticScope create(RBX_DIAG_COMMAND_CREATE,0,this);
	_privateDevice = _privateCommandQueue->privateDevice();

	// Each pending buffer owns its pool exclusively until GPU completion.
	{
		RbxProfiler::DiagnosticScope poolLock(RBX_DIAG_COMMAND_POOL_LOCK,0,this);
		std::scoped_lock lock(_privateDevice->recycledCommandMutex);
		poolLock.detail=_privateDevice->recycledCommands.size();
		poolLock.finish();
		if (!_privateDevice->recycledCommands.empty()) {
			auto entry = _privateDevice->recycledCommands.back();
			_privateDevice->recycledCommands.pop_back();
			_commandPool = entry.first; _commandBuffer = entry.second;
		}
	}
	if (!_commandPool) {
		RbxProfiler::DiagnosticScope allocate(RBX_DIAG_COMMAND_POOL_CREATE,0,this);
		VkCommandPoolCreateInfo poolInfo {};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.queueFamilyIndex = _privateDevice->graphicsQueueFamilyIndex() ? *_privateDevice->graphicsQueueFamilyIndex() : *_privateDevice->computeQueueFamilyIndex();
		if (DynamicVK::vkCreateCommandPool(_privateDevice->device(), &poolInfo, nullptr, &_commandPool) != VK_SUCCESS)
			throw std::runtime_error("Could not create command pool");
		VkCommandBufferAllocateInfo allocInfo {};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = _commandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;

		if (DynamicVK::vkAllocateCommandBuffers(_privateDevice->device(), &allocInfo, &_commandBuffer) != VK_SUCCESS) {
			// TODO: handle this in a more C++-friendly way
			abort();
		}

	}

	VkCommandBufferBeginInfo beginInfo {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	RbxProfiler::DiagnosticScope begin(RBX_DIAG_COMMAND_BEGIN,0,this);
	if (DynamicVK::vkBeginCommandBuffer(_commandBuffer, &beginInfo) != VK_SUCCESS) {
		// TODO: same
		abort();
	}
	begin.finish();
	synchronizeGraphicsQueue(_commandBuffer);
	RbxProfiler::DiagnosticScope queries(RBX_DIAG_QUERY_SETUP,0,this);
	_timestampPool = beginGpuTiming(*_privateDevice, _commandBuffer);
};

Indium::PrivateCommandBuffer::~PrivateCommandBuffer() {
	if (_timestampPool) {
		std::scoped_lock lock(_privateDevice->rendererCacheMutex);
		if (_privateDevice->timestampPools.size() < 32) _privateDevice->timestampPools.push_back(_timestampPool);
		else destroyTimestampPool(_privateDevice->device(), _timestampPool);
	}
	recycleCommandPool();
};

void Indium::PrivateCommandBuffer::recycleCommandPool() {
	if (!_commandPool) return;
	const auto pool = _commandPool;
	const auto command = _commandBuffer;
	_commandPool = VK_NULL_HANDLE;
	_commandBuffer = VK_NULL_HANDLE;
	// A buffer that was never committed is also safe to reset. Bound the idle
	// cache so a temporary burst of command buffers does not retain all its pools.
	if (DynamicVK::vkResetCommandPool(_privateDevice->device(), pool, 0) == VK_SUCCESS) {
		std::scoped_lock lock(_privateDevice->recycledCommandMutex);
		if (_privateDevice->recycledCommands.size() < 32) {
			_privateDevice->recycledCommands.emplace_back(pool, command);
			return;
		}
	}
	DynamicVK::vkDestroyCommandPool(_privateDevice->device(), pool, nullptr);
};

std::shared_ptr<Indium::RenderCommandEncoder> Indium::PrivateCommandBuffer::renderCommandEncoder(const RenderPassDescriptor& descriptor) {
	auto encoder = std::make_shared<Indium::PrivateRenderCommandEncoder>(shared_from_this(), descriptor);
	{
		std::scoped_lock lock(_mutex);
		_commandEncoders.push_back(encoder);
	}
	return encoder;
};

std::shared_ptr<Indium::BlitCommandEncoder> Indium::PrivateCommandBuffer::blitCommandEncoder() {
	auto encoder = std::make_shared<Indium::TrackedBlit>(shared_from_this());
	{
		std::scoped_lock lock(_mutex);
		_commandEncoders.push_back(encoder);
	}
	return encoder;
};

std::shared_ptr<Indium::BlitCommandEncoder> Indium::PrivateCommandBuffer::blitCommandEncoder(const BlitPassDescriptor& descriptor) {
	auto encoder = std::make_shared<Indium::TrackedBlit>(shared_from_this(), descriptor);
	{
		std::scoped_lock lock(_mutex);
		_commandEncoders.push_back(encoder);
	}
	return encoder;
};

std::shared_ptr<Indium::ComputeCommandEncoder> Indium::PrivateCommandBuffer::computeCommandEncoder() {
	return computeCommandEncoder(ComputePassDescriptor {});
};

std::shared_ptr<Indium::ComputeCommandEncoder> Indium::PrivateCommandBuffer::computeCommandEncoder(const ComputePassDescriptor& descriptor) {
	auto encoder = std::make_shared<Indium::PrivateComputeCommandEncoder>(shared_from_this(), descriptor);
	{
		std::scoped_lock lock(_mutex);
		_commandEncoders.push_back(encoder);
	}
	return encoder;
};

std::shared_ptr<Indium::ComputeCommandEncoder> Indium::PrivateCommandBuffer::computeCommandEncoder(DispatchType dispatchType) {
	return computeCommandEncoder(ComputePassDescriptor { {}, dispatchType });
};

void Indium::PrivateCommandBuffer::commit() {
	RbxProfiler::DiagnosticScope diagnostic(RBX_DIAG_COMMAND_COMMIT,0,this);
 RbxProfiler::CpuScope cpu(RBX_PROF_COMMIT);
 RbxProfiler::CpuScope phase(RBX_PROF_RESOURCES);
	const auto timingStart = [] { return commandTimingStart(); };
	const auto commitStart = timingStart();
	std::unique_lock lock(_mutex);

	_committed = true;

	// Multiple passes (and texture views) may reference the same image. Lock/acquire
	// it only once per submission; otherwise we deadlock or wait on our own signal.
	std::map<VkImage, std::shared_ptr<PrivateTexture>> reads, writes;
	// Every Indium texture is a PrivateTexture.
	const auto addTextures = [](auto& target, const auto& textures) {
		for (const auto& texture : textures) if (texture) {
			auto image = std::static_pointer_cast<PrivateTexture>(texture);
			target.emplace(image->image(), image);
		}
	};
	for (const auto& encoder: _commandEncoders) {
		if (auto render = std::dynamic_pointer_cast<PrivateRenderCommandEncoder>(encoder)) {
			if(diagnostic.active())diagnostic.detail|=1;
			addTextures(reads, render->readOnlyTextures()); addTextures(writes, render->readWriteTextures());
		}
		if (auto blit = std::dynamic_pointer_cast<TrackedBlit>(encoder)) {
			if(diagnostic.active()){diagnostic.detail|=2;diagnostic.bytes+=blit->bufferCopyBytes;}
			addTextures(reads, blit->reads); addTextures(writes, blit->writes);
		}
		if (auto compute = std::dynamic_pointer_cast<PrivateComputeCommandEncoder>(encoder)) {
			if(diagnostic.active())diagnostic.detail|=4;
			// Compute snapshots retain every dispatched binding. Conservatively
			// serialize storage images until reflection distinguishes their access.
			for (const auto& state : compute->_savedFunctionResources) addTextures(writes, state.textures);
		}
	}
	std::vector<std::shared_ptr<Texture>> readOnlyTextures, readWriteTextures;
	// With the swapchain presenter, drawables are copied straight from their
	// image: no GL export blit (precommit) and no exportable semaphore.
	const bool glPresentation = !_privateDevice->vulkanPresentation.load(std::memory_order_acquire);
	const auto exported = [glPresentation](const std::shared_ptr<PrivateTexture>& texture) {
		return glPresentation && texture->needsExportablePresentationSemaphore();
	};
	for (const auto& [image, texture]: writes) {
		reads.erase(image);
		readWriteTextures.push_back(texture);
		if (glPresentation || !texture->needsExportablePresentationSemaphore()) texture->precommit(shared_from_this());
	}
	for (const auto& [image, texture]: reads) {
		readOnlyTextures.push_back(texture);
		if (glPresentation || !texture->needsExportablePresentationSemaphore()) texture->precommit(shared_from_this());
	}

	// Serialize texture dependency allocation and submission on the shared VkQueue.
	const auto queueStart = timingStart();
 phase.change(RBX_PROF_QUEUE);
	RbxProfiler::DiagnosticScope queueWait(RBX_DIAG_QUEUE_LOCK,0,this);
	std::unique_lock queueLock(_privateDevice->queueMutex);
	queueWait.finish();
	const auto queueAcquired = timingStart();
 phase.change(RBX_PROF_UPLOADS);
	if (_privateDevice->uploads) { _privateDevice->uploads->flush(); _privateDevice->uploads->collect(); }
	const auto uploadsFinished = timingStart();
 phase.change(RBX_PROF_END_COMMAND);
	endGpuTiming(_commandBuffer, _timestampPool);
	if (DynamicVK::vkEndCommandBuffer(_commandBuffer) != VK_SUCCESS) {
		// TODO
		abort();
	}
	const auto encodingFinished = timingStart();
 phase.change(RBX_PROF_SYNC);
	RbxProfiler::DiagnosticScope sync(RBX_DIAG_COMMAND_SYNC,diagnostic.bytes,this,diagnostic.detail);

	// to keep ourselves alive until we're done
	auto self = shared_from_this();

	// for the event loop
	auto timelineSemaphore = _privateDevice->getCompletionSemaphore();

	++timelineSemaphore->count;
	_completionSemaphore = timelineSemaphore;

	std::vector<std::shared_ptr<BinarySemaphore>> presentationSemaphores;

	for (const auto& texture: readWriteTextures) {
		auto privateTexture = std::static_pointer_cast<PrivateTexture>(texture);
		// Track B uses Darling's CAMetalDrawable for presentation. Ordinary
		// attachments are synchronized on the graphics queue;
		// no consumer ever waits on a presentation binary semaphore for them.
		if (!exported(privateTexture)) {
			presentationSemaphores.push_back(nullptr);
			continue;
		}
		auto sema = PresentationSemaphores::acquire(_privateDevice);
		presentationSemaphores.push_back(sema);
		privateTexture->beginUpdatingPresentationSemaphore(sema);
	}

	std::vector<VkSemaphoreSubmitInfo> signalInfos;
	std::vector<VkSemaphoreSubmitInfo> waitInfos;
	VkCommandBufferSubmitInfo commandBufferInfo {};

	VkSemaphoreSubmitInfo signalEventLoopInfo {};
	signalEventLoopInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	signalEventLoopInfo.semaphore = timelineSemaphore->semaphore;
	signalEventLoopInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	signalEventLoopInfo.value = timelineSemaphore->count;
	signalInfos.push_back(signalEventLoopInfo);

	std::vector<std::shared_ptr<BinarySemaphore>> extraWaitSemaphores;

	const auto handleTextureSemaphores = [&](const std::shared_ptr<PrivateTexture>& privateTexture) {
		if (usesGraphicsQueueOnly(privateTexture)) return;
		uint64_t waitValue;
		std::shared_ptr<BinarySemaphore> extraWaitSema;
		uint64_t signalValue;
		RbxProfiler::DiagnosticScope acquire(RBX_DIAG_TEXTURE_ACQUIRE,0,privateTexture.get(),reinterpret_cast<uintptr_t>(this));
		const auto& sema = privateTexture->acquire(waitValue, extraWaitSema, signalValue);
		acquire.finish();

		if (extraWaitSema) {
			extraWaitSemaphores.push_back(extraWaitSema);
			VkSemaphoreSubmitInfo extraWaitInfo {};
			extraWaitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
			extraWaitInfo.semaphore = extraWaitSema->semaphore;
			extraWaitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			waitInfos.push_back(extraWaitInfo);
		}

		VkSemaphoreSubmitInfo waitInfo {};
		waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
		waitInfo.semaphore = sema.semaphore;
		waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		waitInfo.value = waitValue;
		waitInfos.push_back(waitInfo);

		if (signalValue != 0) {
			VkSemaphoreSubmitInfo signalInfo {};
			signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
			signalInfo.semaphore = sema.semaphore;
			signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			signalInfo.value = signalValue;
			signalInfos.push_back(signalInfo);
		}
	};

	for (const auto& texture: readOnlyTextures) {
		handleTextureSemaphores(std::static_pointer_cast<PrivateTexture>(texture));
	}

	for (size_t i = 0; i < readWriteTextures.size(); ++i) {
		const auto& texture = readWriteTextures[i];
		auto privateTexture = std::static_pointer_cast<PrivateTexture>(texture);
		const auto& presentSema = presentationSemaphores[i];

		if (presentSema) {
			VkSemaphoreSubmitInfo signalInfo {};
			signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
			signalInfo.semaphore = presentSema->semaphore;
			signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			signalInfos.push_back(signalInfo);
		}

		handleTextureSemaphores(privateTexture);
	}

	auto completed = [self, timelineSemaphore, extraWaitSemaphores, presentationSemaphores]() {
		reportCommandTiming(CommandSpan::Completion, self->_submittedAt);
		reportGpuTiming(*self->_privateDevice, self->_timestampPool);
		std::vector<std::shared_ptr<CommandEncoder>> encoders;
		{
			std::unique_lock lock(self->_mutex);
			// Autoreleased Metal wrappers can survive a whole loading burst.
			// Their completed Vulkan commands no longer need to pin driver pools.
			self->recycleCommandPool();
			encoders.swap(self->_commandEncoders);
			self->_completed = true;
			// Completed Metal commands may outlive their GPU work (e.g. autorelease
			// pools on streaming workers). Readers and this callback retain any
			// semaphore still in use; the command no longer needs to pin the pool.
			self->_completionSemaphore.reset();
		}
		// Driver-backed resource destruction can block too. Release completed
		// upload resources off the polling thread, before notifying waiters.
		if (self->_completedHandlers.empty() && encoders.empty()) self->finishCompletedHandlers();
		else scheduleMetalHandlers([self, encoders = std::move(encoders)]() mutable {
			encoders.clear();
			self->finishCompletedHandlers();
		}, true);
	};

	commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
	commandBufferInfo.commandBuffer = _commandBuffer;

	VkSubmitInfo2 info {};
	info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
	info.commandBufferInfoCount = 1;
	info.pCommandBufferInfos = &commandBufferInfo;
	info.signalSemaphoreInfoCount = signalInfos.size();
	info.pSignalSemaphoreInfos = signalInfos.data();
	info.waitSemaphoreInfoCount = waitInfos.size();
	info.pWaitSemaphoreInfos = waitInfos.data();

	// FIXME: we need to check if the queue we're submitting on supports the operations encoded in the command buffer.
	//        the Device constructor tries to choose command queues that support as many operations as possible, but it's possible
	//        that a particular device only supports certain operations on certain queues (e.g. maybe it only supports transfer operations
	//        on an exclusive queue that doesn't support graphics or compute).
	sync.finish();
	_submittedAt = timingStart();
#ifdef DARLING
	// Capture presentation order while submissions still hold the device queue
	// lock. A commit can be delayed after unlocking and present out of order.
	const uint64_t presentSerial = _drawablesToPresent.empty() ? 0 : trackb_next_present_serial();
#endif
	{ RbxProfiler::CpuScope cpuSubmit(RBX_PROF_SUBMIT);
	RbxProfiler::DiagnosticScope submit(RBX_DIAG_QUEUE_SUBMIT,diagnostic.bytes,this,diagnostic.detail);
	if (DynamicVK::vkQueueSubmit2(_privateDevice->graphicsQueue(), 1, &info, VK_NULL_HANDLE) != VK_SUCCESS) {
		// TODO
		abort();
	}
	}
	const auto submitFinished = timingStart();
 phase.change(RBX_PROF_POST_SUBMIT);

	// Submission is scheduled now. Serialize application handlers separately
	// from GPU polling; arm completion afterward to preserve callback ordering.
	if (_scheduledHandlers.empty()) {
		_privateDevice->waitForSemaphore(timelineSemaphore->semaphore, timelineSemaphore->count, std::move(completed));
	} else {
		scheduleMetalHandlers(
			[self, timelineSemaphore, completed = std::move(completed)]() mutable {
				reportCommandTiming(CommandSpan::Scheduled, self->_submittedAt);
				for (const auto& handler : self->_scheduledHandlers) handler(self);
				self->_scheduledHandlers.clear();
				self->_privateDevice->waitForSemaphore(timelineSemaphore->semaphore, timelineSemaphore->count, std::move(completed));
			});
	}

	queueLock.unlock();
	lock.unlock();

	// now that the binary semaphore signals are pending, we can allow them to be used
	for (const auto& texture: readWriteTextures) {
		auto privateTexture = std::static_pointer_cast<PrivateTexture>(texture);
		if (exported(privateTexture))
			privateTexture->endUpdatingPresentationSemaphore();
	}

	// we can now queue drawables for presentation and they'll be synchronized properly
	const auto presentStart = timingStart();
	for (const auto& drawable: _drawablesToPresent) {
#ifdef DARLING
		trackb_present_drawable(drawable, presentSerial);
#else
		drawable->present();
#endif
	}
	if (!_drawablesToPresent.empty()) reportCommandTiming(CommandSpan::PresentEnqueue, presentStart);
	if (commitStart != CommandClock::time_point{} && commandTimingEnabled()) {
		const auto commitFinished = CommandClock::now();
		const auto ms = [](auto duration) { return std::chrono::duration<double,std::milli>(duration).count(); };
		// Capture endpoints before reporting; statistics never extend the measured
		// driver call or hold the queue lock. Counts describe this exact submission.
		reportCommandMetric(CommandSpan::Commit, ms(commitFinished-commitStart));
		reportCommandMetric(CommandSpan::PreSubmit, ms(_submittedAt-commitStart));
		reportCommandMetric(CommandSpan::Submit, ms(submitFinished-_submittedAt));
		reportCommandMetric(CommandSpan::PostSubmit, ms(commitFinished-submitFinished));
		reportCommandMetric(CommandSpan::PreResources, ms(queueStart-commitStart));
		reportCommandMetric(CommandSpan::QueueLock, ms(queueAcquired-queueStart));
		reportCommandMetric(CommandSpan::PreUploads, ms(uploadsFinished-queueAcquired));
		reportCommandMetric(CommandSpan::PreEndEncoding, ms(encodingFinished-uploadsFinished));
		reportCommandMetric(CommandSpan::PreSync, ms(_submittedAt-encodingFinished));
		reportCommandMetric(CommandSpan::SubmitWaits, info.waitSemaphoreInfoCount);
		reportCommandMetric(CommandSpan::SubmitSignals, info.signalSemaphoreInfoCount);
		reportCommandMetric(CommandSpan::SubmitReads, readOnlyTextures.size());
		reportCommandMetric(CommandSpan::SubmitWrites, readWriteTextures.size());
	}
};

void Indium::PrivateCommandBuffer::presentDrawable(std::shared_ptr<Drawable> drawable) {
	std::scoped_lock lock(_mutex);
	if (_committed) {
		// TODO
		abort();
	}
	_drawablesToPresent.push_back(drawable);
};

void Indium::PrivateCommandBuffer::addScheduledHandlerLocked(std::function<void(std::shared_ptr<CommandBuffer>)> handler) {
	_scheduledHandlers.push_back(handler);
};

void Indium::PrivateCommandBuffer::addCompletedHandlerLocked(std::function<void(std::shared_ptr<CommandBuffer>)> handler) {
	_completedHandlers.push_back(handler);
};

void Indium::PrivateCommandBuffer::addScheduledHandler(std::function<void(std::shared_ptr<CommandBuffer>)> handler) {
	std::scoped_lock lock(_mutex);
	if (_committed) {
		// TODO
		abort();
	}
	addScheduledHandlerLocked(handler);
};

void Indium::PrivateCommandBuffer::addCompletedHandler(std::function<void(std::shared_ptr<CommandBuffer>)> handler) {
	std::scoped_lock lock(_mutex);
	if (_committed) {
		// TODO
		abort();
	}
	addCompletedHandlerLocked(handler);
};

void Indium::PrivateCommandBuffer::finishCompletedHandlers() {
	for (const auto& handler : _completedHandlers) handler(shared_from_this());
	_completedHandlers.clear();
	{
		std::scoped_lock lock(_mutex);
		_handlersCompleted = true;
	}
	_completedCondvar.notify_all();
}

void Indium::PrivateCommandBuffer::waitUntilCompleted() {
	RbxProfiler::DiagnosticScope diagnosticWait(RBX_DIAG_COMMAND_WAIT,0,this,0);
 RbxProfiler::CpuScope cpu(RBX_PROF_WAIT);
	const auto start = commandTimingStart();
	std::unique_lock lock(_mutex);

	while (!_handlersCompleted) {
		_completedCondvar.wait(lock);
	}
	lock.unlock();
	reportCommandTiming(CommandSpan::Wait, start);
};

void Indium::PrivateCommandBuffer::waitForGPU() {
	RbxProfiler::DiagnosticScope diagnosticWait(RBX_DIAG_COMMAND_WAIT,0,this,1);
 RbxProfiler::CpuScope cpu(RBX_PROF_READBACK);
	const auto start = commandTimingStart();
	std::shared_ptr<TimelineSemaphore> semaphore;
	{
		std::scoped_lock lock(_mutex);
		if (!_committed) throw std::logic_error("Readback command has not been committed");
		if (_completed) return;
		semaphore = _completionSemaphore;
	}
	VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	wait.semaphoreCount = 1; wait.pSemaphores = &semaphore->semaphore; wait.pValues = &semaphore->count;
	if (DynamicVK::vkWaitSemaphores(_privateDevice->device(), &wait, UINT64_MAX) != VK_SUCCESS)
		throw std::runtime_error("Texture readback wait failed");
	reportCommandTiming(CommandSpan::Readback, start);
}
