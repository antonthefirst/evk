#include "evk.h"
#include "core/log.h" // for ARRSIZE
#include "core/cpu_timer.h"
#include <stdlib.h>	  // for malloc, system
#include <stdio.h>    // for popen
#include "core/string_range.h"
#include "core/runprog.h"
#include "compute_shared.inl"

typedef unsigned short DrawIdx;
struct DrawVert {
    vec2  pos;
    vec2  uv;
    u32   col;
};

struct FrameRenderBuffers {
    VkDeviceMemory      VertexBufferMemory;
    VkDeviceMemory      IndexBufferMemory;
    VkDeviceSize        VertexBufferSize;
    VkDeviceSize        IndexBufferSize;
    VkBuffer            VertexBuffer;
    VkBuffer            IndexBuffer;
};
struct WindowRenderBuffers {
    uint32_t            Index;
    uint32_t            Count;
    FrameRenderBuffers*   Frames;
};

static int img_width = 128;
static int img_height = 128;

static VkDeviceSize             g_BufferMemoryAlignment = 256;
static VkPipelineCreateFlags    g_PipelineCreateFlags = 0x00;
static VkDescriptorSetLayout    g_DescriptorSetLayout = VK_NULL_HANDLE;
static VkPipelineLayout         g_PipelineLayout = VK_NULL_HANDLE;
static VkDescriptorSet          g_DescriptorSet = VK_NULL_HANDLE;
static VkPipeline               g_Pipeline = VK_NULL_HANDLE;

// Compute
static VkPipelineCreateFlags    g_ComputePipelineCreateFlags = 0x00;
static VkDescriptorSetLayout    g_ComputeDescriptorSetLayout = VK_NULL_HANDLE;
static VkPipelineLayout         g_ComputePipelineLayout = VK_NULL_HANDLE;
static VkDescriptorSet          g_ComputeDescriptorSet = VK_NULL_HANDLE;
static VkPipeline               g_ComputePipeline = VK_NULL_HANDLE;

// #TODO make this image uploader
static VkSampler                g_FontSampler = VK_NULL_HANDLE;
static VkDeviceMemory           g_FontMemory = VK_NULL_HANDLE;
static VkImage                  g_FontImage = VK_NULL_HANDLE;
static VkImageView              g_FontView = VK_NULL_HANDLE;
static VkDeviceMemory           g_UploadBufferMemory = VK_NULL_HANDLE;
static VkBuffer                 g_UploadBuffer = VK_NULL_HANDLE;

// Compute
static VkImageView              g_ComputeFontView = VK_NULL_HANDLE;

static WindowRenderBuffers    g_MainWindowRenderBuffers;

#include <stdio.h>
static uint8_t* loadBinary(const char* pathfile, size_t* size = NULL) {
	FILE* f = fopen(pathfile, "rb");
	size_t s = 0;
	uint8_t* b = 0;
	if (f) {
		fseek(f, 0, SEEK_END);
		s = ftell(f);
		fseek(f, 0, SEEK_SET);
		b = (uint8_t*)malloc(s);
		fread(b, s, 1, f);
		fclose(f);
	}
	if (size) *size = s;
	return b;
}

/*
#include <shaderc/shaderc.h>
static uint8_t* compileGLSLStringtoSPIRV(const char* str) {
	shaderc_compiler_t compiler = shaderc_compiler_initialize();
	shaderc_compiler_release(compiler);
}
static uint32_t* compileGLSLFiletoSPIRV(const char* pathfile, size_t* spirv_len) {
	size_t src_len = 0;
	char* src = (char*)loadBinary(pathfile, &src_len);

	shaderc_shader_kind kind = shaderc_fragment_shader;
	shaderc_compiler_t compiler = shaderc_compiler_initialize();
    shaderc_compilation_result_t compilation_result = shaderc_compile_into_spv(compiler, src, src_len, kind, pathfile, "main", nullptr);
	*spirv_len = shaderc_result_get_length(compilation_result);
	uint32_t* spirv = (uint32_t*)malloc(*spirv_len);
	memcpy(spirv, shaderc_result_get_bytes(compilation_result), *spirv_len);
	log("ERROR:\n");
	log(shaderc_result_get_error_message(compilation_result));
	log("----\n");
	shaderc_result_release(compilation_result);
	shaderc_compiler_release(compiler);
	free(src);
	return spirv;
}
*/
#if 0
static uint32_t* compileGLSLFiletoSPIRV(const char* pathfile, size_t* spirv_len) {
	FILE* out = _popen(TempStr("glslc -o %s.spv %s 2>&1", pathfile, pathfile), "rt");
	if (out) {
		log("TEST\n");
		char out_str[2048] = { };
		while(fgets(out_str, sizeof(out_str), out))
		{
			log(out_str);
		}
		/*
		char* ret = fgets(out_str, sizeof(out_str), out);
		_pclose(out);
		log("OUTPUT:\n");
		log(out_str);
		log("----\n");
		*/
	}
	return (uint32_t*)loadBinary(TempStr("%s.spv", pathfile), spirv_len);
}
#else

