#include "ui_synth.h"
#include "ui_diff_spv.h"
#include "ui_smooth_spv.h"
#include "config.h"
#include "log.h"
#include <mutex>
#include <unordered_map>

namespace fgvk {
namespace {
constexpr uint32_t RingCount=3;
VkDevice device{}; VkPhysicalDevice physical{}; PFN_vkGetDeviceProcAddr gdpa{};
VkFormat format{}; VkExtent2D extent{};
VkImage staging[RingCount]{}; VkDeviceMemory stagingMemory[RingCount]{}; VkImageView stagingView[RingCount]{}; bool stagingInitialized[RingCount]{};
VkSampler sampler{}; VkDescriptorSetLayout setLayout{}; VkDescriptorPool pool{}; VkDescriptorSet sets[RingCount]{};
VkPipelineLayout pipelineLayout{}; VkPipeline pipeline{};
// Temporal smoothing (XeSSSynthSmooth): one persistent accumulator image + second pipeline
// (raw ring slot sampled + accumulator storage -> accumulator). emaWeight==0 disables it.
VkImage accum{}; VkDeviceMemory accumMemory{}; VkImageView accumView{}; bool accumInitialized{};
VkPipelineLayout smoothLayout{}; VkPipeline smoothPipeline{}; VkDescriptorSet smoothSets[RingCount]{};
float emaWeight{}; bool debugTint{}; // XeSSSynthDebugTint: magenta-paint the layer (SDK consumption proof)
uint32_t ringCursor{}; bool failedLogged{}; bool successLogged{};
std::unordered_map<VkImage,VkImageView> inputViews;
std::mutex gate;

template<class T> T D(const char* n){return gdpa&&device?reinterpret_cast<T>(gdpa(device,n)):nullptr;}
void FailOnce(const char* message){if(!failedLogged){failedLogged=true;Log("Synthetic UI layer unavailable: %s",message);}}

void DestroyAll(){
  if(device&&gdpa){
    auto destroyView=D<PFN_vkDestroyImageView>("vkDestroyImageView");
    auto destroySampler=D<PFN_vkDestroySampler>("vkDestroySampler");
    auto destroyPool=D<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    auto destroyLayout=D<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout");
    auto destroyPipeline=D<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    auto destroyPipelineLayout=D<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout");
    auto destroyImage=D<PFN_vkDestroyImage>("vkDestroyImage");
    auto freeMemory=D<PFN_vkFreeMemory>("vkFreeMemory");
    if(destroyView){for(auto& item:inputViews)if(item.second)destroyView(device,item.second,nullptr);for(auto& v:stagingView)if(v)destroyView(device,v,nullptr);if(accumView)destroyView(device,accumView,nullptr);}
    if(destroySampler&&sampler)destroySampler(device,sampler,nullptr);
    if(destroyPool&&pool)destroyPool(device,pool,nullptr);
    if(destroyPipeline&&pipeline)destroyPipeline(device,pipeline,nullptr);
    if(destroyPipeline&&smoothPipeline)destroyPipeline(device,smoothPipeline,nullptr);
    if(destroyPipelineLayout&&pipelineLayout)destroyPipelineLayout(device,pipelineLayout,nullptr);
    if(destroyPipelineLayout&&smoothLayout)destroyPipelineLayout(device,smoothLayout,nullptr);
    if(destroyLayout&&setLayout)destroyLayout(device,setLayout,nullptr);
    for(uint32_t i=0;i<RingCount;++i){if(destroyImage&&staging[i])destroyImage(device,staging[i],nullptr);if(freeMemory&&stagingMemory[i])freeMemory(device,stagingMemory[i],nullptr);}
    if(destroyImage&&accum)destroyImage(device,accum,nullptr);
    if(freeMemory&&accumMemory)freeMemory(device,accumMemory,nullptr);
  }
  inputViews.clear();sampler={};setLayout={};pool={};pipeline={};pipelineLayout={};
  smoothPipeline={};smoothLayout={};accum={};accumMemory={};accumView={};accumInitialized=false;emaWeight=0;
  for(uint32_t i=0;i<RingCount;++i){staging[i]={};stagingMemory[i]={};stagingView[i]={};stagingInitialized[i]=false;sets[i]={};smoothSets[i]={};}
}

bool CreateStaging(uint32_t i){
  auto create=D<PFN_vkCreateImage>("vkCreateImage");auto getReq=D<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements");
  auto allocate=D<PFN_vkAllocateMemory>("vkAllocateMemory");auto bind=D<PFN_vkBindImageMemory>("vkBindImageMemory");
  auto createView=D<PFN_vkCreateImageView>("vkCreateImageView");
  if(!create||!getReq||!allocate||!bind||!createView)return false;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R8G8B8A8_UNORM;
  ci.extent={extent.width,extent.height,1};ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
  ci.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
  if(create(device,&ci,nullptr,&staging[i])!=VK_SUCCESS)return false;
  VkMemoryRequirements req{};getReq(device,staging[i],&req);
  uint32_t type=UINT32_MAX;for(uint32_t k=0;k<32;++k)if(req.memoryTypeBits&(1u<<k)){type=k;break;}
  if(type==UINT32_MAX)return false;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
  if(allocate(device,&ai,nullptr,&stagingMemory[i])!=VK_SUCCESS||bind(device,staging[i],stagingMemory[i],0)!=VK_SUCCESS)return false;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=staging[i];vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=VK_FORMAT_R8G8B8A8_UNORM;
  vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};return createView(device,&vi,nullptr,&stagingView[i])==VK_SUCCESS;
}

// Persistent EMA accumulator image. Same layout/usage family as the ring slots, but it also
// needs TRANSFER_SRC because with smoothing on it replaces the ring slot as the copy source.
bool CreateAccum(){
  auto create=D<PFN_vkCreateImage>("vkCreateImage");auto getReq=D<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements");
  auto allocate=D<PFN_vkAllocateMemory>("vkAllocateMemory");auto bind=D<PFN_vkBindImageMemory>("vkBindImageMemory");
  auto createView=D<PFN_vkCreateImageView>("vkCreateImageView");
  if(!create||!getReq||!allocate||!bind||!createView)return false;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R8G8B8A8_UNORM;
  ci.extent={extent.width,extent.height,1};ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
  ci.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
  if(create(device,&ci,nullptr,&accum)!=VK_SUCCESS)return false;
  VkMemoryRequirements req{};getReq(device,accum,&req);
  uint32_t type=UINT32_MAX;for(uint32_t k=0;k<32;++k)if(req.memoryTypeBits&(1u<<k)){type=k;break;}
  if(type==UINT32_MAX)return false;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
  if(allocate(device,&ai,nullptr,&accumMemory)!=VK_SUCCESS||bind(device,accum,accumMemory,0)!=VK_SUCCESS)return false;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=accum;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=VK_FORMAT_R8G8B8A8_UNORM;
  vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};return createView(device,&vi,nullptr,&accumView)==VK_SUCCESS;
}

