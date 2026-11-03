// vkd3d-proton D3D12<->Vulkan interop interfaces.
//
// Declared from vkd3d-proton's PUBLIC IDL (include/vkd3d_device_vkd3d_ext.idl, LGPL-2.1): the IIDs and
// method signatures are the stable interop ABI exposed by the vkd3d-proton D3D12 device. This is the
// only bridge from a D3D12 resource the game handed us to a VkImage we can run fp16 compute on, which
// is impossible through D3D12 on Turnip (no native 16-bit / no relaxed min precision). Written here
// rather than taken from the GPL fork so the interceptor stays off GPL.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <vulkan/vulkan.h>

MIDL_INTERFACE("39da4e09-bd1c-4198-9fae-86bbe3be41fd")
ID3D12DXVKInteropDevice : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetDXGIAdapter(REFIID iid, void** ppvObject) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInstanceExtensions(UINT* pExtensionCount, const char** ppExtensions) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceExtensions(UINT* pExtensionCount, const char** ppExtensions) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFeatures(const VkPhysicalDeviceFeatures2** ppFeatures) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanHandles(VkInstance* pVkInstance, VkPhysicalDevice* pVkPhysicalDevice,
                                                       VkDevice* pVkDevice) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanQueueInfo(ID3D12CommandQueue* pCommandQueue, VkQueue* pVkQueue,
                                                         UINT32* pVkQueueFamily) = 0;
    virtual void STDMETHODCALLTYPE GetVulkanImageLayout(ID3D12Resource* pResource, D3D12_RESOURCE_STATES State,
                                                        VkImageLayout* pVkLayout) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanResourceInfo(ID3D12Resource* pResource, UINT64* pVkHandle,
                                                            UINT64* pBufferOffset) = 0;
    virtual HRESULT STDMETHODCALLTYPE LockCommandQueue(ID3D12CommandQueue* pCommandQueue) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnlockCommandQueue(ID3D12CommandQueue* pCommandQueue) = 0;
};

MIDL_INTERFACE("902d8115-59eb-4406-9518-fe00f991ee65")
ID3D12DXVKInteropDevice1 : public ID3D12DXVKInteropDevice
{
    virtual HRESULT STDMETHODCALLTYPE GetVulkanResourceInfo1(ID3D12Resource* pResource, UINT64* pVkHandle,
                                                             UINT64* pBufferOffset, VkFormat* pFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateInteropCommandQueue(const D3D12_COMMAND_QUEUE_DESC* pDesc,
                                                               UINT32 vkQueueFamilyIndex,
                                                               ID3D12CommandQueue** ppQueue) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateInteropCommandAllocator(D3D12_COMMAND_LIST_TYPE type,
                                                                   UINT32 vkQueueFamilyIndex,
                                                                   ID3D12CommandAllocator** ppAllocator) = 0;
    virtual HRESULT STDMETHODCALLTYPE BeginVkCommandBufferInterop(ID3D12CommandList* pCmdList,
                                                                  VkCommandBuffer* pCommandBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndVkCommandBufferInterop(ID3D12CommandList* pCmdList) = 0;
};

#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(ID3D12DXVKInteropDevice, 0x39da4e09, 0xbd1c, 0x4198, 0x9f, 0xae, 0x86, 0xbb, 0xe3, 0xbe, 0x41, 0xfd)
__CRT_UUID_DECL(ID3D12DXVKInteropDevice1, 0x902d8115, 0x59eb, 0x4406, 0x95, 0x18, 0xfe, 0x00, 0xf9, 0x91, 0xee, 0x65)
#endif
