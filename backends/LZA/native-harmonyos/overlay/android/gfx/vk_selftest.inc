/* ZA_LAYER_TEST=1: a startup check of layered rendering on this driver (included by nvn_vk.c, run once after the
   device is made). Z-A builds its colour-grading LUT (32x32x32) and a noise volume (64x64x6) by drawing one instance per
   slice of a 3D texture, the layer chosen by the vertex shader; without shaderOutputLayer a generated pass-through
   geometry shader writes gl_Layer (vk_layer_gs.inc). On the Maleoon 935 both volumes stayed empty. This draws the same
   way (VS: full-screen triangle, flat layer = InstanceIndex; GS: pass-through, gl_Layer; FS: ((gl_Layer + 1) / 8,
   0.5, 0.25, 1)) into
     arr-gs   a 4-layer 2D array image, one layered pass of 4 instances;
     3d-gs    a 16x16x4 3D image (2D_ARRAY_COMPATIBLE) through a 2D-array view of its 4 slices, the same draw;
     3d-clr   that 3D image's slices through 2D views, one pass each, cleared to ((s + 1) / 8, 0.75, 0, 1);
     3d-1     slice 2 of the 3D image through a 2D view, the draw without the geometry shader (FS sees layer 0);
   and logs each layer's centre texel, to tell a geometry shader / layer problem from a 3D-view one. */