bool CreateSmoothPipeline(){
  auto createPL=D<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout");
  auto createShader=D<PFN_vkCreateShaderModule>("vkCreateShaderModule");auto destroyShader=D<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
  auto createPipelines=D<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
  if(!createPL||!createShader||!destroyShader||!createPipelines)return false;
  // Reuses the shared set layout (bindings 0/1 sampled, 2 storage). Binding 1 stays bound to the
  // HUD-less view as a valid placeholder; the smooth shader never samples it.
  VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,20};VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount=1;pli.pSetLayouts=&setLayout;pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&range;
  if(createPL(device,&pli,nullptr,&smoothLayout)!=VK_SUCCESS)return false;
  VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smi.codeSize=UiSmoothSpirvSize;smi.pCode=UiSmoothSpirv;
  VkShaderModule shader{};if(createShader(device,&smi,nullptr,&shader)!=VK_SUCCESS)return false;
  VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;stage.module=shader;stage.pName="main";
  VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pci.stage=stage;pci.layout=smoothLayout;
  VkResult result=createPipelines(device,VK_NULL_HANDLE,1,&pci,nullptr,&smoothPipeline);destroyShader(device,shader,nullptr);return result==VK_SUCCESS;
}

VkImageView GetView(VkImage image){
  auto found=inputViews.find(image);if(found!=inputViews.end())return found->second;
  auto create=D<PFN_vkCreateImageView>("vkCreateImageView");if(!create)return VK_NULL_HANDLE;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;
  vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkImageView view{};
  if(create(device,&vi,nullptr,&view)!=VK_SUCCESS)return VK_NULL_HANDLE;
  inputViews.emplace(image,view);return view;
}

