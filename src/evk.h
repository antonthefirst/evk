/* An easy and hackable implementation of a basic vulkan setup         */
/* Heavily borrowed from Dear ImGui's vulkan example, thanks Omar! <3  */

#pragma once
#include <vulkan/vulkan.h>
#include "core/basic_types.h"

// #TODO replace with own structs
#include "imgui/imgui.h"
#include "wrap/imgui_impl_vulkan.h"

struct EasyVk {
	VkInstance inst;
	VkAllocationCallbacks* alloc;
	VkPhysicalDevice phys_dev;
	VkDevice dev;
	uint32_t que_fam;
	VkQueue que;
	VkDescriptorPool desc_pool;
	VkPipelineCache pipe_cache;
	VkDebugReportCallbackEXT debug;
	ImGui_ImplVulkanH_Window win;
};

typedef void (*EasyVkCheckErrorFunc)(VkResult err);

extern EasyVk evk;

void evkInit(const char** extensions, uint32_t extensions_count);
void evkTerm();

int  evkMinImageCount();
void evkSelectSurfaceFormatAndPresentMode(VkSurfaceKHR surface);
void evkResizeWindow(ivec2 res);
void evkCheckError(VkResult err);
EasyVkCheckErrorFunc evkGetCheckErrorFunc();

void evkRenderBegin();
VkCommandBuffer evkGetRenderCommandBuffer();
void evkRenderEnd();
void evkPresent();

/*

struct EasyVkFrame {
	VkCommandPool comm_pool;
	VkCommandBuffer comm_buff;
	VkFence fence;
	VkImage back_buff;
	VkImageView back_buff_view;
	VkFramebuffer frame_buff;
};

struct EasyVkFrameSem {

};

#include "core/vec2.h"
struct EasyVkWindow {
	ivec2 res;
	VkSwapchainKHR swapchain;
	VkSurfaceKHR surface;
	VkSurfaceFormatKHR surface_format;
	VkPresentModeKHR present_mode;
	VkRenderPass render_pass;
	bool clear_enable;
	VkClearValue clear_value;
	uint32_t frame_idx;
	uint32_t buff_count;
	uint32_t sem_idx;
	EasyVkFrame* frames;
	EasyVkFrameSem* frame_sems;
};

evk.win.res.x


*/