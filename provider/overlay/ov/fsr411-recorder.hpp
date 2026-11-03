#pragma once
#include <cstdlib>
#include "fv_npu.hpp"  // Vela 0.8.3: full network on the NPU
#include "quality-recorder.hpp"

namespace fsr4 {
// 4.1.1 separates tensor-border clearing from each convolution. The root
// layouts below come from the selected INT8 provider in the official SDK DLL.
// Tables are rebased onto the same compact descriptor heaps as the 4.0.2 core.
inline constexpr std::array<uint32_t,17> k411TensorDivisors{
    2,1,2,2,4,4,4,8,8,8,4,4,2,2,1,1,1};
inline constexpr std::array<uint32_t,13> k411BorderDivisors{
    2,2,2,4,4,4,8,8,8,4,4,2,2};
inline constexpr std::array<const char*,27> k411PassNames{
    "pre", "border_0", "model_1", "border_1", "model_2", "border_2",
    "model_3", "border_3", "model_4", "border_4", "model_5", "border_5",
    "model_6", "border_6", "model_7", "border_7", "model_8", "border_8",
    "model_9", "border_9", "model_10", "border_10", "model_11", "border_11",
    "model_12", "border_12", "post"};

inline std::array<uint32_t,2> fsr411_dispatch(uint32_t pass, uint32_t width, uint32_t height,
                                            uint32_t capacity_width, uint32_t capacity_height) {
    if(pass>26 || width>capacity_width || height>capacity_height)
        throw std::invalid_argument("invalid 4.1.1 dispatch dimensions");
    if(pass==0 || pass==26)
        return {divide_up(width,pass==0 ? 16 : 32),divide_up(height,pass==0 ? 16 : 32)};
    // Neural tensors are padded to eight pixels; pre/post and their textures
    // retain the application's exact output dimensions in 4.1.1.
    width=round_up(width,8); height=round_up(height,8);
    if(pass%2==1) {
        const auto divisor=k411BorderDivisors[pass/2];
        const auto w=width/divisor,h=height/divisor;
        const auto px=std::min(capacity_width/divisor+1-w,5u);
        const auto py=std::min(capacity_height/divisor+1-h,1u);
        return {divide_up((w+1+px)*(1+py)+h*(1+px),32),1};
    }
    const auto model=pass/2;
    const auto divisor=(model<=2 || model==12) ? 2u : (model<=5 || model>=10) ? 4u : 8u;
    return {divide_up(width/divisor,64),height/divisor};
}

// netg + begin_frame once per frame (the context calls it before a TAIL; record_fsr411_frame again is a no-op)
inline void fv_prepare(fv::Bridge* fvb, uint32_t width, uint32_t height, uint32_t capacity_width, uint32_t capacity_height) {
    if(!fvb || fvb->begun) return;
    for(uint32_t j=2;j<=26;++j){ const auto g=fsr411_dispatch(j,width,height,capacity_width,capacity_height); fvb->netg[j-1][0]=g[0]; fvb->netg[j-1][1]=g[1]; }
    fvb->begin_frame(width,height,capacity_width,capacity_height);
}
// part: 0 the whole frame, 1 HEAD (pre + border_0 + HTP kick) after a TAIL, 4 HEAD without a TAIL (zero-length stamps for passes 2..26),
// 2 TAIL of the previous frame (passes 2..26, its constants), 3 only the TAIL's history/recurrent/reprojected layout transitions
inline void record_fsr411_frame(
    VkCommandBuffer command_buffer, VkPipelineLayout pipeline_layout,
    std::span<const VkPipeline> pipelines,
    const std::array<VkDeviceAddress,3>& descriptor_addresses,
    VkDeviceAddress constants_address, VkImage history, VkImage recurrent,
    VkImage reprojected,
    PFN_vkCmdBindDescriptorBuffersEXT bind_descriptor_buffers,
    PFN_vkCmdSetDescriptorBufferOffsetsEXT set_descriptor_offsets,
    PFN_vkCmdBindDescriptorBufferEmbeddedSamplersEXT bind_embedded_samplers,
    VkQueryPool timestamps, uint32_t timestamp_base,
    uint32_t width, uint32_t height, uint32_t capacity_width, uint32_t capacity_height,
    PFN_vkCmdPipelineBarrier2 pipeline_barrier2,
    PFN_vkCmdWriteTimestamp2 write_timestamp2,
    VkPipeline post_k2=VK_NULL_HANDLE, VkDeviceAddress inter_address=0,
    fv::Bridge* fvb=nullptr, VkDeviceAddress fv_sadr=0, int part=0, bool fx=false, uint32_t k2t=0) {
    bool fv_ind=false;
    // History = L (K2 samples it, already READ_ONLY), reprojected = O (K2 writes it, already GENERAL): before K2 only recurrent moves;
    // a dropped TAIL (part 3) moves all three as before (L -> GENERAL, O -> READ_ONLY = the state after a K2 + swap)
    const auto tail_layouts=[&](bool all) {
        for (auto image : {history,recurrent}) if(all || image==recurrent)
            image_barrier(pipeline_barrier2,command_buffer,image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        if(all) image_barrier(pipeline_barrier2,command_buffer,reprojected,VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT); };
    if(part==3){ tail_layouts(true); return; }
    if(part!=2) fv_prepare(fvb,width,height,capacity_width,capacity_height);
    if (pipelines.size()!=27) throw std::invalid_argument("4.1.1 requires 27 model passes");
    std::array<VkDescriptorBufferBindingInfoEXT,3> bindings{};
    for (size_t i=0;i<3;++i) {
        bindings[i].sType=VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT;
        bindings[i].address=descriptor_addresses[i];
        bindings[i].usage=i==0 ? VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT :
                                VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
    }
    bind_descriptor_buffers(command_buffer,3,bindings.data());
    const uint32_t indices[]{0,1,2}; const VkDeviceSize offsets[]{0,0,0};
    set_descriptor_offsets(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,3,indices,offsets);
    bind_embedded_samplers(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,3);
    const uint32_t i0=part==2 ? 2u : 0u, i1=(part==1 || part==4) ? 2u : 27u;
    // Slim HEAD (no X0, no border_0) / slim TAIL (no fallback block, K2b on the other k1 slot); see fv_npu.hpp fp_allow()
    bool fp_hs=fvb && (part==1 || part==4) && fvb->fp_allow(); const bool fp_ts=fvb && part==2 && fvb->fp_prev_head;
    if(fvb && part!=2) fvb->fp_prev_head=false;
    for (uint32_t i=i0;i<i1;++i) {
        if (i==26) tail_layouts(!fx);
        if(fvb && i==2) fv_ind=part==2 ? fvb->tail_net(command_buffer) : fvb->before_net(command_buffer,fv_sadr);
        if((fp_hs && i==1) || (fp_ts && fv_ind && fvb->k1h_frame && i>=2 && i<=25)) {   // Skipped pass, timestamps kept
            if(timestamps){ write_timestamp2(command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,timestamps,timestamp_base+i*2);
                            write_timestamp2(command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,timestamps,timestamp_base+i*2+1); }
            if(i==2) ++fvb->fp_tails;
            if(i==24) fvb->after_net(command_buffer,fv_sadr);
            continue; }
        const bool border=i%2==1;
        const bool main=i==0 || i==26;
        const bool root=main || border || (i!=6 && i!=12 && i!=14 && i!=16);
        std::array<uint32_t,4> push{};
        uint32_t count=0;
        if(root) {
            auto address=constants_address+(main ? 0 : 1024);
            push[count++]=uint32_t(address); push[count++]=uint32_t(address>>32);
        }
        push[count++]=i==26 ? 51 : 0;
        if(!border) push[count++]=i==26 ? 63 : 12;
        const auto groups=fsr411_dispatch(i,width,height,capacity_width,capacity_height);
        const bool split=(i==26 && post_k2!=VK_NULL_HANDLE);
        if(split){ const bool kh=fv_ind && fvb && fvb->k1h_frame;   // K1 (fallback only) and K2b always use the scratch InterBuf; K2a the HTP k1 slot of N-1
            const VkDeviceAddress ia=inter_address;
            std::array<uint32_t,6> p6{push[0],push[1],push[2],push[3],uint32_t(ia),uint32_t(ia>>32)};
            vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[i]);
            vkCmdPushConstants(command_buffer,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,24,p6.data());
            if(timestamps)write_timestamp2(command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,timestamps,timestamp_base+i*2);
            static const bool skip1=std::getenv("FSR4_X_SKIP1")!=nullptr, skip2=std::getenv("FSR4_X_SKIP2")!=nullptr;
            const bool fp_sk=fp_ts && fv_ind && fvb->k1h_frame;   // no K1 (no GPU network ran); K2b reads k1(N-2)
            if(fp_sk){} else if(!skip1){ if(fv_ind && fvb->k1h_frame) vkCmdDispatchIndirect(command_buffer,fvb->ab,fvb->arg_off(26)); else vkCmdDispatch(command_buffer,groups[0],groups[1],1); }   // 0 groups when HTP k1 is valid
            if(!fp_sk) compute_barrier(pipeline_barrier2,command_buffer);
            if(fp_sk){ const VkDeviceAddress oa=fvb->k1_other(); p6[4]=uint32_t(oa); p6[5]=uint32_t(oa>>32); }
            if(fvb && !(fv_ind && fvb->k1h_frame) && fvb->dump(command_buffer,2,ia,(width/2)*(height/2)))   // Debug dump: K1 (InterBuf)
                vkCmdPushConstants(command_buffer,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,24,p6.data());
            vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,post_k2);
            if(timestamps)write_timestamp2(command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,timestamps,timestamp_base+54);
            if(!skip2){ if(kh){ if(fvb->k1_addr){ std::array<uint32_t,6> pa{push[0],push[1],push[2],push[3],uint32_t(fvb->k1_addr),uint32_t(fvb->k1_addr>>32)};
                    vkCmdPushConstants(command_buffer,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,24,pa.data());
                    vkCmdDispatchIndirect(command_buffer,fvb->ab,fvb->arg_off(1));   // K2a: groups only when WAIT(N-1) is OK
                    vkCmdPushConstants(command_buffer,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,24,p6.data()); }
                  vkCmdDispatchIndirect(command_buffer,fvb->ab,fvb->arg_off(27)); }   // K2b: groups only on fallback (or no k1 slot yet)
                else vkCmdDispatch(command_buffer,k2t ? divide_up(width/2,k2t) : groups[0],k2t ? divide_up(height/2,k2t) : groups[1],1); }   // 14x14-block K2 tiles
            if(timestamps)write_timestamp2(command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,timestamps,timestamp_base+55);
            if(timestamps)write_timestamp2(command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,timestamps,timestamp_base+i*2+1);
            continue;
        }
        vkCmdBindPipeline(command_buffer,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[i]);
        if(i==0){ const VkDeviceAddress pd0=fvb?fvb->pf_din():0; if(!pd0) fp_hs=false; const VkDeviceAddress pd=pd0 | (fp_hs ? 1u : 0u);   // Bit 0 = pre skips X0
            if(fvb && (part==1 || part==4)) { fvb->fp_prev_head=fp_hs; if(fp_hs) ++fvb->fp_heads; }   // pass-01 (fused) always gets 24 bytes; 0 = no HTP-input store
            std::array<uint32_t,6> q6{push[0],push[1],push[2],push[3],uint32_t(pd),uint32_t(pd>>32)};
            vkCmdPushConstants(command_buffer,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,24,q6.data()); }
        else vkCmdPushConstants(command_buffer,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,count*4,push.data());
        if(timestamps)write_timestamp2(command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,timestamps,timestamp_base+i*2);
        if(fv_ind && ((i>=2 && i<=24) || (i==25 && fvb->k1h_frame))) vkCmdDispatchIndirect(command_buffer,fvb->ab,fvb->arg_off(i));
        else vkCmdDispatch(command_buffer,groups[0],groups[1],1);
#if defined(FSR4_RESEARCH_CAPTURE)
        fsr4capture::after_pass(command_buffer,i+1);
#endif
        if(timestamps)write_timestamp2(command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,timestamps,timestamp_base+i*2+1);
        if(timestamps && i==26) { write_timestamp2(command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,timestamps,timestamp_base+54);
            write_timestamp2(command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,timestamps,timestamp_base+55); }
        if(i!=26)compute_barrier(pipeline_barrier2,command_buffer);
        if(fvb && (i==1 || i==24)) fvb->dump(command_buffer,i==1 ? 0 : 1,fv_sadr,fv::kRegionVec);   // Debug dump: X0 / OUT (region 0)
        if(fvb && i==24) fvb->after_net(command_buffer,fv_sadr);
    }
    if(part==2) return;
    if(part==1 || part==4) {
        if(timestamps && part==4) for(uint32_t q=4;q<56;++q) write_timestamp2(command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,timestamps,timestamp_base+q);
        if(fvb) fvb->head_kick(command_buffer,fv_sadr);
        if(fvb) fvb->end_frame();
        return;
    }
    if(fvb) fvb->after_post(command_buffer);   // Late NPU start (HTP kick after post)
    if(fvb) fvb->end_frame();
}
}