static uint32_t* compileGLSLFiletoSPIRV(const char* pathfile, size_t* spirv_len) {
	String output;

	runProg(TempStr("glslc -o %s.spv %s", pathfile, pathfile), &output);

	if (output.str) {
		log(output.str);
	}
	output.free();

	return (uint32_t*)loadBinary(TempStr("%s.spv", pathfile), spirv_len);
}
#endif

template<VkFormat FORMAT>
struct CustomMap {
	ivec2    size   = ivec2(3,8);

	// handles
	VkImage        image  = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView    view   = VK_NULL_HANDLE;

	void destroy() {
		if (view)   { vkDestroyImageView(evk.dev, view, evk.alloc); view   = VK_NULL_HANDLE; }
		if (image)  { vkDestroyImage(evk.dev, image, evk.alloc);    image  = VK_NULL_HANDLE; }
		if (memory) { vkFreeMemory(evk.dev, memory, evk.alloc);     memory = VK_NULL_HANDLE; }
		size = ivec2(0,0);
	}
	bool resize(ivec2 new_size, VkCommandBuffer command_buffer) {
		if (new_size == size) return true; 
		destroy();
		size = new_size;
		if (size == ivec2(0,0)) return true; 

		VkResult err;

		// Create the Image:
		{
			VkImageCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			info.imageType = VK_IMAGE_TYPE_2D;
			info.format = FORMAT;
			info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
			info.extent.width = size.x;
			info.extent.height = size.y;
			info.extent.depth = 1;
			info.mipLevels = 1;
			info.arrayLayers = 1;
			info.samples = VK_SAMPLE_COUNT_1_BIT;
			info.tiling = VK_IMAGE_TILING_OPTIMAL;
			info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
			info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			err = vkCreateImage(evk.dev, &info, evk.alloc, &image);
			evkCheckError(err);
			if (err) { destroy(); return false; }
			VkMemoryRequirements req;
			vkGetImageMemoryRequirements(evk.dev, image, &req);
			VkMemoryAllocateInfo alloc_info = {};
			alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			alloc_info.allocationSize = req.size;
			alloc_info.memoryTypeIndex = evkMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, req.memoryTypeBits);
			err = vkAllocateMemory(evk.dev, &alloc_info, evk.alloc, &memory);
			evkCheckError(err);
			if (err) { destroy(); return false; }
			err = vkBindImageMemory(evk.dev, image, memory, 0);
			evkCheckError(err);
			if (err) { destroy(); return false; }
			logInfo("IMAGE", "Resized to %dx%d, %d MiB (%d B)", size.x, size.y, alloc_info.allocationSize / (1024 * 1024), alloc_info.allocationSize);
		}

		// Create the View:
		{
			VkImageViewCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			info.image = image;
			info.viewType = VK_IMAGE_VIEW_TYPE_2D;
			info.format = FORMAT;
			info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			info.subresourceRange.levelCount = 1;
			info.subresourceRange.layerCount = 1;
			err = vkCreateImageView(evk.dev, &info, evk.alloc, &view);
			evkCheckError(err);
			if (err) { destroy(); return false; }
		}

		// Change format:
		{
		    VkImageMemoryBarrier use_barrier[1] = {};
			use_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			use_barrier[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			use_barrier[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
			use_barrier[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
			use_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			use_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			use_barrier[0].image = image;
			use_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			use_barrier[0].subresourceRange.levelCount = 1;
			use_barrier[0].subresourceRange.layerCount = 1;
			vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, use_barrier);
		}
		return true;
	}
};

struct TotalMap {
	CustomMap<VK_FORMAT_R32G32B32A32_UINT> apple;
	CustomMap<VK_FORMAT_R32_UINT> bannana;
	CustomMap<VK_FORMAT_R8_UINT> cucumber;
	CustomMap<VK_FORMAT_R8G8B8A8_UINT> durian;
	ivec2 size = ivec2(0,0);

	VkImageView render_view = VK_NULL_HANDLE;

