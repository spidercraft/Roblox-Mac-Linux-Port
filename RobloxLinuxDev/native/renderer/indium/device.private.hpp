#pragma once

#include "indium/base.hpp"
#include <cstdint>
#include <type_traits>
#include <vulkan/vulkan.h>

#include <indium/device.hpp>
#include <indium/types.private.hpp>

#include <vector>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <optional>
#include <chrono>
#include <string>

namespace Indium {
	class PrivateDevice;
	class UploadQueue;
	struct DescriptorArena;
	struct RenderTargetCacheEntry;
	struct BufferMemoryBlock;
	struct ImageMemoryBlock;
	class GraphicsLibraries;

	extern std::vector<std::shared_ptr<PrivateDevice>> globalDeviceList;

	void initGlobalDeviceList();
	void finitGlobalDeviceList();
	// Held around every vkQueueSubmit driver call, including prebuilt callers';
	// anything else that uses those queues (vkQueuePresentKHR) must hold it too.
	std::mutex& queueSubmissionMutex(VkQueue queue);

	class PrivateDevice: public Device, public std::enable_shared_from_this<PrivateDevice> {
	private:

		std::mutex _eventLoopMutex;
		std::mutex _pollingMutex;

		// these are stored separately (rather than in a single structure)
		// so that we can easily copy them individually in the event loop polling function.
		// index 0 is reserved for the wakeup semaphore.
		std::vector<VkSemaphore> _eventLoopSemaphores;
		std::vector<uint64_t> _eventLoopWaitValues;
		std::vector<std::function<void()>> _eventLoopCallbacks;

	public:
		PrivateDevice(VkPhysicalDevice physicalDevice);
		~PrivateDevice();

		enum class Feature: uint64_t {
			Swapchain           = 1 << 0,
			ExternalMemoryFD    = 1 << 1,
			ExternalSemaphoreFD = 1 << 2,
			NonSemanticInfo     = 1 << 3,
		};

		friend inline Feature operator|(Feature lhs, Feature rhs) {
			return static_cast<Feature>(static_cast<std::underlying_type_t<Feature>>(lhs) | static_cast<std::underlying_type_t<Feature>>(rhs));
		}

		friend inline Feature operator&(Feature lhs, Feature rhs) {
			return static_cast<Feature>(static_cast<std::underlying_type_t<Feature>>(lhs) & static_cast<std::underlying_type_t<Feature>>(rhs));
		}

		friend inline bool operator!(Feature features) {
			return features == static_cast<Feature>(0);
		}

		virtual std::string name() const override;
		virtual std::shared_ptr<CommandQueue> newCommandQueue() override;
		virtual std::shared_ptr<RenderPipelineState> newRenderPipelineState(const RenderPipelineDescriptor& descriptor) override;
		virtual std::shared_ptr<ComputePipelineState> newComputePipelineState(const ComputePipelineDescriptor& descriptor, PipelineOption options, std::shared_ptr<ComputePipelineReflection> reflection) override;
		virtual std::shared_ptr<ComputePipelineState> newComputePipelineState(std::shared_ptr<Function> computeFunction, PipelineOption options = PipelineOption::None, std::shared_ptr<ComputePipelineReflection> reflection = nullptr) override;
		virtual std::shared_ptr<Buffer> newBuffer(size_t length, ResourceOptions options) override;
		virtual std::shared_ptr<Buffer> newBuffer(const void* pointer, size_t length, ResourceOptions options) override;
		virtual std::shared_ptr<Library> newLibrary(const void* data, size_t length) override;
		virtual std::shared_ptr<Texture> newTexture(const TextureDescriptor& descriptor) override;
		virtual std::shared_ptr<SamplerState> newSamplerState(const SamplerDescriptor& descriptor) override;
		virtual std::shared_ptr<DepthStencilState> newDepthStencilState(const DepthStencilDescriptor& descriptor) override;

		virtual void pollEvents(uint64_t timeoutNanoseconds) override;
		virtual void wakeupEventLoop() override;

		void waitForSemaphore(VkSemaphore semaphore, uint64_t targetValue, std::function<void()> callback);

		/**
		 * @note This method MAY return a previously-used semaphore, so the count may not
		 *       always be 0. Be sure to use the count value of the returned structure.
		 */
		TimelineSemaphore getTimelineSemaphore();
		void putTimelineSemaphore(const TimelineSemaphore& semaphore);

		/**
		 * Similar to getTimelineSemaphore(), but wraps it in a shared pointer
		 * that automatically releases the semaphore when all references to it die out.
		 */
		std::shared_ptr<TimelineSemaphore> getWrappedTimelineSemaphore();
		// Non-exported timelines; retain their current values across reuse.
		std::shared_ptr<TimelineSemaphore> getCompletionSemaphore();

		BinarySemaphore getBinarySemaphore(bool exportable = false);
		void putBinarySemaphore(const BinarySemaphore& semaphore);