bool CreatePipeline(){
  auto createSetLayout=D<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout");
  auto createPool=D<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool");auto allocSets=D<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets");
  auto createSampler=D<PFN_vkCreateSampler>("vkCreateSampler");auto createPL=D<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout");
  auto createShader=D<PFN_vkCreateShaderModule>("vkCreateShaderModule");auto destroyShader=D<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
  auto createPipelines=D<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
  if(!createSetLayout||!createPool||!allocSets||!createSampler||!createPL||!createShader||!destroyShader||!createPipelines)return false;
  VkDescriptorSetLayoutBinding bindings[3]{};
  for(uint32_t i=0;i<2;++i){bindings[i].binding=i;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
  bindings[2].binding=2;bindings[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;bindings[2].descriptorCount=1;bindings[2].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sli.bindingCount=3;sli.pBindings=bindings;
  if(createSetLayout(device,&sli,nullptr,&setLayout)!=VK_SUCCESS)return false;
  // Two sets per ring slot: the diff pass and (when smoothing is on) the EMA pass. The pool is
  // sized for both even when smoothing is off — descriptor pools are cheap and this avoids a
  // second initialization branch.
  VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,4*RingCount},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2*RingCount}};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpi.maxSets=2*RingCount;dpi.poolSizeCount=2;dpi.pPoolSizes=sizes;
  if(createPool(device,&dpi,nullptr,&pool)!=VK_SUCCESS)return false;
  VkDescriptorSetLayout layouts[2*RingCount]{setLayout,setLayout,setLayout,setLayout,setLayout,setLayout};
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dai.descriptorPool=pool;dai.descriptorSetCount=2*RingCount;dai.pSetLayouts=layouts;
  VkDescriptorSet allocated[2*RingCount]{};
  if(allocSets(device,&dai,allocated)!=VK_SUCCESS)return false;
  for(uint32_t i=0;i<RingCount;++i){sets[i]=allocated[i];smoothSets[i]=allocated[RingCount+i];}
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;si.maxLod=0;
  if(createSampler(device,&si,nullptr,&sampler)!=VK_SUCCESS)return false;
  VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,16};VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount=1;pli.pSetLayouts=&setLayout;pli.pushConstantRangeCount=1;pli.pPushConstantRanges=&range;
  if(createPL(device,&pli,nullptr,&pipelineLayout)!=VK_SUCCESS)return false;
  VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};smi.codeSize=UiDiffSpirvSize;smi.pCode=UiDiffSpirv;
  VkShaderModule shader{};if(createShader(device,&smi,nullptr,&shader)!=VK_SUCCESS)return false;
  VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;stage.module=shader;stage.pName="main";
  VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pci.stage=stage;pci.layout=pipelineLayout;
  VkResult result=createPipelines(device,VK_NULL_HANDLE,1,&pci,nullptr,&pipeline);destroyShader(device,shader,nullptr);return result==VK_SUCCESS;
}

void Barrier(VkCommandBuffer cmd,VkImage image,VkImageLayout oldLayout,VkImageLayout newLayout,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags srcStage,VkPipelineStageFlags dstStage,uint32_t srcFamily,uint32_t dstFamily){
  auto fn=D<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");if(!fn)return;
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=src;b.dstAccessMask=dst;b.oldLayout=oldLayout;b.newLayout=newLayout;
  b.srcQueueFamilyIndex=srcFamily;b.dstQueueFamilyIndex=dstFamily;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  fn(cmd,srcStage,dstStage,0,0,nullptr,0,nullptr,1,&b);
}
}

