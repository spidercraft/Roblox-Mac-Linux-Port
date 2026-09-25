#include <cstdio>
#include <indium/device.private.hpp>
#include <indium/uploads.hpp>
#include <indium/gpu-timing.hpp>
#include <indium/instance.private.hpp>
#include <indium/command-queue.private.hpp>
#include <indium/render-pipeline.private.hpp>
#include <indium/buffer.private.hpp>
#include <indium/library.private.hpp>
#include <indium/sampler.private.hpp>
#include <indium/texture.private.hpp>
#include <indium/depth-stencil.private.hpp>
#include <indium/compute-pipeline.private.hpp>
#include <indium/dynamic-vk.hpp>
#include <indium/graphics-libraries.hpp>

#include <iridium/iridium.hpp>

#include <set>
#include <map>
#include <stdexcept>
#include <thread>
#include <unordered_set>

#include <cstdlib>
#include <cstring>
#include <vector>
#include <atomic>

namespace {
// The captures submit up to 124 geometry copies plus rendering in one burst.
// ponytail: reserve 128 completions at device setup; larger backlogs keep the
// existing allocation fallback, and only 128 idle handles remain cached.
constexpr size_t completionSemaphoreCapacity = 128;
void semaphoreLifetimeDiagnostic(bool binary, bool created, bool exportable = false) {
	if (!std::getenv("RBX_PRESENT_RESOURCE_TRACE")) return;
	static std::atomic<uint64_t> made[2]{}, destroyed[2]{}, exported{0};
	auto count = created ? ++made[binary] : ++destroyed[binary];
	if (exportable) ++exported;
	if (count == 1 || count % 256 == 0)
		std::fprintf(stderr, "PRESENT Vulkan semaphores binary-created=%llu binary-destroyed=%llu exportable-created=%llu timeline-created=%llu timeline-destroyed=%llu\n",
			(unsigned long long)made[1].load(), (unsigned long long)destroyed[1].load(),
			(unsigned long long)exported.load(), (unsigned long long)made[0].load(), (unsigned long long)destroyed[0].load());
}
// Prebuilt Cocotron also calls these DynamicVK entry points when initializing
// drawables, outside Track B's queueMutex. Serialize the actual driver calls too.
// Different queues must remain independent when presentation stalls.
PFN_vkQueueSubmit originalSubmit;
PFN_vkQueueSubmit2 originalSubmit2;
VkResult synchronizedSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* infos, VkFence fence) {
	std::scoped_lock lock(Indium::queueSubmissionMutex(queue));
	return originalSubmit(queue, count, infos, fence);
}
VkResult synchronizedSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* infos, VkFence fence) {
	std::scoped_lock lock(Indium::queueSubmissionMutex(queue));
	return originalSubmit2(queue, count, infos, fence);
}
// Optional device extensions that have no public Indium feature bit.
bool hasExtension(const std::vector<VkExtensionProperties>& available, const char* name) {
	for (const auto& extension : available) if (strcmp(extension.extensionName, name) == 0) return true;
	return false;
}

// Pipeline cache file: ROBLOX_MAC_PIPELINE_CACHE, else next to the shader cache.
std::string pipelineCachePath() {
	if (const char* path = std::getenv("ROBLOX_MAC_PIPELINE_CACHE")) return path;
	const char* shaders = std::getenv("ROBLOX_MAC_SHADER_CACHE");
	if (!shaders || !*shaders) return {};
	std::string directory = shaders;
	while (directory.size() > 1 && directory.back() == '/') directory.pop_back();
	const auto slash = directory.rfind('/');
	if (slash == std::string::npos) return {};
	return directory.substr(0, slash + 1) + "vk-pipeline-cache-v1.bin";
}