	void destroy() {
		if (render_view) { vkDestroyImageView(evk.dev, render_view, evk.alloc); render_view = VK_NULL_HANDLE; }
		apple.destroy();
		bannana.destroy();
		cucumber.destroy();
		durian.destroy();
		size = ivec2(0,0);
	}
	bool resize(ivec2 new_size) {
		if (size == new_size) return true;

		VkResult err;

		// Wait until textures are no longer being used
		evkWaitUntilDeviceIdle();
	
		destroy();
		if (new_size == ivec2(0, 0)) return true;

		VkCommandPool command_pool = evk.win.Frames[evk.win.FrameIndex].CommandPool;
		VkCommandBuffer command_buffer = evk.win.Frames[evk.win.FrameIndex].CommandBuffer;
		
		evkResetCommandPool(command_pool);
		evkBeginCommandBuffer(command_buffer);

		if (!apple.resize(new_size, command_buffer)) { destroy(); return false; }
		if (!bannana.resize(new_size, command_buffer)) { destroy(); return false; }
		if (!cucumber.resize(new_size, command_buffer)) { destroy(); return false; }
		if (!durian.resize(new_size, command_buffer)) { destroy(); return false; }

		evkEndCommandBufferAndSubmit(command_buffer);

		size = new_size;

		// Create render view:
		{
			VkImageViewCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			info.image = durian.image;
			info.viewType = VK_IMAGE_VIEW_TYPE_2D;
			info.format = VK_FORMAT_R8G8B8A8_UNORM;
			info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			info.subresourceRange.levelCount = 1;
			info.subresourceRange.layerCount = 1;
			err = vkCreateImageView(evk.dev, &info, evk.alloc, &render_view);
			evkCheckError(err);
			if (err) { destroy(); return false; }
		}

		updateDescriptors();

		// Wait until images are finished initializing before using them.
		evkWaitUntilDeviceIdle();

		return true;
	}
	void updateDescriptors() {
		// Update compute descriptor set:
		{
			VkImageView views[] = { apple.view, bannana.view, cucumber.view, durian.view };
			VkWriteDescriptorSet write_desc[ARRSIZE(views)] = { };
			VkDescriptorImageInfo desc_image[ARRSIZE(views)][1] = { };
			for (int i = 0; i < ARRSIZE(views); ++i) {
				desc_image[i][0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
				desc_image[i][0].imageView = views[i];
				write_desc[i].dstBinding = i;
				write_desc[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write_desc[i].dstSet = g_ComputeDescriptorSet;
				write_desc[i].descriptorCount = 1;
				write_desc[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				write_desc[i].pImageInfo = desc_image[i];
			}
			vkUpdateDescriptorSets(evk.dev, ARRSIZE(views), write_desc, 0, NULL);
		}
		// Update render descriptor set:
		{
			VkDescriptorImageInfo desc_image[1] = {};
			desc_image[0].sampler = g_FontSampler;
			desc_image[0].imageView = render_view;
			desc_image[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
			VkWriteDescriptorSet write_desc[1] = {};
			write_desc[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write_desc[0].dstSet = g_DescriptorSet;
			write_desc[0].descriptorCount = 1;
			write_desc[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			write_desc[0].pImageInfo = desc_image;
			vkUpdateDescriptorSets(evk.dev, 1, write_desc, 0, NULL);
		}
	}
};

static TotalMap total_map;

static void createTexture(VkCommandBuffer command_buffer) {

    size_t upload_size = img_width*img_height*4*sizeof(char);
	unsigned char* pixels = (unsigned char*)malloc(upload_size);
	{
		u32* ptr = (u32*)pixels;
		for (int y = 0; y < img_height; ++y) {
			for (int x = 0; x < img_width; ++x) {
				int i = x + y * img_width;
				if ((x^y)&1) ptr[i] = 0xffffffff;
				else ptr[i] = 0xffff0000;
			}
		}
	}
    
    VkResult err;

    // Create the Image:
    {
        VkImageCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
		info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        info.extent.width = img_width;
        info.extent.height = img_height;
        info.extent.depth = 1;
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        err = vkCreateImage(evk.dev, &info, evk.alloc, &g_FontImage);
        evkCheckError(err);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(evk.dev, g_FontImage, &req);
        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = req.size;
        alloc_info.memoryTypeIndex = evkMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, req.memoryTypeBits);
        err = vkAllocateMemory(evk.dev, &alloc_info, evk.alloc, &g_FontMemory);
        evkCheckError(err);
        err = vkBindImageMemory(evk.dev, g_FontImage, g_FontMemory, 0);
        evkCheckError(err);
    }

    // Create the Image View:
    {
        VkImageViewCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = g_FontImage;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.layerCount = 1;
        err = vkCreateImageView(evk.dev, &info, evk.alloc, &g_FontView);
        evkCheckError(err);
    }

    // Create the compute Image View:
    {
        VkImageViewCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = g_FontImage;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UINT;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.layerCount = 1;
        err = vkCreateImageView(evk.dev, &info, evk.alloc, &g_ComputeFontView);
        evkCheckError(err);
    }

	// #TODO pull this out
    // Update the Graphics Descriptor Set:
	/*
    {
        VkDescriptorImageInfo desc_image[1] = {};
        desc_image[0].sampler = g_FontSampler;
        desc_image[0].imageView = g_FontView;
        desc_image[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;//VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write_desc[1] = {};
        write_desc[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write_desc[0].dstSet = g_DescriptorSet;
        write_desc[0].descriptorCount = 1;
        write_desc[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write_desc[0].pImageInfo = desc_image;
        vkUpdateDescriptorSets(evk.dev, 1, write_desc, 0, NULL);
    }
	*/

	// Update the Compute Descriptor Set:
	/*
	{
	    VkDescriptorImageInfo desc_image[1] = {};
        //desc_image[0].sampler = g_FontSampler;
        desc_image[0].imageView = g_ComputeFontView;
        desc_image[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet write_desc[1] = {};
        write_desc[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write_desc[0].dstSet = g_ComputeDescriptorSet;
        write_desc[0].descriptorCount = 1;
        write_desc[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write_desc[0].pImageInfo = desc_image;
		vkUpdateDescriptorSets(evk.dev, 1, write_desc, 0, NULL);
	}
	*/

    // Create the Upload Buffer:
    {
        VkBufferCreateInfo buffer_info = {};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = upload_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        err = vkCreateBuffer(evk.dev, &buffer_info, evk.alloc, &g_UploadBuffer);
        evkCheckError(err);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(evk.dev, g_UploadBuffer, &req);
        g_BufferMemoryAlignment = (g_BufferMemoryAlignment > req.alignment) ? g_BufferMemoryAlignment : req.alignment;
        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = req.size;
        alloc_info.memoryTypeIndex = evkMemoryType(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, req.memoryTypeBits);
        err = vkAllocateMemory(evk.dev, &alloc_info, evk.alloc, &g_UploadBufferMemory);
        evkCheckError(err);
        err = vkBindBufferMemory(evk.dev, g_UploadBuffer, g_UploadBufferMemory, 0);
        evkCheckError(err);
    }

    // Upload to Buffer:
    {
        char* map = NULL;
        err = vkMapMemory(evk.dev, g_UploadBufferMemory, 0, upload_size, 0, (void**)(&map));
        evkCheckError(err);
        memcpy(map, pixels, upload_size);
        VkMappedMemoryRange range[1] = {};
        range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range[0].memory = g_UploadBufferMemory;
        range[0].size = upload_size;
        err = vkFlushMappedMemoryRanges(evk.dev, 1, range);
        evkCheckError(err);
        vkUnmapMemory(evk.dev, g_UploadBufferMemory);
    }

    // Copy to Image:
    {
        VkImageMemoryBarrier copy_barrier[1] = {};
        copy_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        copy_barrier[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        copy_barrier[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        copy_barrier[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        copy_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copy_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copy_barrier[0].image = g_FontImage;
        copy_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_barrier[0].subresourceRange.levelCount = 1;
        copy_barrier[0].subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, copy_barrier);

        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = img_width;
        region.imageExtent.height = img_height;
        region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(command_buffer, g_UploadBuffer, g_FontImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier use_barrier[1] = {};
        use_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        use_barrier[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        use_barrier[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        use_barrier[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        use_barrier[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;//VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        use_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        use_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        use_barrier[0].image = g_FontImage;
        use_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        use_barrier[0].subresourceRange.levelCount = 1;
        use_barrier[0].subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, use_barrier);
    }
}
static void destroyUploadBuffer() {
    if (g_UploadBuffer)
    {
        vkDestroyBuffer(evk.dev, g_UploadBuffer, evk.alloc);
        g_UploadBuffer = VK_NULL_HANDLE;
    }
    if (g_UploadBufferMemory)
    {
        vkFreeMemory(evk.dev, g_UploadBufferMemory, evk.alloc);
        g_UploadBufferMemory = VK_NULL_HANDLE;
    }
}

static void uploadTexture(const char* pathfile) {
	VkResult err;

    // Use any command queue
    VkCommandPool command_pool = evk.win.Frames[evk.win.FrameIndex].CommandPool;
    VkCommandBuffer command_buffer = evk.win.Frames[evk.win.FrameIndex].CommandBuffer;
		
	evkResetCommandPool(command_pool);
	evkBeginCommandBuffer(command_buffer);

	createTexture(command_buffer);

	evkEndCommandBufferAndSubmit(command_buffer);

    err = vkDeviceWaitIdle(evk.dev);
    evkCheckError(err);

	destroyUploadBuffer();
}

void recreateComputePipelineIfNeeded() {
	if (g_ComputePipeline) return;

    VkResult err;
    VkShaderModule comp_module = evkCreateShaderFromFile("shaders/basic.comp");
	/*
    // Create The Shader Module:
    {
        VkShaderModuleCreateInfo comp_info = {};
        comp_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		comp_info.pCode = (uint32_t*)loadBinary("shaders/basic.comp.spv", &comp_info.codeSize);
        err = vkCreateShaderModule(evk.dev, &comp_info, evk.alloc, &comp_module);
		free((void*)comp_info.pCode);
    }
	*/

	// Create Descriptor Set Layout
	{
		VkDescriptorSetLayoutBinding setLayoutBindings[] = {
			evkMakeDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT, 0),
			evkMakeDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT, 1),
			evkMakeDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT, 2),
			evkMakeDescriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT, 3),
		};
		VkDescriptorSetLayoutCreateInfo descriptorLayout = evkMakeDescriptorSetLayoutCreateInfo(setLayoutBindings, ARRSIZE(setLayoutBindings));
		err = vkCreateDescriptorSetLayout(evk.dev, &descriptorLayout, nullptr, &g_ComputeDescriptorSetLayout);
		evkCheckError(err);
	}

	// Create Pipeline Layout
	{
		VkPipelineLayoutCreateInfo pPipelineLayoutCreateInfo = evkMakePipelineLayoutCreateInfo(&g_ComputeDescriptorSetLayout);
		VkPushConstantRange push_constants[1] = {};
        push_constants[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_constants[0].size = sizeof(ComputeUPC);
		pPipelineLayoutCreateInfo.pushConstantRangeCount = 1;
		pPipelineLayoutCreateInfo.pPushConstantRanges = push_constants;
		err = vkCreatePipelineLayout(evk.dev, &pPipelineLayoutCreateInfo, nullptr, &g_ComputePipelineLayout);
		evkCheckError(err);
	}

	// Create Descriptor Set
	{
		VkDescriptorSetAllocateInfo allocInfo = evkMakeDescriptorSetAllocateInfo(evk.desc_pool, &g_ComputeDescriptorSetLayout, 1);
		err = vkAllocateDescriptorSets(evk.dev, &allocInfo, &g_ComputeDescriptorSet);
		evkCheckError(err);

		//#TODO update descriptor set here? right now updated as part of tex load...
	}

	// Create compute shader pipeline
	{
		VkComputePipelineCreateInfo computePipelineCreateInfo = evkMakeComputePipelineCreateInfo(g_ComputePipelineLayout);
		VkPipelineShaderStageCreateInfo stage = {};
		stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		stage.module = comp_module;
		stage.pName = "main";
		computePipelineCreateInfo.stage = stage;
		vkCreateComputePipelines(evk.dev, evk.pipe_cache, 1, &computePipelineCreateInfo, evk.alloc, &g_ComputePipeline);
		evkCheckError(err);
	}
	
    vkDestroyShaderModule(evk.dev, comp_module, evk.alloc);
}
void recreateGraphicsPipelineIfNeeded() {
	// if shaders changed
	if (g_Pipeline) return;

    VkResult err;
    VkShaderModule vert_module = evkCreateShaderFromFile("shaders/basic.vert");
    VkShaderModule frag_module = evkCreateShaderFromFile("shaders/basic.frag");
	
    if (!g_FontSampler)
    {
        VkSamplerCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = VK_FILTER_NEAREST;
        info.minFilter = VK_FILTER_NEAREST;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        info.minLod = -1000;
        info.maxLod = 1000;
        info.maxAnisotropy = 1.0f;
        err = vkCreateSampler(evk.dev, &info, evk.alloc, &g_FontSampler);
        evkCheckError(err);
    }
	
	
    if (!g_DescriptorSetLayout)
    {
        VkSampler sampler[1] = {g_FontSampler};
        VkDescriptorSetLayoutBinding binding[1] = {};
        binding[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding[0].descriptorCount = 1;
        binding[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binding[0].pImmutableSamplers = sampler;
        VkDescriptorSetLayoutCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 1;
        info.pBindings = binding;
        err = vkCreateDescriptorSetLayout(evk.dev, &info, evk.alloc, &g_DescriptorSetLayout);
        evkCheckError(err);
    }
	

	
    // Create Descriptor Set:
    {
        VkDescriptorSetAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = evk.desc_pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &g_DescriptorSetLayout;
        err = vkAllocateDescriptorSets(evk.dev, &alloc_info, &g_DescriptorSet);
        evkCheckError(err);
    }
	

    //if (!g_PipelineLayout)
    {
        // Constants: we are using 'vec2 offset' and 'vec2 scale' instead of a full 3d projection matrix
        VkPushConstantRange push_constants[1] = {};
        push_constants[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push_constants[0].offset = sizeof(float) * 0;
        push_constants[0].size = sizeof(float) * 4;
        VkDescriptorSetLayout set_layout[1] = { g_DescriptorSetLayout };
        VkPipelineLayoutCreateInfo layout_info = {};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = set_layout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = push_constants;
        err = vkCreatePipelineLayout(evk.dev, &layout_info, evk.alloc, &g_PipelineLayout);
        evkCheckError(err);
    }

    VkPipelineShaderStageCreateInfo stage[2] = {};
    stage[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage[0].module = vert_module;
    stage[0].pName = "main";
    stage[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stage[1].module = frag_module;
    stage[1].pName = "main";

    VkVertexInputBindingDescription binding_desc[1] = {};
    binding_desc[0].stride = sizeof(DrawVert);
    binding_desc[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attribute_desc[3] = {};
    attribute_desc[0].location = 0;
    attribute_desc[0].binding = binding_desc[0].binding;
    attribute_desc[0].format = VK_FORMAT_R32G32_SFLOAT;
    attribute_desc[0].offset = IM_OFFSETOF(DrawVert, pos);
    attribute_desc[1].location = 1;
    attribute_desc[1].binding = binding_desc[0].binding;
    attribute_desc[1].format = VK_FORMAT_R32G32_SFLOAT;
    attribute_desc[1].offset = IM_OFFSETOF(DrawVert, uv);
    attribute_desc[2].location = 2;
    attribute_desc[2].binding = binding_desc[0].binding;
    attribute_desc[2].format = VK_FORMAT_R8G8B8A8_UNORM;
    attribute_desc[2].offset = IM_OFFSETOF(DrawVert, col);

    VkPipelineVertexInputStateCreateInfo vertex_info = {};
    vertex_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_info.vertexBindingDescriptionCount = 1;
    vertex_info.pVertexBindingDescriptions = binding_desc;
    vertex_info.vertexAttributeDescriptionCount = 3;
    vertex_info.pVertexAttributeDescriptions = attribute_desc;

    VkPipelineInputAssemblyStateCreateInfo ia_info = {};
    ia_info.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia_info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_info = {};
    viewport_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_info.viewportCount = 1;
    viewport_info.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster_info = {};
    raster_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster_info.polygonMode = VK_POLYGON_MODE_FILL;
    raster_info.cullMode = VK_CULL_MODE_NONE;
    raster_info.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster_info.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms_info = {};
    ms_info.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms_info.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState color_attachment[1] = {};
    color_attachment[0].blendEnable = VK_TRUE;
    color_attachment[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    color_attachment[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    color_attachment[0].colorBlendOp = VK_BLEND_OP_ADD;
    color_attachment[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    color_attachment[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    color_attachment[0].alphaBlendOp = VK_BLEND_OP_ADD;
    color_attachment[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_info = {};
    depth_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineColorBlendStateCreateInfo blend_info = {};
    blend_info.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_info.attachmentCount = 1;
    blend_info.pAttachments = color_attachment;

    VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state = {};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = (uint32_t)IM_ARRAYSIZE(dynamic_states);
    dynamic_state.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.flags = g_PipelineCreateFlags;
    info.stageCount = 2;
    info.pStages = stage;
    info.pVertexInputState = &vertex_info;
    info.pInputAssemblyState = &ia_info;
    info.pViewportState = &viewport_info;
    info.pRasterizationState = &raster_info;
    info.pMultisampleState = &ms_info;
    info.pDepthStencilState = &depth_info;
    info.pColorBlendState = &blend_info;
    info.pDynamicState = &dynamic_state;
    info.layout = g_PipelineLayout;
    info.renderPass = evk.win.RenderPass;
    err = vkCreateGraphicsPipelines(evk.dev, evk.pipe_cache, 1, &info, evk.alloc, &g_Pipeline);
    evkCheckError(err);

    vkDestroyShaderModule(evk.dev, vert_module, evk.alloc);
    vkDestroyShaderModule(evk.dev, frag_module, evk.alloc);
}

static void createOrResizeVertexBuffer(VkBuffer& buffer, VkDeviceMemory& buffer_memory, VkDeviceSize& p_buffer_size, size_t new_size, VkBufferUsageFlagBits usage) {
    VkResult err;
    if (buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(evk.dev, buffer, evk.alloc);
    if (buffer_memory != VK_NULL_HANDLE)
        vkFreeMemory(evk.dev, buffer_memory, evk.alloc);

    VkDeviceSize vertex_buffer_size_aligned = ((new_size - 1) / g_BufferMemoryAlignment + 1) * g_BufferMemoryAlignment;
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = vertex_buffer_size_aligned;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    err = vkCreateBuffer(evk.dev, &buffer_info, evk.alloc, &buffer);
    evkCheckError(err);

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(evk.dev, buffer, &req);
    g_BufferMemoryAlignment = (g_BufferMemoryAlignment > req.alignment) ? g_BufferMemoryAlignment : req.alignment;
    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = req.size;
    alloc_info.memoryTypeIndex = evkMemoryType(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, req.memoryTypeBits);
    err = vkAllocateMemory(evk.dev, &alloc_info, evk.alloc, &buffer_memory);
    evkCheckError(err);

    err = vkBindBufferMemory(evk.dev, buffer, buffer_memory, 0);
    evkCheckError(err);
    p_buffer_size = new_size;
}
static void destroyFrameRenderBuffers(FrameRenderBuffers* buffers) {
    if (buffers->VertexBuffer) { vkDestroyBuffer(evk.dev, buffers->VertexBuffer, evk.alloc); buffers->VertexBuffer = VK_NULL_HANDLE; }
    if (buffers->VertexBufferMemory) { vkFreeMemory(evk.dev, buffers->VertexBufferMemory, evk.alloc); buffers->VertexBufferMemory = VK_NULL_HANDLE; }
    if (buffers->IndexBuffer) { vkDestroyBuffer(evk.dev, buffers->IndexBuffer, evk.alloc); buffers->IndexBuffer = VK_NULL_HANDLE; }
    if (buffers->IndexBufferMemory) { vkFreeMemory(evk.dev, buffers->IndexBufferMemory, evk.alloc); buffers->IndexBufferMemory = VK_NULL_HANDLE; }
    buffers->VertexBufferSize = 0;
    buffers->IndexBufferSize = 0;
}

static void destroyWindowRenderBuffers(WindowRenderBuffers* buffers) {
    for (uint32_t n = 0; n < buffers->Count; n++)
        destroyFrameRenderBuffers(&buffers->Frames[n]);
    free(buffers->Frames);
    buffers->Frames = NULL;
    buffers->Index = 0;
    buffers->Count = 0;
}

static void setupRenderState(VkCommandBuffer command_buffer, FrameRenderBuffers* rb, int fb_width, int fb_height)
{
    // Bind pipeline and descriptor sets:
    {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_Pipeline);
        VkDescriptorSet desc_set[1] = { g_DescriptorSet };
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_PipelineLayout, 0, 1, desc_set, 0, NULL);
    }

    // Bind Vertex And Index Buffer:
    {
        VkBuffer vertex_buffers[1] = { rb->VertexBuffer };
        VkDeviceSize vertex_offset[1] = { 0 };
        vkCmdBindVertexBuffers(command_buffer, 0, 1, vertex_buffers, vertex_offset);
        vkCmdBindIndexBuffer(command_buffer, rb->IndexBuffer, 0, sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    }

    // Setup viewport:
    {
        VkViewport viewport;
        viewport.x = 0;
        viewport.y = 0;
        viewport.width = (float)fb_width;
        viewport.height = (float)fb_height;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    }

    // Setup scale and translation:
    // Our visible imgui space lies from draw_data->DisplayPps (top left) to draw_data->DisplayPos+data_data->DisplaySize (bottom right). DisplayPos is (0,0) for single viewport apps.
    {
        float scale[2];
        scale[0] = 2.0f / evk.win.Width;
        scale[1] = 2.0f / evk.win.Height;
        float translate[2];
        translate[0] = -1.0f - 0.0f * scale[0];
        translate[1] = -1.0f - 0.0f * scale[1];
        vkCmdPushConstants(command_buffer, g_PipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, sizeof(float) * 0, sizeof(float) * 2, scale);
        vkCmdPushConstants(command_buffer, g_PipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, sizeof(float) * 2, sizeof(float) * 2, translate);
    }
}

static void loadTexturesIfNeeded() {
	if (g_FontImage) return;
	uploadTexture("test.png");
}

struct BasicQuad {
	DrawVert verts[4] = {
		{vec2(0.0f,0.0f), vec2(0.0f, 0.0f), 0xffffffff},
		{vec2(1.0f,0.0f), vec2(1.0f, 0.0f), 0xffffffff},
		{vec2(0.0f,1.0f), vec2(0.0f, 1.0f), 0xffffffff},
		{vec2(1.0f,1.0f), vec2(1.0f, 1.0f), 0xffffffff},
	};
	DrawIdx  idxs[6] = { 0, 1, 2,  1, 3 , 2 };
};

static BasicQuad quad;
static ComputeUPC upc;
static bool do_reset = true;
static bool do_update = true;
static bool do_render = true;

void destroyPipelines() {
	if (g_ComputeFontView)             { vkDestroyImageView(evk.dev, g_ComputeFontView, evk.alloc); g_ComputeFontView = VK_NULL_HANDLE; }
    if (g_FontView)             { vkDestroyImageView(evk.dev, g_FontView, evk.alloc); g_FontView = VK_NULL_HANDLE; }
    if (g_FontImage)            { vkDestroyImage(evk.dev, g_FontImage, evk.alloc); g_FontImage = VK_NULL_HANDLE; }
    if (g_FontMemory)           { vkFreeMemory(evk.dev, g_FontMemory, evk.alloc); g_FontMemory = VK_NULL_HANDLE; }
    if (g_FontSampler)          { vkDestroySampler(evk.dev, g_FontSampler, evk.alloc); g_FontSampler = VK_NULL_HANDLE; }
    if (g_DescriptorSetLayout)  { vkDestroyDescriptorSetLayout(evk.dev, g_DescriptorSetLayout, evk.alloc); g_DescriptorSetLayout = VK_NULL_HANDLE; }
    if (g_PipelineLayout)       { vkDestroyPipelineLayout(evk.dev, g_PipelineLayout, evk.alloc); g_PipelineLayout = VK_NULL_HANDLE; }
    if (g_Pipeline)             { vkDestroyPipeline(evk.dev, g_Pipeline, evk.alloc); g_Pipeline = VK_NULL_HANDLE; }
    if (g_ComputeDescriptorSetLayout)  { vkDestroyDescriptorSetLayout(evk.dev, g_ComputeDescriptorSetLayout, evk.alloc); g_ComputeDescriptorSetLayout = VK_NULL_HANDLE; }
    if (g_ComputePipelineLayout)       { vkDestroyPipelineLayout(evk.dev, g_ComputePipelineLayout, evk.alloc); g_ComputePipelineLayout = VK_NULL_HANDLE; }
    if (g_ComputePipeline)             { vkDestroyPipeline(evk.dev, g_ComputePipeline, evk.alloc); g_ComputePipeline = VK_NULL_HANDLE; }
}

void basicUpdate() {
	recreateComputePipelineIfNeeded();
	recreateGraphicsPipelineIfNeeded();
	//loadTexturesIfNeeded();
	gui::InputInt("stage", (int*)&upc.stage);
	static ivec2 size = ivec2(256,256);
	gui::InputInt2("size", (int*)&size);
	if (total_map.size.x == 0)
		total_map.resize(size);
	if (gui::Button("resize")) {
		total_map.resize(size);
	}
	if (gui::Button("reset"))
		do_reset = true;
	gui::Checkbox("update", &do_update);
	gui::Checkbox("render", &do_render);
	if (gui::Button("recompile shader")) {
		BlockTimer T("compile");
		{
			BlockTimer T("wait until idle");
			evkWaitUntilDeviceIdle();
		}
		// destroy old shaders
		{
			BlockTimer T("destroy pipelines");
			destroyPipelines();
		}

		// rebuild pipelines with new shaders
		{
			BlockTimer T("rebuild pipelines");
			recreateComputePipelineIfNeeded();
			recreateGraphicsPipelineIfNeeded();
		}

		total_map.updateDescriptors();
	}
}
void basicTerm() {
	total_map.destroy();

    destroyWindowRenderBuffers(&g_MainWindowRenderBuffers);
    destroyUploadBuffer();
	destroyPipelines();
}
void basicCompute(VkCommandBuffer command_buffer) {	
	// Bind pipeline
	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_ComputePipeline);
	
	// Bind descriptor sets
	vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_ComputePipelineLayout, 0, 1, &g_ComputeDescriptorSet, 0, 0);
	
	if (do_reset) {
		do_reset = false;
		upc.stage = 0;
		vkCmdPushConstants(command_buffer, g_ComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputeUPC) , &upc);
		vkCmdDispatch(command_buffer, total_map.size.x / 16, total_map.size.y / 16, 1);
		evkMemoryBarrier(command_buffer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	}
	
	if (do_update) {
		for (int i = 0; i < 1; ++i) {
			upc.stage = 1;
			vkCmdPushConstants(command_buffer, g_ComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputeUPC) , &upc);
			vkCmdDispatch(command_buffer, total_map.size.x / 16, total_map.size.y / 16, 1);
			evkMemoryBarrier(command_buffer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		}
	}
	
	if (do_render) {
		upc.stage = 2;
		vkCmdPushConstants(command_buffer, g_ComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputeUPC) , &upc);
		vkCmdDispatch(command_buffer, total_map.size.x / 16, total_map.size.y / 16, 1);
		evkMemoryBarrier(command_buffer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}
	
}
void basicRender(VkCommandBuffer command_buffer) {
    // Allocate array to store enough vertex/index buffers
    WindowRenderBuffers* wrb = &g_MainWindowRenderBuffers;
    if (wrb->Frames == NULL)
    {
        wrb->Index = 0;
        wrb->Count = evk.win.ImageCount;
        wrb->Frames = (FrameRenderBuffers*)malloc(sizeof(FrameRenderBuffers) * wrb->Count);
        memset(wrb->Frames, 0, sizeof(FrameRenderBuffers) * wrb->Count);
    }
    assert(wrb->Count == evk.win.ImageCount);
    wrb->Index = (wrb->Index + 1) % wrb->Count;
    FrameRenderBuffers* rb = &wrb->Frames[wrb->Index];

    VkResult err;

    // Create or resize the vertex/index buffers
    size_t vertex_size = sizeof(quad.verts);
    size_t index_size = sizeof(quad.idxs);
    if (rb->VertexBuffer == VK_NULL_HANDLE || rb->VertexBufferSize < vertex_size)
        createOrResizeVertexBuffer(rb->VertexBuffer, rb->VertexBufferMemory, rb->VertexBufferSize, vertex_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (rb->IndexBuffer == VK_NULL_HANDLE || rb->IndexBufferSize < index_size)
        createOrResizeVertexBuffer(rb->IndexBuffer, rb->IndexBufferMemory, rb->IndexBufferSize, index_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    // Upload vertex/index data into a single contiguous GPU buffer
    {
        DrawVert* vtx_dst = NULL;
        DrawIdx* idx_dst = NULL;
        err = vkMapMemory(evk.dev, rb->VertexBufferMemory, 0, vertex_size, 0, (void**)(&vtx_dst));
        evkCheckError(err);
        err = vkMapMemory(evk.dev, rb->IndexBufferMemory, 0, index_size, 0, (void**)(&idx_dst));
        evkCheckError(err);
		memcpy(vtx_dst, quad.verts, sizeof(quad.verts));
		memcpy(idx_dst, quad.idxs, sizeof(quad.idxs));
        VkMappedMemoryRange range[2] = {};
        range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range[0].memory = rb->VertexBufferMemory;
        range[0].size = VK_WHOLE_SIZE;
        range[1].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range[1].memory = rb->IndexBufferMemory;
        range[1].size = VK_WHOLE_SIZE;
        err = vkFlushMappedMemoryRanges(evk.dev, 2, range);
        evkCheckError(err);
        vkUnmapMemory(evk.dev, rb->VertexBufferMemory);
        vkUnmapMemory(evk.dev, rb->IndexBufferMemory);
    }

    // Setup desired Vulkan state
    setupRenderState(command_buffer, rb, evk.win.Width, evk.win.Height);

    VkRect2D scissor;
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent.width = evk.win.Width;
    scissor.extent.height = evk.win.Height;
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);

	vkCmdDrawIndexed(command_buffer, 6, 1, 0, 0, 0);
}