void SynthUiInit(VkDevice d,VkPhysicalDevice pd,PFN_vkGetDeviceProcAddr getProc,VkFormat fmt,VkExtent2D size){
  std::lock_guard<std::mutex> lock(gate);DestroyAll();device=d;physical=pd;gdpa=getProc;format=fmt;extent=size;failedLogged=false;successLogged=false;ringCursor=0;
  const int smooth=Cfg().xessSynthSmooth;emaWeight=smooth<=0?0.f:(smooth>100?.35f:(float)smooth/100.f);
  debugTint=Cfg().xessSynthDebugTint;
  if((fmt!=VK_FORMAT_R8G8B8A8_UNORM&&fmt!=VK_FORMAT_B8G8R8A8_UNORM)||!size.width||!size.height){FailOnce("only 8-bit UNORM SDR backbuffers are supported");return;}
  for(uint32_t i=0;i<RingCount;++i)if(!CreateStaging(i)){DestroyAll();FailOnce("staging allocation failed");return;}
  if(!CreatePipeline()){DestroyAll();FailOnce("Vulkan pipeline creation failed");return;}
  // Smoothing is an enhancement, not a requirement: on any failure we log once and continue
  // with the raw per-frame layer instead of taking the whole synthetic UI down with it.
  if(emaWeight>0&&(!CreateAccum()||!CreateSmoothPipeline())){emaWeight=0;if(smoothPipeline==VK_NULL_HANDLE&&accum)Log("Synthetic UI smoothing unavailable; continuing with the raw layer");accumInitialized=false;}
}

void SynthUiDestroy(){std::lock_guard<std::mutex> lock(gate);DestroyAll();device={};physical={};gdpa=nullptr;format=VK_FORMAT_UNDEFINED;extent={};}

bool SynthUiAvailable(){std::lock_guard<std::mutex> lock(gate);return pipeline!=VK_NULL_HANDLE;}

void SynthUiReset(){std::lock_guard<std::mutex> lock(gate);accumInitialized=false;}