// Creates the device's pipeline cache from a previous session's file when
// that file was written by this exact device and driver.
void loadPipelineCache(Indium::PrivateDevice& device, std::string& path) {
	path = pipelineCachePath();
	std::vector<char> data;
	if (!path.empty()) if (FILE* file = std::fopen(path.c_str(), "rb")) {
		char chunk[1 << 16];
		for (size_t read; (read = std::fread(chunk, 1, sizeof(chunk), file)) > 0;) data.insert(data.end(), chunk, chunk + read);
		std::fclose(file);
	}
	const auto& properties = device.properties();
	VkPipelineCacheHeaderVersionOne header {};
	if (data.size() >= sizeof(header)) std::memcpy(&header, data.data(), sizeof(header));
	if (data.size() < sizeof(header) || header.headerSize < sizeof(header) || header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
	    header.vendorID != properties.vendorID || header.deviceID != properties.deviceID ||
	    std::memcmp(header.pipelineCacheUUID, properties.pipelineCacheUUID, VK_UUID_SIZE)) data.clear();
	VkPipelineCacheCreateInfo info {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
	info.initialDataSize = data.size();
	info.pInitialData = data.empty() ? nullptr : data.data();
	if (Indium::DynamicVK::vkCreatePipelineCache(device.device(), &info, nullptr, &device.pipelineCache) != VK_SUCCESS) {
		info.initialDataSize = 0;
		info.pInitialData = nullptr;
		if (Indium::DynamicVK::vkCreatePipelineCache(device.device(), &info, nullptr, &device.pipelineCache) != VK_SUCCESS)
			device.pipelineCache = VK_NULL_HANDLE;
	}
}
void synchronizeSubmissionEntryPoints() {
	static std::once_flag once;
	std::call_once(once, [] {
		auto& submit = Indium::DynamicVK::vkQueueSubmit;
		auto& submit2 = Indium::DynamicVK::vkQueueSubmit2;
		if (!submit.resolve() || !submit2.resolve()) std::abort();
		originalSubmit = reinterpret_cast<PFN_vkQueueSubmit>(submit.pointer);
		originalSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>(submit2.pointer);
		submit.pointer = reinterpret_cast<void*>(&synchronizedSubmit);
		submit2.pointer = reinterpret_cast<void*>(&synchronizedSubmit2);
	});
}
}

std::vector<std::shared_ptr<Indium::PrivateDevice>> Indium::globalDeviceList;

std::mutex& Indium::queueSubmissionMutex(VkQueue queue) {
	static std::mutex registryMutex;
	static std::map<VkQueue, std::mutex> queues;
	std::scoped_lock lock(registryMutex);
	return queues[queue];
}

void Indium::initGlobalDeviceList() {
	std::vector<VkPhysicalDevice> physicalDevices;
	uint32_t count = 0;

	auto result = DynamicVK::vkEnumeratePhysicalDevices(globalInstance, &count, nullptr);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
		// TODO: maybe warn?
		return;
	}

	physicalDevices.resize(count);
	result = DynamicVK::vkEnumeratePhysicalDevices(globalInstance, &count, physicalDevices.data());
	if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
		// TODO: maybe warn?
		return;
	}

	for (auto&& device: physicalDevices) {
		VkPhysicalDeviceProperties props;
		DynamicVK::vkGetPhysicalDeviceProperties(device, &props);

		if (VK_API_VERSION_VARIANT(props.apiVersion) != 0 || props.apiVersion < VK_API_VERSION_1_3) {
			// unsupported device
			continue;
		}

		VkPhysicalDeviceFeatures2 features {};
		VkPhysicalDeviceVulkan11Features features11 {};
		VkPhysicalDeviceVulkan12Features features12 {};
		VkPhysicalDeviceVulkan13Features features13 {};

		features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		features.pNext = &features11;

		features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
		features11.pNext = &features12;

		features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		features12.pNext = &features13;

		features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

		DynamicVK::vkGetPhysicalDeviceFeatures2(device, &features);

		if (!features12.timelineSemaphore) {
			// unsupported device
			continue;
		}

		globalDeviceList.push_back(std::make_shared<PrivateDevice>(std::move(device)));
	}
};

void Indium::finitGlobalDeviceList() {
	globalDeviceList.clear();
};

Indium::Device::~Device() {};