		std::shared_ptr<BinarySemaphore> getWrappedBinarySemaphore(bool exportable = false);

		INDIUM_PROPERTY(VkPhysicalDevice, p, P,hysicalDevice) = VK_NULL_HANDLE;
		INDIUM_PROPERTY(VkPhysicalDeviceProperties, p, P,roperties);
		INDIUM_PROPERTY(VkDevice, d, D,evice) = VK_NULL_HANDLE;
		INDIUM_PROPERTY(std::optional<uint32_t>, g, G,raphicsQueueFamilyIndex);
		INDIUM_PROPERTY(std::optional<uint32_t>, c, C,omputeQueueFamilyIndex);
		INDIUM_PROPERTY(std::optional<uint32_t>, t, T,ransferQueueFamilyIndex);
		// in theory, the presentation queue *can* be different from the graphics queue,
		// but in practice, they're actually the same.
		// TODO: maybe implement this just in case?
		//INDIUM_PROPERTY(std::optional<uint32_t>, p, P,resentQueueFamilyIndex);
		INDIUM_PROPERTY(VkQueue, g, G,raphicsQueue) = VK_NULL_HANDLE;
		INDIUM_PROPERTY(VkQueue, c, C,omputeQueue) = VK_NULL_HANDLE;
		INDIUM_PROPERTY(VkQueue, t, T,ransferQueue) = VK_NULL_HANDLE;
		//INDIUM_PROPERTY(VkQueue, p, P,resentQueue) = VK_NULL_HANDLE;
		INDIUM_PROPERTY(VkCommandPool, o,O,neshotCommandPool) = VK_NULL_HANDLE;

		INDIUM_PROPERTY(VkPhysicalDeviceMemoryProperties, m, M,emoryProperties);
		INDIUM_PROPERTY_READONLY(Feature, f, F,eatures);
		public:
		// Append state to preserve offsets used by the prebuilt framework.
		std::mutex queueMutex, oneshotMutex;
		std::mutex recycledCommandMutex;
		std::vector<std::pair<VkCommandPool,VkCommandBuffer>> recycledCommands;
		std::shared_ptr<UploadQueue> uploads;
		// queueMutex protects the upload recorder and all submissions.
		UploadQueue& uploadQueue();
		std::mutex rendererCacheMutex;
		std::vector<std::shared_ptr<DescriptorArena>> descriptorArenas;
		std::vector<std::shared_ptr<RenderTargetCacheEntry>> renderTargets;
		uint32_t graphicsTimestampBits = 0;
		std::vector<VkQueryPool> timestampPools;
		// Raw idle handles avoid a Device ownership cycle. recycledCommandMutex
		// protects this pool; live shared owners retain the Device as before.
		std::vector<std::pair<VkSemaphore,uint64_t>> recycledCompletionSemaphores;
		std::mutex bufferMemoryMutex;
		std::vector<std::shared_ptr<BufferMemoryBlock>> bufferMemoryBlocks;
		// Appended after the buffer pool so existing field offsets are unchanged
		// for the prebuilt Metal framework.
		std::mutex imageMemoryMutex;
		std::vector<std::shared_ptr<ImageMemoryBlock>> imageMemoryBlocks;
		// Optional Vulkan paths, appended like the fields above.
		// Graphics descriptor sets: 0 = buffers of both stages (pushed when
		// pushDescriptors), 1 = other vertex resources, 2 = other fragment resources.
		bool pushDescriptors = false;   // VK_KHR_push_descriptor
		bool dynamicDepthClamp = false; // VK_EXT_extended_dynamic_state3 depthClampEnable
		bool pipelineLibraries = false; // VK_EXT_graphics_pipeline_library with fast linking
		uint32_t maxPushDescriptors = 0;
		VkDescriptorSetLayout bufferSetLayout = VK_NULL_HANDLE; // set 0 of every graphics layout
		VkPipelineCache pipelineCache = VK_NULL_HANDLE;
		// Writes the pipeline cache to disk; without force, at most every few seconds.
		void savePipelineCache(bool force = false);
		std::shared_ptr<GraphicsLibraries> graphicsLibraries;
		// A second queue of the graphics family, used only by the swapchain
		// presenter so a blocking present never holds up other submissions.
		VkQueue presentQueue = VK_NULL_HANDLE;
		std::atomic<bool> vulkanPresentation{false};
	private:
		std::mutex _pipelineCacheMutex;
		std::string _pipelineCachePath;
		std::chrono::steady_clock::time_point _pipelineCacheSaved;
		// Armed under _eventLoopMutex only while the idle poll sleeps.
		std::atomic<bool> _eventLoopWaiting{false};
		std::condition_variable _eventLoopIdle; // Appended to preserve framework offsets.
		bool _eventLoopWakePending = false; // Protected by _eventLoopMutex.
	};
};