bool SynthUiRecord(VkCommandBuffer cmd,VkImage presented,VkImage hudless,VkImage target,uint32_t family,bool targetFirstUse){
  std::lock_guard<std::mutex> lock(gate);
  if(!pipeline||!cmd||!presented||!hudless||!target)return false;
  VkImageView pv=GetView(presented),hv=GetView(hudless);
  if(!pv||!hv){FailOnce("input image view creation failed");return false;}
  const uint32_t slot=ringCursor++%RingCount;
  auto update=D<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets");auto bindPipeline=D<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
  auto bindSets=D<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");auto push=D<PFN_vkCmdPushConstants>("vkCmdPushConstants");
  auto dispatch=D<PFN_vkCmdDispatch>("vkCmdDispatch");auto copy=D<PFN_vkCmdCopyImage>("vkCmdCopyImage");
  if(!update||!bindPipeline||!bindSets||!push||!dispatch||!copy){FailOnce("required Vulkan commands are unavailable");return false;}
  VkDescriptorImageInfo images[3]{{sampler,pv,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{sampler,hv,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{VK_NULL_HANDLE,stagingView[slot],VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet writes[3]{};for(uint32_t i=0;i<3;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=sets[slot];writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=i==2?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[i].pImageInfo=&images[i];}
  update(device,3,writes,0,nullptr);
  // Inputs: the presented frame lives in GENERAL between the bridge's own transfers; the HUD-less
  // capture was left in SHADER_READ_ONLY by RecordColor. Output staging cycles like the mask module.
  // With smoothing on, a used slot ends in SHADER_READ_ONLY (sampled by the EMA pass) instead of
  // TRANSFER_SRC, so the next cycle-out of that slot must start from the matching layout.
  Barrier(cmd,presented,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED);
  const bool ema=emaWeight>0;
  const VkImageLayout slotOld=ema?(stagingInitialized[slot]?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED)
                                 :(stagingInitialized[slot]?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED);
  const VkAccessFlags slotOldAccess=ema?(stagingInitialized[slot]?VK_ACCESS_SHADER_READ_BIT:VkAccessFlags(0))
                                        :(stagingInitialized[slot]?VK_ACCESS_TRANSFER_READ_BIT:VkAccessFlags(0));
  Barrier(cmd,staging[slot],slotOld,VK_IMAGE_LAYOUT_GENERAL,
          slotOldAccess,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED);
  // Target is a shared (DX-imported) image: family handoff mirrors the transport image exactly.
  Barrier(cmd,target,targetFirstUse?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_QUEUE_FAMILY_EXTERNAL,family);
  bindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);bindSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&sets[slot],0,nullptr);
  uint32_t params[4]={extent.width,extent.height,format==VK_FORMAT_B8G8R8A8_UNORM?1u:0u,(uint32_t)debugTint};push(cmd,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(params),params);
  dispatch(cmd,(extent.width+7)/8,(extent.height+7)/8,1);
  // With smoothing the raw layer stays GENERAL and the EMA pass samples it afterwards; without
  // smoothing the ring slot goes straight to the copy source as before.
  VkImage copySource=staging[slot];
  if(!ema){
    Barrier(cmd,staging[slot],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED);
  }else{
    Barrier(cmd,staging[slot],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED);
    // The accumulator persists across frames: settle layout from its last role (EMA write after
    // the copy read) back to GENERAL, or transition UNDEFINED on the very first use.
    Barrier(cmd,accum,accumInitialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,
            accumInitialized?VkAccessFlags(VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT):VkAccessFlags(0),
            VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
            accumInitialized?VkPipelineStageFlags(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT):VkPipelineStageFlags(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED);
    VkDescriptorImageInfo smoothInfos[3]{{sampler,stagingView[slot],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{sampler,hv,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{VK_NULL_HANDLE,accumView,VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet smoothWrites[3]{};for(uint32_t i=0;i<3;++i){smoothWrites[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;smoothWrites[i].dstSet=smoothSets[slot];smoothWrites[i].dstBinding=i;smoothWrites[i].descriptorCount=1;smoothWrites[i].descriptorType=i==2?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;smoothWrites[i].pImageInfo=&smoothInfos[i];}
    update(device,3,smoothWrites,0,nullptr);
    bindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,smoothPipeline);bindSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,smoothLayout,0,1,&smoothSets[slot],0,nullptr);
    struct SmoothParams{uint32_t width,height,swizzle;float weight;uint32_t firstFrame;} sp{extent.width,extent.height,format==VK_FORMAT_B8G8R8A8_UNORM?1u:0u,emaWeight,accumInitialized?0u:1u};
    push(cmd,smoothLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(sp),&sp);
    dispatch(cmd,(extent.width+7)/8,(extent.height+7)/8,1);
    Barrier(cmd,accum,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED);
    copySource=accum;
  }
  Barrier(cmd,presented,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_SHADER_READ_BIT,
          VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED);
  VkImageCopy region{};region.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.dstSubresource=region.srcSubresource;region.extent={extent.width,extent.height,1};
  copy(cmd,copySource,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
  Barrier(cmd,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,0,
          VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,family,VK_QUEUE_FAMILY_EXTERNAL);
  if(ema)Barrier(cmd,accum,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_READ_BIT,0,
          VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED);
  stagingInitialized[slot]=true;accumInitialized=true;
  if(!successLogged){successLogged=true;Log("Synthetic UI layer first frame: %ux%u format=%u (premultiplied B-(1-a)*C, ring=%u, smoothing=%d, tint=%d)",extent.width,extent.height,(uint32_t)format,RingCount,ema?1:0,(int)debugTint);}
  return true;
}
}
