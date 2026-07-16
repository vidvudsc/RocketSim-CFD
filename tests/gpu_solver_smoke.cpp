#include "FlowSolver.h"

#include <vulkan/vulkan.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

int main() {
    uint32_t extensionCount=0; vkEnumerateInstanceExtensionProperties(nullptr,&extensionCount,nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount); vkEnumerateInstanceExtensionProperties(nullptr,&extensionCount,extensions.data());
    std::vector<const char*> instanceExtensions; VkInstanceCreateFlags flags=0;
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
    for(const auto& e:extensions) if(std::strcmp(e.extensionName,VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)==0){instanceExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);flags|=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;}
#endif
    VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};ici.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ici.flags=flags;ici.pApplicationInfo=&app;ici.enabledExtensionCount=static_cast<uint32_t>(instanceExtensions.size());ici.ppEnabledExtensionNames=instanceExtensions.data();
    VkInstance instance{};if(vkCreateInstance(&ici,nullptr,&instance)!=VK_SUCCESS)return 2;
    uint32_t physicalCount=0;vkEnumeratePhysicalDevices(instance,&physicalCount,nullptr);if(!physicalCount)return 3;
    std::vector<VkPhysicalDevice> physicals(physicalCount);vkEnumeratePhysicalDevices(instance,&physicalCount,physicals.data());VkPhysicalDevice physical=physicals[0];
    uint32_t queueCount=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&queueCount,nullptr);std::vector<VkQueueFamilyProperties> queues(queueCount);vkGetPhysicalDeviceQueueFamilyProperties(physical,&queueCount,queues.data());
    uint32_t family=0;while(family<queueCount&&(queues[family].queueFlags&VK_QUEUE_COMPUTE_BIT)==0)++family;if(family==queueCount)return 4;
    uint32_t deviceExtensionCount=0;vkEnumerateDeviceExtensionProperties(physical,nullptr,&deviceExtensionCount,nullptr);std::vector<VkExtensionProperties> deviceProperties(deviceExtensionCount);vkEnumerateDeviceExtensionProperties(physical,nullptr,&deviceExtensionCount,deviceProperties.data());std::vector<const char*> deviceExtensions;
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
    for(const auto& e:deviceProperties)if(std::strcmp(e.extensionName,VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)==0)deviceExtensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif
    float priority=1.0f;VkDeviceQueueCreateInfo qci{};qci.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
    VkDeviceCreateInfo dci{};dci.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;dci.enabledExtensionCount=static_cast<uint32_t>(deviceExtensions.size());dci.ppEnabledExtensionNames=deviceExtensions.data();
    VkDevice device{};if(vkCreateDevice(physical,&dci,nullptr,&device)!=VK_SUCCESS)return 5;VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
    rocket::Parameters parameters;rocket::FlowSolver solver(192,80);solver.enableGpu(physical,device,queue,family);solver.advanceSteps(12000,parameters);
    float minimumCenterlineExhaust=1.0f;for(int x=24;x<solver.width()-24;x+=24)minimumCenterlineExhaust=std::min(minimumCenterlineExhaust,solver.exhaustFractionAt(x,solver.height()/2));
    float symmetryError=0.0f;for(int y=0;y<solver.height()/2;y+=5)for(int x=0;x<solver.width();x+=8){const auto a=solver.colorAt(x,y,rocket::FieldView::Mach),b=solver.colorAt(x,solver.height()-1-y,rocket::FieldView::Mach);for(int c=0;c<3;++c)symmetryError=std::max(symmetryError,std::abs(a[c]-b[c]));}
    const auto d=solver.diagnostics();const bool valid=d.iteration==12000&&std::isfinite(d.maxMach)&&std::isfinite(d.maxTemperatureK)&&std::isfinite(d.massFlowKgPerS)&&d.maxMach<20.0f&&d.maxTemperatureK<20000.0f&&d.minTemperatureK>0.0f&&minimumCenterlineExhaust>0.5f&&symmetryError<0.05f;
    std::cout<<"gpu iterations="<<d.iteration<<" maxMach="<<d.maxMach<<" temperature=["<<d.minTemperatureK<<","<<d.maxTemperatureK<<"] massFlow="<<d.massFlowKgPerS<<'\n';
    std::cout<<"minimumCenterlineExhaust="<<minimumCenterlineExhaust<<'\n';
    std::cout<<"symmetryError="<<symmetryError<<'\n';
    solver.disableGpu();vkDeviceWaitIdle(device);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);return valid?0:1;
}
