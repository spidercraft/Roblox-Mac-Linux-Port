#include "swapchain.hpp"
#include <indium/dynamic-vk.hpp>
#include <indium/instance.private.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {
using namespace Indium;
// A drawable is at most a frame behind; never block a commit longer waiting
// for the compositor to release an image, drop the frame instead.
constexpr uint64_t acquireTimeoutNs = 100'000'000;

template<class T, class F> std::vector<T> enumerate(F query) {
	uint32_t count = 0;
	std::vector<T> values;
	if (query(&count, nullptr) != VK_SUCCESS) return values;
	values.resize(count);
	if (query(&count, values.data()) != VK_SUCCESS) values.clear();
	values.resize(count);
	return values;
}
}

Indium::SwapchainPresenter::SwapchainPresenter(std::shared_ptr<PrivateDevice> device, VkSurfaceKHR surface):
	_device(std::move(device)),
	_surface(surface)
{
	VkBool32 supported = VK_FALSE;
	const uint32_t family = *_device->graphicsQueueFamilyIndex();
	if (DynamicVK::vkGetPhysicalDeviceSurfaceSupportKHR(_device->physicalDevice(), family, _surface, &supported) != VK_SUCCESS || !supported) {
		DynamicVK::vkDestroySurfaceKHR(globalInstance, _surface, nullptr);
		throw std::runtime_error("The graphics queue cannot present to this surface");
	}
	_sharedQueue = !_device->presentQueue;
	_queue = _sharedQueue ? _device->graphicsQueue() : _device->presentQueue;
	_copies = _device->getTimelineSemaphore();
	_submitted = _copies.count;
	for (auto& frame : _frames) {
		VkCommandPoolCreateInfo pool {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		pool.queueFamilyIndex = family;
		VkCommandBufferAllocateInfo commands {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commands.commandBufferCount = 1;
		VkSemaphoreCreateInfo semaphore {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		if (DynamicVK::vkCreateCommandPool(_device->device(), &pool, nullptr, &frame.pool) != VK_SUCCESS ||
		    (commands.commandPool = frame.pool, DynamicVK::vkAllocateCommandBuffers(_device->device(), &commands, &frame.commands)) != VK_SUCCESS ||
		    DynamicVK::vkCreateSemaphore(_device->device(), &semaphore, nullptr, &frame.acquired) != VK_SUCCESS) {
			this->~SwapchainPresenter();
			throw std::runtime_error("Could not create presentation resources");
		}
		frame.done = _submitted;
	}
}

Indium::SwapchainPresenter::~SwapchainPresenter() {
	waitIdle();
	const auto device = _device->device();
	for (auto semaphore : _rendered) DynamicVK::vkDestroySemaphore(device, semaphore, nullptr);
	for (auto& frame : _frames) {
		if (frame.acquired) DynamicVK::vkDestroySemaphore(device, frame.acquired, nullptr);
		if (frame.pool) DynamicVK::vkDestroyCommandPool(device, frame.pool, nullptr);
		frame = {};
	}
	_rendered.clear();
	if (_swapchain) DynamicVK::vkDestroySwapchainKHR(device, _swapchain, nullptr);
	_swapchain = VK_NULL_HANDLE;
	if (_surface) DynamicVK::vkDestroySurfaceKHR(globalInstance, _surface, nullptr);
	_surface = VK_NULL_HANDLE;
	if (_copies.semaphore) _device->putTimelineSemaphore(_copies);
	_copies.semaphore = VK_NULL_HANDLE;
}

void Indium::SwapchainPresenter::waitForCopies(uint64_t value) {
	VkSemaphoreWaitInfo wait {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	wait.semaphoreCount = 1;
	wait.pSemaphores = &_copies.semaphore;
	wait.pValues = &value;
	DynamicVK::vkWaitSemaphores(_device->device(), &wait, UINT64_MAX);
}

void Indium::SwapchainPresenter::waitIdle() {
	if (!_copies.semaphore) return;
	waitForCopies(_submitted);
	// Presents also wait on semaphores that are about to be destroyed.
	std::scoped_lock lock(queueSubmissionMutex(_queue));
	DynamicVK::vkQueueWaitIdle(_queue);
}

bool Indium::SwapchainPresenter::recreate(VkExtent2D extent, bool vsync) {
	const auto device = _device->device();
	const auto physical = _device->physicalDevice();
	VkSurfaceCapabilitiesKHR capabilities {};
	if (DynamicVK::vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, _surface, &capabilities) != VK_SUCCESS) return false;
	if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;
	const auto formats = enumerate<VkSurfaceFormatKHR>([&](uint32_t* count, VkSurfaceFormatKHR* values) {
		return DynamicVK::vkGetPhysicalDeviceSurfaceFormatsKHR(physical, _surface, count, values);
	});
	const auto modes = enumerate<VkPresentModeKHR>([&](uint32_t* count, VkPresentModeKHR* values) {
		return DynamicVK::vkGetPhysicalDeviceSurfacePresentModesKHR(physical, _surface, count, values);
	});
	if (formats.empty() || modes.empty()) return false;
	// Same bytes the GL path showed: an 8-bit UNORM image in sRGB space.
	VkSurfaceFormatKHR format = formats[0];
	for (const auto wanted : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
		const auto found = std::find_if(formats.begin(), formats.end(), [&](const auto& candidate) {
			return candidate.format == wanted && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		});
		if (found != formats.end()) { format = *found; break; }
	}
	auto has = [&](VkPresentModeKHR mode) { return std::find(modes.begin(), modes.end(), mode) != modes.end(); };
	// Without vsync: newest frame at each refresh, no tearing (mailbox).
	VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
	if (!vsync) mode = has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR : has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
	// Wayland reports no current extent; the swapchain decides the surface size.
	if (capabilities.currentExtent.width != UINT32_MAX) extent = capabilities.currentExtent;
	extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
	extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
	if (!extent.width || !extent.height) return false;
	uint32_t images = std::max(capabilities.minImageCount + 1, 3u);
	if (capabilities.maxImageCount) images = std::min(images, capabilities.maxImageCount);
	VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	// The last GL frame stays mapped underneath; it must never bleed through.
	if (!(capabilities.supportedCompositeAlpha & alpha)) return false;

	// Every copy and present using the old images has finished.
	waitIdle();
	VkSwapchainCreateInfoKHR info {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	info.surface = _surface;
	info.minImageCount = images;
	info.imageFormat = format.format;
	info.imageColorSpace = format.colorSpace;
	info.imageExtent = extent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = (capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : capabilities.currentTransform;
	info.compositeAlpha = alpha;
	info.presentMode = mode;
	info.clipped = VK_TRUE;
	info.oldSwapchain = _swapchain;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	const auto created = DynamicVK::vkCreateSwapchainKHR(device, &info, nullptr, &swapchain);
	for (auto semaphore : _rendered) DynamicVK::vkDestroySemaphore(device, semaphore, nullptr);
	_rendered.clear();
	_images.clear();
	if (_swapchain) DynamicVK::vkDestroySwapchainKHR(device, _swapchain, nullptr);
	_swapchain = created == VK_SUCCESS ? swapchain : VK_NULL_HANDLE;
	if (!_swapchain) return false;
	_images = enumerate<VkImage>([&](uint32_t* count, VkImage* values) {
		return DynamicVK::vkGetSwapchainImagesKHR(device, _swapchain, count, values);
	});
	for (size_t i = 0; i < _images.size(); ++i) {
		VkSemaphoreCreateInfo semaphore {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		VkSemaphore rendered = VK_NULL_HANDLE;
		if (DynamicVK::vkCreateSemaphore(device, &semaphore, nullptr, &rendered) != VK_SUCCESS) return false;
		_rendered.push_back(rendered);
	}
	_extent = info.imageExtent;
	_format = format.format;
	_mode = mode;
	_vsync = vsync;
	_stale = false;
	if (std::getenv("RBX_PRESENT_RESOURCE_TRACE"))
		std::fprintf(stderr, "PRESENT swapchain %ux%u images=%zu mode=%d format=%d\n", _extent.width, _extent.height, _images.size(), int(_mode), int(_format));
	return !_images.empty();
}

Indium::SwapchainPresenter::Result Indium::SwapchainPresenter::present(PrivateTexture& texture, VkExtent2D extent, bool vsync, std::function<void()> released) {
	std::lock_guard lock(_mutex);
	const auto device = _device->device();
	// A resized window takes the swapchain's extent; a compositor-requested
	// change (suboptimal/out of date) takes effect on the next frame.
	if (!_swapchain || _stale || extent.width != _extent.width || extent.height != _extent.height || vsync != _vsync)
		if (!recreate(extent, vsync)) return Result::Failed;
	auto& frame = _frames[_next++ % _frames.size()];
	waitForCopies(frame.done);
	uint32_t index = 0;
	const auto acquired = DynamicVK::vkAcquireNextImageKHR(device, _swapchain, acquireTimeoutNs, frame.acquired, VK_NULL_HANDLE, &index);
	if (acquired == VK_ERROR_OUT_OF_DATE_KHR) { _stale = true; return Result::Dropped; }
	if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY) return Result::Dropped;
	if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return Result::Failed;
	if (acquired == VK_SUBOPTIMAL_KHR) _stale = true;

	DynamicVK::vkResetCommandPool(device, frame.pool, 0);
	VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	DynamicVK::vkBeginCommandBuffer(frame.commands, &begin);
	VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = _images[index];
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	DynamicVK::vkCmdPipelineBarrier(frame.commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	// The texture's writers finished before the semaphore wait below; the GL
	// path's texture sampling was nearest too.
	VkImageBlit region {};
	region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.srcOffsets[1] = {int32_t(texture.width()), int32_t(texture.height()), 1};
	region.dstOffsets[1] = {int32_t(_extent.width), int32_t(_extent.height), 1};
	DynamicVK::vkCmdBlitImage(frame.commands, texture.image(), texture.imageLayout(), _images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = 0;
	DynamicVK::vkCmdPipelineBarrier(frame.commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	DynamicVK::vkEndCommandBuffer(frame.commands);

	// Same ordering every Indium command gets for this texture: after its
	// last user, and before its next one (the next frame rendering into it).
	uint64_t waitValue = 0, signalValue = 0;
	std::shared_ptr<BinarySemaphore> extraWait;
	const auto& textureSync = texture.acquire(waitValue, extraWait, signalValue);
	std::vector<VkSemaphoreSubmitInfo> waits, signals;
	auto semaphore = [](VkSemaphore handle, uint64_t value) {
		VkSemaphoreSubmitInfo info {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
		info.semaphore = handle;
		info.value = value;
		info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		return info;
	};
	waits.push_back(semaphore(frame.acquired, 0));
	waits.push_back(semaphore(textureSync.semaphore, waitValue));
	if (extraWait) waits.push_back(semaphore(extraWait->semaphore, 0));
	signals.push_back(semaphore(_rendered[index], 0));
	if (signalValue) signals.push_back(semaphore(textureSync.semaphore, signalValue));
	signals.push_back(semaphore(_copies.semaphore, ++_submitted));
	VkCommandBufferSubmitInfo commandInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
	commandInfo.commandBuffer = frame.commands;
	VkSubmitInfo2 submit {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
	submit.waitSemaphoreInfoCount = waits.size();
	submit.pWaitSemaphoreInfos = waits.data();
	submit.commandBufferInfoCount = 1;
	submit.pCommandBufferInfos = &commandInfo;
	submit.signalSemaphoreInfoCount = signals.size();
	submit.pSignalSemaphoreInfos = signals.data();
	// Indium serializes submissions only against other users of this queue.
	if (DynamicVK::vkQueueSubmit2(_queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
		--_submitted;
		return Result::Failed;
	}
	frame.done = _submitted;
	// Completion releases the drawable even if the present itself fails below.
	_device->waitForSemaphore(_copies.semaphore, _submitted, std::move(released));

	VkPresentInfoKHR present {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &_rendered[index];
	present.swapchainCount = 1;
	present.pSwapchains = &_swapchain;
	present.pImageIndices = &index;
	VkResult presented;
	{
		std::scoped_lock queueLock(queueSubmissionMutex(_queue));
		presented = DynamicVK::vkQueuePresentKHR(_queue, &present);
	}
	if (presented == VK_SUBOPTIMAL_KHR || presented == VK_ERROR_OUT_OF_DATE_KHR) _stale = true;
	else if (presented != VK_SUCCESS) return Result::Lost;
	return Result::Presented;
}
