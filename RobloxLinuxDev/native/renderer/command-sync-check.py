#!/usr/bin/env python3
"""Check production queue isolation, barriers and texture/view synchronization."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
source = (here / 'command-buffer.cpp').read_text()


def function(signature, source=source):
    start = source.index(signature)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


assert source.index('synchronizeGraphicsQueue(_commandBuffer);') < source.index('beginGpuTiming(')
assert 'if (usesGraphicsQueueOnly(privateTexture)) return;' in source
recorder = r'''
#include <cassert>
#include <memory>
#include <map>
#include <mutex>
#include <thread>
#include <vulkan/vulkan.h>
namespace Indium {
std::mutex& queueSubmissionMutex(VkQueue queue);
struct Texture {
    std::shared_ptr<Texture> parent;
    virtual ~Texture() = default;
    auto parentTexture() { return parent; }
};
struct ConcreteTexture : Texture {};
namespace DynamicVK {
void vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags from, VkPipelineStageFlags to,
                         VkDependencyFlags, uint32_t count, const VkMemoryBarrier* barriers,
                         uint32_t buffers, const VkBufferMemoryBarrier*,
                         uint32_t images, const VkImageMemoryBarrier*) {
    assert(from == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && to == from);
    assert(count == 1 && !buffers && !images);
    assert(barriers[0].srcAccessMask & VK_ACCESS_MEMORY_WRITE_BIT);
    assert(barriers[0].dstAccessMask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT));
}
}
}
'''
recorder += function('bool usesGraphicsQueueOnly(') + function('void synchronizeGraphicsQueue(')
device = (here / 'device.cpp').read_text()
assert device.count('std::scoped_lock lock(Indium::queueSubmissionMutex(queue));') == 2
recorder += function('std::mutex& Indium::queueSubmissionMutex(', device)
recorder += r'''
int main() {
    auto graphics = reinterpret_cast<VkQueue>(1);
    auto present = reinterpret_cast<VkQueue>(2);
    auto& graphicsLock = Indium::queueSubmissionMutex(graphics);
    assert(&graphicsLock == &Indium::queueSubmissionMutex(graphics));
    std::unique_lock held(graphicsLock);
    std::thread other([&] {
        assert(!Indium::queueSubmissionMutex(graphics).try_lock());
        auto& presentLock = Indium::queueSubmissionMutex(present);
        assert(presentLock.try_lock());
        presentLock.unlock();
    });
    other.join();
    auto texture = std::make_shared<Indium::ConcreteTexture>();
    auto drawable = std::make_shared<Indium::Texture>();
    auto view = std::make_shared<Indium::Texture>();
    auto nested = std::make_shared<Indium::Texture>();
    nested->parent = view;
    view->parent = texture;
    assert(usesGraphicsQueueOnly(texture) && usesGraphicsQueueOnly(nested));
    view->parent = drawable;
    assert(!usesGraphicsQueueOnly(drawable) && !usesGraphicsQueueOnly(nested));
    synchronizeGraphicsQueue(VK_NULL_HANDLE);
}
'''
with tempfile.TemporaryDirectory(prefix='command-sync-') as temporary:
    root = Path(temporary)
    (root / 'check.cpp').write_text(recorder)
    subprocess.run(['c++', '-std=c++17', '-pthread', '-I', str(here / '../../runtime/src/Vulkan-Headers-1.3.290/include'),
                    str(root / 'check.cpp'), '-o', str(root / 'check')], check=True)
    subprocess.run([str(root / 'check')], check=True)