Indium::PrivateDevice::PrivateDevice(VkPhysicalDevice physicalDevice):
	_physicalDevice(physicalDevice)
{
	DynamicVK::vkGetPhysicalDeviceProperties(_physicalDevice, &_properties);

	std::vector<VkQueueFamilyProperties> queueFamilies;
	uint32_t count = 0;

	DynamicVK::vkGetPhysicalDeviceQueueFamilyProperties(_physicalDevice, &count, nullptr);
	queueFamilies.resize(count);
	DynamicVK::vkGetPhysicalDeviceQueueFamilyProperties(_physicalDevice, &count, queueFamilies.data());

	DynamicVK::vkGetPhysicalDeviceMemoryProperties(_physicalDevice, &_memoryProperties);

	uint32_t index = 0;
	size_t maxSupportedSameQueueFamily = 0;
	for (const auto& queueFamily: queueFamilies) {
		bool supportsGraphics = false;
		bool supportsCompute = false;
		bool supportsTransfer = false;
		bool supportsPresent = false;
		size_t supported = 0;

		if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			supportsGraphics = true;
			++supported;
		}

		if (queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT) {
			supportsCompute = true;
			++supported;
		}

		if (queueFamily.queueFlags & VK_QUEUE_TRANSFER_BIT) {
			supportsTransfer = true;
			++supported;
		}

		if (supportsGraphics && !_graphicsQueueFamilyIndex) {
			_graphicsQueueFamilyIndex = index;
		}

		if (supportsCompute && !_computeQueueFamilyIndex) {
			_computeQueueFamilyIndex = index;
		}

		if (supportsTransfer && !_transferQueueFamilyIndex) {
			_transferQueueFamilyIndex = index;
		}

		// we want to use the same queue family as much as possible to conserve resources,
		// so if this queue family supports more operations than the currently saved queue families,
		// we want to use this queue family instead of those.

		if (supported > maxSupportedSameQueueFamily) {
			maxSupportedSameQueueFamily = supported;

			if (supportsGraphics) {
				_graphicsQueueFamilyIndex = index;
			}

			if (supportsCompute) {
				_computeQueueFamilyIndex = index;
			}

			if (supportsTransfer) {
				_transferQueueFamilyIndex = index;
			}
		}

		if (supported == 3) {
			// we've found the best queue family: one that supports everything.
			// we can stop looking now.
			break;
		}

		++index;
	}

	// TODO: we should try to ensure that the same queue is used for graphics and compute
	//       to make it to create CommandBuffers that can do either one.

	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	std::set<uint32_t> queueFamilyIndices;
	std::vector<float> queuePriorities = { 1.0f, 1.0f };

	if (_graphicsQueueFamilyIndex) {
		queueFamilyIndices.insert(*_graphicsQueueFamilyIndex);
	}

	if (_computeQueueFamilyIndex) {
		queueFamilyIndices.insert(*_computeQueueFamilyIndex);
	}

	if (_transferQueueFamilyIndex) {
		queueFamilyIndices.insert(*_transferQueueFamilyIndex);
	}

	// The graphics family gets a second queue for the swapchain presenter.
	const bool separatePresentQueue = _graphicsQueueFamilyIndex && queueFamilies[*_graphicsQueueFamilyIndex].queueCount >= 2;
	for (const auto& index: queueFamilyIndices) {
		VkDeviceQueueCreateInfo createInfo {};
		createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		createInfo.queueFamilyIndex = index;
		createInfo.pQueuePriorities = queuePriorities.data();
		createInfo.queueCount = separatePresentQueue && index == *_graphicsQueueFamilyIndex ? 2 : 1;
		queueCreateInfos.push_back(createInfo);
	}

	std::vector<VkExtensionProperties> extProps;
	DynamicVK::vkEnumerateDeviceExtensionProperties(_physicalDevice, nullptr, &count, nullptr);
	extProps.resize(count);
	DynamicVK::vkEnumerateDeviceExtensionProperties(_physicalDevice, nullptr, &count, extProps.data());

	VkPhysicalDeviceFeatures2 features {};
	VkPhysicalDeviceVulkan11Features features11 {};
	VkPhysicalDeviceVulkan12Features features12 {};
	VkPhysicalDeviceVulkan13Features features13 {};
	VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamicState3 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
	VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT libraryFeatures {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};

	features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features.pNext = &features11;

	features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
	features11.pNext = &features12;

	features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	features12.pNext = &features13;

	features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

	// Extension feature structs are chained only when the extension exists.
	void** chain = &features13.pNext;
	const bool hasDynamicState3 = hasExtension(extProps, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
	const bool hasLibraries = hasExtension(extProps, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) && hasExtension(extProps, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
	if (hasDynamicState3) { *chain = &dynamicState3; chain = &dynamicState3.pNext; }
	if (hasLibraries) { *chain = &libraryFeatures; chain = &libraryFeatures.pNext; }

	DynamicVK::vkGetPhysicalDeviceFeatures2(_physicalDevice, &features);

	// Only the dynamic state actually used is enabled; the rest stay off.
	const VkBool32 depthClampState = dynamicState3.extendedDynamicState3DepthClampEnable && features.features.depthClamp;
	dynamicState3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, dynamicState3.pNext};
	dynamicState3.extendedDynamicState3DepthClampEnable = depthClampState;

	VkPhysicalDevicePushDescriptorPropertiesKHR pushProperties {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
	VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT libraryProperties {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
	VkPhysicalDeviceProperties2 properties2 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	const bool hasPush = hasExtension(extProps, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
	properties2.pNext = &pushProperties;
	pushProperties.pNext = hasLibraries ? &libraryProperties : nullptr;
	DynamicVK::vkGetPhysicalDeviceProperties2(_physicalDevice, &properties2);

	std::vector<const char*> extensions {
		// put required extensions here
	};
	// Set 0 must hold 16 buffers per graphics stage.
	pushDescriptors = hasPush && pushProperties.maxPushDescriptors >= 2 * 16;
	maxPushDescriptors = pushDescriptors ? pushProperties.maxPushDescriptors : 0;
	if (pushDescriptors) extensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
	dynamicDepthClamp = hasDynamicState3 && depthClampState;
	if (hasDynamicState3) extensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
	// Libraries only pay off with fast linking; otherwise linking is a full compile.
	pipelineLibraries = hasLibraries && libraryFeatures.graphicsPipelineLibrary && libraryProperties.graphicsPipelineLibraryFastLinking &&
		!std::getenv("RBX_NO_PIPELINE_LIBRARIES");
	if (hasLibraries) {
		extensions.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
		extensions.push_back(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
		libraryFeatures.graphicsPipelineLibrary = pipelineLibraries;
	}
	auto indiumFeatures = static_cast<Feature>(0);

	std::vector<std::pair<const char*, Feature>> optionalExtensions {
		// put optional extensions here
		{ VK_KHR_SWAPCHAIN_EXTENSION_NAME, Feature::Swapchain },
		{ VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, Feature::ExternalMemoryFD },
		{ VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, Feature::ExternalSemaphoreFD },
		{ VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME, Feature::NonSemanticInfo },
	};

	for (const auto& prop: extProps) {
		for (const auto& [name, feature]: optionalExtensions) {
			if (strcmp(prop.extensionName, name) == 0) {
				extensions.push_back(name);
				indiumFeatures = indiumFeatures | feature;
			}
		}
	}

	VkDeviceCreateInfo deviceCreateInfo {};
	deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
	deviceCreateInfo.queueCreateInfoCount = queueCreateInfos.size();
	deviceCreateInfo.pNext = &features; // enable all features
	deviceCreateInfo.enabledExtensionCount = extensions.size();
	deviceCreateInfo.ppEnabledExtensionNames = extensions.data();
	auto deviceResult = DynamicVK::vkCreateDevice(_physicalDevice, &deviceCreateInfo, nullptr, &_device);
	if (deviceResult != VK_SUCCESS) {
		fprintf(stderr, "Indium: vkCreateDevice failed (%d)\n", int(deviceResult));
		abort();
	}

	_features = indiumFeatures;

	for (const auto& index: queueFamilyIndices) {
		VkQueue queue;
		DynamicVK::vkGetDeviceQueue(_device, index, 0, &queue);

		if (_graphicsQueueFamilyIndex && *_graphicsQueueFamilyIndex == index) {
			_graphicsQueue = queue;
		}

		if (_computeQueueFamilyIndex && *_computeQueueFamilyIndex == index) {
			_computeQueue = queue;
		}

		if (_transferQueueFamilyIndex && *_transferQueueFamilyIndex == index) {
			_transferQueue = queue;
		}
	}
	if (separatePresentQueue) DynamicVK::vkGetDeviceQueue(_device, *_graphicsQueueFamilyIndex, 1, &presentQueue);

	// Set 0 of every graphics pipeline layout: the buffers of both stages.
	{
		std::vector<VkDescriptorSetLayoutBinding> bindings;
		for (uint32_t i = 0; pushDescriptors && i < 2 * pushedBuffersPerStage; ++i) {
			VkDescriptorSetLayoutBinding binding {};
			binding.binding = i;
			binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			binding.descriptorCount = 1;
			binding.stageFlags = i < pushedBuffersPerStage ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
			bindings.push_back(binding);
		}
		VkDescriptorSetLayoutCreateInfo info {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		info.flags = pushDescriptors ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
		info.bindingCount = bindings.size();
		info.pBindings = bindings.data();
		if (DynamicVK::vkCreateDescriptorSetLayout(_device, &info, nullptr, &bufferSetLayout) != VK_SUCCESS)
			throw std::runtime_error("Could not create buffer descriptor set layout");
	}
	loadPipelineCache(*this, _pipelineCachePath);
	graphicsLibraries = std::make_shared<GraphicsLibraries>(*this);

	// create the event loop semaphore

	VkSemaphoreTypeCreateInfo semaphoreTypeInfo {};
	semaphoreTypeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
	semaphoreTypeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

	VkSemaphoreCreateInfo semaphoreCreateInfo {};
	semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	semaphoreCreateInfo.pNext = &semaphoreTypeInfo;

	VkSemaphore wakeupSemaphore;
	if (DynamicVK::vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &wakeupSemaphore) != VK_SUCCESS) {
		// TODO
		abort();
	}

	_eventLoopSemaphores.push_back(wakeupSemaphore);
	_eventLoopWaitValues.push_back(1);
	_eventLoopCallbacks.push_back(nullptr);

	// Vulkan semaphore creation cost ~1.3 ms for four upload-time pool misses.
	// Pay once before frames start, and retain the reserve across bursts.
	recycledCompletionSemaphores.reserve(completionSemaphoreCapacity);
	for (size_t i = 0; i < completionSemaphoreCapacity; ++i) {
		VkSemaphore semaphore;
		if (DynamicVK::vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &semaphore) != VK_SUCCESS)
			abort();
		recycledCompletionSemaphores.emplace_back(semaphore, 0);
		semaphoreLifetimeDiagnostic(false, true);
	}

	if (_graphicsQueueFamilyIndex || _computeQueueFamilyIndex) {
		VkCommandPoolCreateInfo createInfo {};
		createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		createInfo.queueFamilyIndex = _graphicsQueueFamilyIndex ? *_graphicsQueueFamilyIndex : *_computeQueueFamilyIndex;

		if (DynamicVK::vkCreateCommandPool(_device, &createInfo, nullptr, &_oneshotCommandPool) != VK_SUCCESS) {
			// TODO
			abort();
		}
	}
	if (_graphicsQueueFamilyIndex) graphicsTimestampBits = queueFamilies[*_graphicsQueueFamilyIndex].timestampValidBits;
	synchronizeSubmissionEntryPoints();
};

void Indium::PrivateDevice::savePipelineCache(bool force) {
	if (!pipelineCache || _pipelineCachePath.empty()) return;
	// A compile thread never waits for another thread's save.
	std::unique_lock lock(_pipelineCacheMutex, std::try_to_lock);
	if (!lock.owns_lock()) {
		if (!force) return;
		lock.lock();
	}
	const auto now = std::chrono::steady_clock::now();
	if (!force && now - _pipelineCacheSaved < std::chrono::seconds(10)) return;
	_pipelineCacheSaved = now;
	size_t size = 0;
	if (DynamicVK::vkGetPipelineCacheData(_device, pipelineCache, &size, nullptr) != VK_SUCCESS || !size) return;
	std::vector<char> data(size);
	// VK_INCOMPLETE: the cache grew meanwhile; the next save gets it.
	if (DynamicVK::vkGetPipelineCacheData(_device, pipelineCache, &size, data.data()) != VK_SUCCESS) return;
	// Write a new file and rename it, so a reader never sees half a cache.
	const auto temporary = _pipelineCachePath + ".tmp";
	FILE* file = std::fopen(temporary.c_str(), "wb");
	if (!file) return;
	const bool written = std::fwrite(data.data(), 1, size, file) == size;
	if (std::fclose(file) == 0 && written) std::rename(temporary.c_str(), _pipelineCachePath.c_str());
	else std::remove(temporary.c_str());
}

Indium::PrivateDevice::~PrivateDevice() {
	// Pipeline states and libraries are gone by now; queued library builds must not start.
	if (graphicsLibraries) graphicsLibraries->close();
	graphicsLibraries.reset();
	savePipelineCache(true);
	if (pipelineCache) DynamicVK::vkDestroyPipelineCache(_device, pipelineCache, nullptr);
	if (bufferSetLayout) DynamicVK::vkDestroyDescriptorSetLayout(_device, bufferSetLayout, nullptr);
	uploads.reset();
	descriptorArenas.clear();
	renderTargets.clear();
	for (auto pool : timestampPools) destroyTimestampPool(_device, pool);
	for (auto entry : recycledCommands) DynamicVK::vkDestroyCommandPool(_device, entry.first, nullptr);
	for (auto entry : recycledCompletionSemaphores) {
		DynamicVK::vkDestroySemaphore(_device, entry.first, nullptr);
		semaphoreLifetimeDiagnostic(false, false);
	}
	if (_oneshotCommandPool) {
		DynamicVK::vkDestroyCommandPool(_device, _oneshotCommandPool, nullptr);
	}
	DynamicVK::vkDestroySemaphore(_device, _eventLoopSemaphores[0], nullptr);
	bufferMemoryBlocks.clear();
	imageMemoryBlocks.clear();
	DynamicVK::vkDestroyDevice(_device, nullptr);
};

std::string Indium::PrivateDevice::name() const {
	return _properties.deviceName;
};

std::shared_ptr<Indium::CommandQueue> Indium::PrivateDevice::newCommandQueue() {
	return std::make_shared<PrivateCommandQueue>(shared_from_this());
};

std::shared_ptr<Indium::RenderPipelineState> Indium::PrivateDevice::newRenderPipelineState(const RenderPipelineDescriptor& descriptor) {
	return std::make_shared<PrivateRenderPipelineState>(shared_from_this(), descriptor);
};

std::shared_ptr<Indium::ComputePipelineState> Indium::PrivateDevice::newComputePipelineState(const ComputePipelineDescriptor& descriptor, PipelineOption options, std::shared_ptr<ComputePipelineReflection> reflection) {
	if (options != PipelineOption::None) {
		throw std::runtime_error("TODO: support compute pipeline options");
	}
	return std::make_shared<PrivateComputePipelineState>(shared_from_this(), descriptor);
};

std::shared_ptr<Indium::ComputePipelineState> Indium::PrivateDevice::newComputePipelineState(std::shared_ptr<Function> computeFunction, PipelineOption options, std::shared_ptr<ComputePipelineReflection> reflection) {
	return newComputePipelineState(ComputePipelineDescriptor { computeFunction }, options, reflection);
};

std::shared_ptr<Indium::Buffer> Indium::PrivateDevice::newBuffer(size_t length, ResourceOptions options) {
	return std::make_shared<PrivateBuffer>(shared_from_this(), length, options);
};

std::shared_ptr<Indium::Buffer> Indium::PrivateDevice::newBuffer(const void* pointer, size_t length, ResourceOptions options) {
	return std::make_shared<PrivateBuffer>(shared_from_this(), pointer, length, options);
};

std::shared_ptr<Indium::Library> Indium::PrivateDevice::newLibrary(const void* data, size_t length) {
	RbxProfiler::Scope profile(RBX_PROF_SHADER);
	// TODO: cache translated libraries
	size_t translatedSize = 0;
	Iridium::OutputInfo outputInfo;
	auto translatedData = Iridium::translate(data, length, translatedSize, outputInfo);
	if (!translatedData || translatedSize < 20 || translatedSize % 4) {
		free(translatedData);
		return nullptr;
	}
	PrivateLibrary::FunctionInfoMap funcInfoMap;

	for (const auto& [name, info]: outputInfo.functionInfos) {
		auto& funcInfo = funcInfoMap[name];

		switch (info.type) {
			case Iridium::FunctionType::Fragment:
				funcInfo.functionType = FunctionType::Fragment;
				break;
			case Iridium::FunctionType::Vertex:
				funcInfo.functionType = FunctionType::Vertex;
				break;
			case Iridium::FunctionType::Kernel:
				funcInfo.functionType = FunctionType::Kernel;
				break;
		}

		funcInfo.bindings.insert(funcInfo.bindings.end(), info.bindings.begin(), info.bindings.end());
		funcInfo.embeddedSamplers.insert(funcInfo.embeddedSamplers.end(), info.embeddedSamplers.begin(), info.embeddedSamplers.end());
	}

	auto lib = std::make_shared<PrivateLibrary>(shared_from_this(), static_cast<const char*>(translatedData), translatedSize, funcInfoMap);
	free(translatedData);
	return lib;
};

std::shared_ptr<Indium::Texture> Indium::PrivateDevice::newTexture(const TextureDescriptor& descriptor) {
	return std::make_shared<ConcreteTexture>(shared_from_this(), descriptor);
};

std::shared_ptr<Indium::SamplerState> Indium::PrivateDevice::newSamplerState(const SamplerDescriptor& descriptor) {
	return std::make_shared<PrivateSamplerState>(shared_from_this(), descriptor);
};

std::shared_ptr<Indium::DepthStencilState> Indium::PrivateDevice::newDepthStencilState(const DepthStencilDescriptor& descriptor) {
	return std::make_shared<PrivateDepthStencilState>(shared_from_this(), descriptor);
};

std::shared_ptr<Indium::Device> Indium::createSystemDefaultDevice() {
	return globalDeviceList.empty() ? nullptr : globalDeviceList.front();
};

void Indium::PrivateDevice::pollEvents(uint64_t timeoutNanoseconds) {
	// This runs once per commit. Taking the queue lock and polling four upload
	// fences every time contended with commit for no benefit; commit flushes the
	// upload queue itself, so servicing it here only matters when work is staged.
	if (uploads && uploads->needsService.load(std::memory_order_relaxed)) {
		std::scoped_lock queueLock(queueMutex);
		if (uploads) { uploads->flush(); uploads->collect(); }
	}
	// this is held for the entire duration of the poll so we can
	// ensure we're the only one polling, which allows us to avoid
	// some extra logic to handle the case of multiple thread polling simultaneously
	std::unique_lock pollingLock(_pollingMutex);

	std::unique_lock lock(_eventLoopMutex);
	// Shutdown can race ahead of the poll arming its wait. Consume that wake
	// here instead of sleeping after the caller has already requested an exit.
	if (_eventLoopWakePending) {
		_eventLoopWakePending = false;
		return;
	}

	// With no GPU work, sleep on a CPU condition variable. Signalling a Vulkan
	// timeline here charged the submitting thread ~10 ms per geometry burst.
	if (_eventLoopSemaphores.size() == 1) {
		_eventLoopWaiting.store(true, std::memory_order_relaxed);
		auto awakened = [&] { return !_eventLoopWaiting.load(std::memory_order_relaxed); };
		if (timeoutNanoseconds == UINT64_MAX)
			_eventLoopIdle.wait(lock, awakened);
		else
			_eventLoopIdle.wait_for(lock, std::chrono::nanoseconds(std::min<uint64_t>(timeoutNanoseconds, INT64_MAX)), awakened);
		_eventLoopWaiting.store(false, std::memory_order_relaxed);
		_eventLoopWakePending = false;
		return;
	}

	const std::vector<VkSemaphore> semaphores = _eventLoopSemaphores;
	const std::vector<uint64_t> values = _eventLoopWaitValues;
	// _eventLoopCallbacks is deliberately not copied here. Previously every commit woke this
	// loop, and each callback captures shared_ptrs plus two shared_ptr vectors, so
	// deep-copying the whole list per wakeup cost O(in-flight) heap allocations
	// each time: at ~2k commits/s with ~65 in flight that was ~128k copies/s, and
	// it grew with the backlog it was supposed to be draining. Only the ready
	// callbacks are moved out, below, once their indices are known.

	lock.unlock();

	VkSemaphoreWaitInfo info {};
	info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
	info.semaphoreCount = semaphores.size();
	info.pSemaphores = semaphores.data();
	info.pValues = values.data();
	info.flags = VK_SEMAPHORE_WAIT_ANY_BIT;

	// ponytail: cap an active GPU wait at 1 ms so newly registered independent
	// waits get a fresh snapshot without a driver signal on every submission.
	// Existing GPU completions still wake immediately; idle polls sleep above.
	// Use an interruptible driver wait if tighter independent-wait latency is needed.
	auto result = DynamicVK::vkWaitSemaphores(_device, &info, std::min<uint64_t>(timeoutNanoseconds, 1000000));

	if (result == VK_TIMEOUT) {
		// timed out with no semaphores ready
		return;
	}

	if (result != VK_SUCCESS) abort();

	std::vector<size_t> readyIndices;

	// now check which semaphores are ready
	// (excluding 0 because that's the special event loop wakeup semaphore)
	for (size_t i = 1; i < semaphores.size(); ++i) {
		uint64_t count;

		if (DynamicVK::vkGetSemaphoreCounterValue(_device, semaphores[i], &count) != VK_SUCCESS) {
			// TODO
			abort();
		}

		if (count >= values[i]) {
			readyIndices.push_back(i);
		}
	}

	// note that we're the only ones allowed to remove elements from the vectors
	// and this method cannot be invoked concurrently by different threads,
	// so we assume that the front portions of the vectors (the portions we copied
	// earlier) remain the same.

	lock.lock();

	// Move out in submission order, then erase back-to-front so the indices stay
	// valid. Callbacks must still run oldest-first.
	std::vector<std::function<void()>> ready(readyIndices.size());
	for (size_t i = 0; i < readyIndices.size(); ++i) {
		ready[i] = std::move(_eventLoopCallbacks[readyIndices[i]]);
	}

	for (auto it = readyIndices.rbegin(); it != readyIndices.rend(); ++it) {
		_eventLoopSemaphores.erase(_eventLoopSemaphores.begin() + *it);
		_eventLoopWaitValues.erase(_eventLoopWaitValues.begin() + *it);
		_eventLoopCallbacks.erase(_eventLoopCallbacks.begin() + *it);
	}

	lock.unlock();

	// now let's invoke callbacks for ready semaphores, outside the lock

	for (auto& callback : ready) {
		if (!callback) {
			continue;
		}

		callback();
	}
};

void Indium::PrivateDevice::wakeupEventLoop() {
	std::unique_lock lock(_eventLoopMutex);
	_eventLoopWakePending = true;
	if (_eventLoopWaiting.exchange(false, std::memory_order_relaxed)) {
		_eventLoopIdle.notify_one();
		return;
	}

	auto oldVal = _eventLoopWaitValues[0]++;

	VkSemaphoreSignalInfo info {};
	info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
	info.semaphore = _eventLoopSemaphores[0];
	info.value = oldVal;
	if (DynamicVK::vkSignalSemaphore(_device, &info) != VK_SUCCESS) abort();
};

void Indium::PrivateDevice::waitForSemaphore(VkSemaphore semaphore, uint64_t targetValue, std::function<void()> callback) {
	RbxProfiler::DiagnosticScope registration(RBX_DIAG_COMPLETION_REGISTER,0,this);
	{
		RbxProfiler::DiagnosticScope lockTiming(RBX_DIAG_COMPLETION_LOCK,0,this);
		std::unique_lock lock(_eventLoopMutex);
		lockTiming.finish();

		_eventLoopSemaphores.push_back(semaphore);
		_eventLoopWaitValues.push_back(targetValue);
		// Moved, not copied: the callback captures shared_ptrs and two shared_ptr
		// vectors, and this runs once per commit.
		_eventLoopCallbacks.push_back(std::move(callback));
		registration.detail=_eventLoopCallbacks.size()-1; // Exclude the wakeup entry.
		// Only an idle CPU wait needs a notification. Active GPU waits refresh
		// their snapshot within 1 ms, even behind an unsignalled dependency.
		if (_eventLoopWaiting.exchange(false, std::memory_order_relaxed)) {
			RbxProfiler::DiagnosticScope wake(RBX_DIAG_COMPLETION_WAKE,0,this,registration.detail);
			_eventLoopIdle.notify_one();
		}
	}
};

// TODO: create a semaphore pool to avoid constantly creating and destroying semaphores

Indium::TimelineSemaphore Indium::PrivateDevice::getTimelineSemaphore() {
	VkSemaphoreTypeCreateInfo typeInfo {};
	typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
	typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

	VkSemaphoreCreateInfo createInfo {};
	createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	createInfo.pNext = &typeInfo;

	VkSemaphore semaphore;
	RbxProfiler::DiagnosticScope create(RBX_DIAG_SEMAPHORE_CREATE,0,this);
	if (DynamicVK::vkCreateSemaphore(_device, &createInfo, nullptr, &semaphore) != VK_SUCCESS) {
		// TODO
		abort();
	}

	create.finish();
	semaphoreLifetimeDiagnostic(false, true);
	return TimelineSemaphore {
		shared_from_this(),
		semaphore,
		0,
	};
};

void Indium::PrivateDevice::putTimelineSemaphore(const TimelineSemaphore& semaphore) {
	DynamicVK::vkDestroySemaphore(_device, semaphore.semaphore, nullptr);
	semaphoreLifetimeDiagnostic(false, false);
};

std::shared_ptr<Indium::TimelineSemaphore> Indium::PrivateDevice::getWrappedTimelineSemaphore() {
	// Texture references survive their GPU users. Preserve the timeline value
	// when recycling, just as for command completions, instead of allocating
	// and destroying driver semaphores during every texture streaming burst.
	return getCompletionSemaphore();
};

std::shared_ptr<Indium::TimelineSemaphore> Indium::PrivateDevice::getCompletionSemaphore() {
	RbxProfiler::DiagnosticScope acquire(RBX_DIAG_COMPLETION_SEMAPHORE,0,this);
	TimelineSemaphore semaphore{shared_from_this(), VK_NULL_HANDLE, 0};
	{
		RbxProfiler::DiagnosticScope poolLock(RBX_DIAG_SEMAPHORE_POOL_LOCK,0,this);
		std::scoped_lock lock(recycledCommandMutex);
		poolLock.detail=recycledCompletionSemaphores.size();
		poolLock.finish();
		if (!recycledCompletionSemaphores.empty()) {
			acquire.detail=1; // Reuse; zero means a new Vulkan semaphore.
			auto entry = recycledCompletionSemaphores.back();
			recycledCompletionSemaphores.pop_back();
			semaphore.semaphore = entry.first;
			semaphore.count = entry.second;
		}
	}
	if (!semaphore.semaphore) semaphore = getTimelineSemaphore();
	return std::shared_ptr<TimelineSemaphore>(new TimelineSemaphore(std::move(semaphore)), [](TimelineSemaphore* ptr) {
		// Pending commands, polling callbacks and in-flight waitForGPU calls
		// retain this wrapper. Recycle only after its last owner releases it;
		// completed command objects need not pin it. Keep values increasing.
		bool cached = false;
		{
			std::scoped_lock lock(ptr->device->recycledCommandMutex);
			auto& pool = ptr->device->recycledCompletionSemaphores;
			if (pool.size() < completionSemaphoreCapacity && ptr->count != UINT64_MAX) {
				pool.emplace_back(ptr->semaphore, ptr->count);
				cached = true;
			}
		}
		if (!cached) ptr->device->putTimelineSemaphore(*ptr);
		delete ptr;
	});
};

Indium::BinarySemaphore Indium::PrivateDevice::getBinarySemaphore(bool exportable) {
	if (exportable && !(_features & Feature::ExternalSemaphoreFD)) {
		throw std::runtime_error("Device does not support exportable semaphores");
	}

	VkSemaphoreCreateInfo createInfo {};
	createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkExportSemaphoreCreateInfo exportInfo {};

	if (exportable) {
		exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
		exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

		createInfo.pNext = &exportInfo;
	}

	VkSemaphore semaphore;
	if (DynamicVK::vkCreateSemaphore(_device, &createInfo, nullptr, &semaphore) != VK_SUCCESS) {
		// TODO
		abort();
	}

	semaphoreLifetimeDiagnostic(true, true, exportable);
	return BinarySemaphore { shared_from_this(), semaphore };
};

void Indium::PrivateDevice::putBinarySemaphore(const BinarySemaphore& semaphore) {
	DynamicVK::vkDestroySemaphore(_device, semaphore.semaphore, nullptr);
	semaphoreLifetimeDiagnostic(true, false);
};

std::shared_ptr<Indium::BinarySemaphore> Indium::PrivateDevice::getWrappedBinarySemaphore(bool exportable) {
	return std::shared_ptr<Indium::BinarySemaphore>(new BinarySemaphore(getBinarySemaphore(exportable)), [](BinarySemaphore* ptr) {
		ptr->device->putBinarySemaphore(*ptr);
		delete ptr;
	});
};