#include "vk_selftest_spv.h"
static void vk_layer_selftest(void) {
    const char *e = getenv("ZA_LAYER_TEST");
    if (!e || atoi(e) != 1) return;
    enum { W = 16, NL = 4 };
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkImage img[2] = {0}; VkDeviceMemory mem[3] = {0}; VkImageView views[8] = {0}; int nviews = 0;
    for (int k = 0; k < 2; k++) {   /* 0: 2D array of NL layers; 1: 3D of NL slices */
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = k ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0, .imageType = k ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, .format = fmt,
            .extent = {W, W, k ? NL : 1}, .mipLevels = 1, .arrayLayers = k ? 1 : NL, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        VKCHECK(vkCreateImage(V.dev, &ci, NULL, &img[k]));
        VkMemoryRequirements q; vkGetImageMemoryRequirements(V.dev, img[k], &q);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = q.size, .memoryTypeIndex = (uint32_t)vk_pick_type(q.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &mem[k])); VKCHECK(vkBindImageMemory(V.dev, img[k], mem[k], 0));
    }
    VkImageView v_arr, v_3d, v_slice[NL];
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img[0], .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, NL}};
    VKCHECK(vkCreateImageView(V.dev, &vi, NULL, &v_arr)); views[nviews++] = v_arr;
    vi.image = img[1]; VKCHECK(vkCreateImageView(V.dev, &vi, NULL, &v_3d)); views[nviews++] = v_3d;
    for (int s = 0; s < NL; s++) { vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.subresourceRange.baseArrayLayer = (uint32_t)s; vi.subresourceRange.layerCount = 1; VKCHECK(vkCreateImageView(V.dev, &vi, NULL, &v_slice[s])); views[nviews++] = v_slice[s]; }
    VkShaderModule m[3]; const uint32_t *code[3] = {g_lt_vert, g_lt_geom, g_lt_frag}; size_t len[3] = {sizeof g_lt_vert, sizeof g_lt_geom, sizeof g_lt_frag};
    for (int i = 0; i < 3; i++) { VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = len[i], .pCode = code[i]}; VKCHECK(vkCreateShaderModule(V.dev, &mci, NULL, &m[i])); }
    VkPipelineLayout pl; VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; VKCHECK(vkCreatePipelineLayout(V.dev, &plci, NULL, &pl));
    VkPipeline pipe[2];
    for (int g = 0; g < 2; g++) {   /* 0: VS + GS + FS; 1: VS + FS */
        VkPipelineShaderStageCreateInfo st[3] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = m[0], .pName = "main"},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_GEOMETRY_BIT, .module = m[1], .pName = "main"},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = m[2], .pName = "main"}};
        if (g) st[1] = st[2];
        VkPipelineVertexInputStateCreateInfo vis = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkViewport vp = {0, 0, W, W, 0, 1}; VkRect2D sc = {{0, 0}, {W, W}};
        VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        VkPipelineDepthStencilStateCreateInfo dss = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 15};
        VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
        VkPipelineRenderingCreateInfo prci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};
        VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &prci, .stageCount = g ? 2 : 3, .pStages = st, .pVertexInputState = &vis, .pInputAssemblyState = &ia,
            .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &dss, .pColorBlendState = &cb, .layout = pl};
        VkResult r = vkCreateGraphicsPipelines(V.dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe[g]);
        if (r) { LOGV("[layer-test] pipeline %s: %d", g ? "VS+FS" : "VS+GS+FS", r); return; }
    }
    VkBuffer rb; VkDeviceMemory rm; uint8_t *rp;
    { VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 2 * W * W * 4 * NL, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT}; VKCHECK(vkCreateBuffer(V.dev, &bci, NULL, &rb));
      VkMemoryRequirements q; vkGetBufferMemoryRequirements(V.dev, rb, &q);
      VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = q.size, .memoryTypeIndex = (uint32_t)vk_pick_type(q.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
      VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &rm)); mem[2] = rm; VKCHECK(vkBindBufferMemory(V.dev, rb, rm, 0)); VKCHECK(vkMapMemory(V.dev, rm, 0, VK_WHOLE_SIZE, 0, (void **)&rp)); memset(rp, 0xee, 2 * W * W * 4 * NL); }
    VkCommandPool cpool; VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = V.qfam}; VKCHECK(vkCreateCommandPool(V.dev, &cpci, NULL, &cpool));
    VkCommandBuffer c; VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1}; VKCHECK(vkAllocateCommandBuffers(V.dev, &cai, &c));
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; VKCHECK(vkBeginCommandBuffer(c, &cbi));
    VkMemoryBarrier2 all = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT};
    VkImageMemoryBarrier2 ib[2];
    for (int k = 0; k < 2; k++) ib[k] = (VkImageMemoryBarrier2){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img[k], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, k ? 1 : NL}};
    VkDependencyInfo d0 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = ib}; vkCmdPipelineBarrier2(c, &d0);
    VkDependencyInfo dall = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &all};
    for (int t = 0; t < 2; t++) {   /* arr-gs, 3d-gs: clear to 0, 4 instances through the geometry shader */
        VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = t ? v_3d : v_arr, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, W}}, .layerCount = NL, .colorAttachmentCount = 1, .pColorAttachments = &ca};
        vkCmdBeginRendering(c, &ri); vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[0]); vkCmdDraw(c, 3, NL, 0, 0); vkCmdEndRendering(c);
        vkCmdPipelineBarrier2(c, &dall);
        VkBufferImageCopy cp = {.bufferOffset = (VkDeviceSize)t * W * W * 4 * NL, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, t ? 1 : NL}, .imageExtent = {W, W, t ? NL : 1}};
        vkCmdCopyImageToBuffer(c, img[t], VK_IMAGE_LAYOUT_GENERAL, rb, 1, &cp);
        vkCmdPipelineBarrier2(c, &dall);
    }
    VKCHECK(vkEndCommandBuffer(c));
    VkCommandBufferSubmitInfo csi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = c};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &csi};
    VKCHECK(vkQueueSubmit2(V.queue, 1, &si, VK_NULL_HANDLE)); VKCHECK(vkQueueWaitIdle(V.queue));
    for (int t = 0; t < 2; t++) {
        char line[256]; int n = snprintf(line, sizeof line, "[layer-test] %s:", t ? "3d-gs " : "arr-gs");
        for (int l = 0; l < NL; l++) { const uint8_t *p = rp + (size_t)t * W * W * 4 * NL + (size_t)l * W * W * 4 + (size_t)(8 * W + 8) * 4; n += snprintf(line + n, sizeof line - (size_t)n, " L%d=(%u %u %u %u)", l, p[0], p[1], p[2], p[3]); }
        LOGV("%s   (expected L0..3 R = 32 64 96 128, G 128, B 64)", line);
    }
    /* 3d-clr and 3d-1: the 3D image's slices through 2D views */
    VKCHECK(vkResetCommandPool(V.dev, cpool, 0)); memset(rp, 0xee, 2 * W * W * 4 * NL);
    VKCHECK(vkBeginCommandBuffer(c, &cbi));
    for (int s = 0; s < NL; s++) {
        VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v_slice[s], .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue.color.float32 = {(float)(s + 1) / 8.0f, 0.75f, 0, 1}};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, W}}, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
        vkCmdBeginRendering(c, &ri);
        if (s == 2) { vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[1]); vkCmdDraw(c, 3, 1, 0, 0); }
        vkCmdEndRendering(c); vkCmdPipelineBarrier2(c, &dall);
    }
    { VkBufferImageCopy cp = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, W, NL}}; vkCmdCopyImageToBuffer(c, img[1], VK_IMAGE_LAYOUT_GENERAL, rb, 1, &cp); vkCmdPipelineBarrier2(c, &dall); }
    VKCHECK(vkEndCommandBuffer(c));
    VKCHECK(vkQueueSubmit2(V.queue, 1, &si, VK_NULL_HANDLE)); VKCHECK(vkQueueWaitIdle(V.queue));
    { char line[256]; int n = snprintf(line, sizeof line, "[layer-test] 3d-clr/3d-1:");
      for (int l = 0; l < NL; l++) { const uint8_t *p = rp + (size_t)l * W * W * 4 + (size_t)(8 * W + 8) * 4; n += snprintf(line + n, sizeof line - (size_t)n, " S%d=(%u %u %u %u)", l, p[0], p[1], p[2], p[3]); }
      LOGV("%s   (expected S0,1,3 R = 32 64 _ 128 G 191 B 0; S2 drawn: R 32 G 128 B 64)", line); }
    vkDestroyCommandPool(V.dev, cpool, NULL);
    for (int g = 0; g < 2; g++) vkDestroyPipeline(V.dev, pipe[g], NULL);
    vkDestroyPipelineLayout(V.dev, pl, NULL);
    for (int i = 0; i < 3; i++) vkDestroyShaderModule(V.dev, m[i], NULL);
    for (int i = 0; i < nviews; i++) vkDestroyImageView(V.dev, views[i], NULL);
    for (int k = 0; k < 2; k++) vkDestroyImage(V.dev, img[k], NULL);
    vkDestroyBuffer(V.dev, rb, NULL);
    for (int k = 0; k < 3; k++) if (mem[k]) vkFreeMemory(V.dev, mem[k], NULL);
}
