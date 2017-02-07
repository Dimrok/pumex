#include <random>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/simd/matrix.h>
#include <gli/gli.hpp>
#include <pumex/Pumex.h>
#include <pumex/AssetLoaderAssimp.h>
#include <tbb/tbb.h>

#define CROWD_MEASURE_TIME 1

const uint32_t MAX_BONES = 63;

struct PositionData
{
  PositionData(const glm::mat4& p)
    : position{p}
  {
  }
  glm::mat4 position;
  glm::mat4 bones[MAX_BONES];
};

struct InstanceData
{
  InstanceData(uint32_t p, uint32_t t, uint32_t m, uint32_t i)
    : positionIndex{ p }, typeID{ t }, materialVariant{ m }, mainInstance {i}
  {
  }
  uint32_t positionIndex;
  uint32_t typeID;
  uint32_t materialVariant;
  uint32_t mainInstance;
};

struct InstanceDataCPU
{
  InstanceDataCPU(uint32_t a, const glm::vec2& p, float r, float s, float t, float o)
    : animation{ a }, position{ p }, rotation{ r }, speed{ s }, time2NextTurn{ t }, animationOffset{o}
  {
  }
  uint32_t  animation;
  glm::vec2 position;
  float     rotation;
  float     speed;
  float     time2NextTurn;
  float     animationOffset;
};

struct MaterialData
{
  glm::vec4 ambient;
  glm::vec4 diffuse;
  glm::vec4 specular;
  float     shininess;
  uint32_t  diffuseTextureIndex = 0;
  uint32_t  std430pad0;
  uint32_t  std430pad1;

  // two functions that define material parameters according to data from an asset's material 
  void registerProperties(const pumex::Material& material)
  {
    ambient   = material.getProperty("$clr.ambient", glm::vec4(0, 0, 0, 0));
    diffuse   = material.getProperty("$clr.diffuse", glm::vec4(1, 1, 1, 1));
    specular  = material.getProperty("$clr.specular", glm::vec4(0, 0, 0, 0));
    shininess = material.getProperty("$mat.shininess", glm::vec4(0, 0, 0, 0)).r;
  }
  void registerTextures(const std::map<pumex::TextureSemantic::Type, uint32_t>& textureIndices)
  {
    auto it = textureIndices.find(pumex::TextureSemantic::Diffuse);
    diffuseTextureIndex = (it == textureIndices.end()) ? 0 : it->second;
  }

};

struct SkelAnimKey
{
  SkelAnimKey(uint32_t s, uint32_t a)
    : skelID{ s }, animID{ a }
  {
  }
  uint32_t skelID;
  uint32_t animID;
};

inline bool operator<(const SkelAnimKey& lhs, const SkelAnimKey& rhs)
{
  if (lhs.skelID != rhs.skelID)
    return lhs.animID < rhs.animID;
  return lhs.skelID < rhs.skelID;
}

struct ApplicationData
{
  std::weak_ptr<pumex::Viewer>                         viewer;
  uint32_t                                             renderMethod;

  glm::vec3                                            minArea;
  glm::vec3                                            maxArea;
  std::vector<pumex::Skeleton>                         skeletons;
  std::vector<pumex::Animation>                        animations;
  std::map<SkelAnimKey, std::vector<uint32_t>>         skelAnimBoneMapping;
  std::vector<float>                                   animationSpeed;

  std::default_random_engine                           randomEngine;
  std::exponential_distribution<float>                 randomTime2NextTurn;
  std::uniform_real_distribution<float>                randomRotation;
  std::uniform_int_distribution<uint32_t>              randomAnimation;

  std::vector<PositionData>                            positionData;
  std::vector<InstanceData>                            instanceData;
  std::vector<InstanceDataCPU>                         instanceDataCPU;

  std::shared_ptr<pumex::AssetBuffer>                  skeletalAssetBuffer;
  std::shared_ptr<pumex::TextureRegistryArray>         textureRegistryArray;
  std::shared_ptr<pumex::MaterialSet<MaterialData>>    materialSet;

  std::shared_ptr<pumex::UniformBuffer<pumex::Camera>>                      cameraUbo;
  std::shared_ptr<pumex::StorageBuffer<PositionData>>                       positionSbo;
  std::shared_ptr<pumex::StorageBuffer<InstanceData>>                       instanceSbo;
  std::shared_ptr<pumex::StorageBuffer<pumex::DrawIndexedIndirectCommand>>  resultsSbo;
  std::shared_ptr<pumex::StorageBuffer<pumex::DrawIndexedIndirectCommand>>  resultsSbo2;
  std::vector<uint32_t>                                                     resultsGeomToType;
  std::shared_ptr<pumex::StorageBuffer<uint32_t>>                           offValuesSbo;

  std::shared_ptr<pumex::RenderPass>                   defaultRenderPass;

  std::shared_ptr<pumex::PipelineCache>                pipelineCache;

  std::shared_ptr<pumex::DescriptorSetLayout>          simpleRenderDescriptorSetLayout;
  std::shared_ptr<pumex::PipelineLayout>               simpleRenderPipelineLayout;
  std::shared_ptr<pumex::GraphicsPipeline>             simpleRenderPipeline;
  std::shared_ptr<pumex::DescriptorPool>               simpleRenderDescriptorPool;
  std::shared_ptr<pumex::DescriptorSet>                simpleRenderDescriptorSet;

  std::shared_ptr<pumex::DescriptorSetLayout>          instancedRenderDescriptorSetLayout;
  std::shared_ptr<pumex::PipelineLayout>               instancedRenderPipelineLayout;
  std::shared_ptr<pumex::GraphicsPipeline>             instancedRenderPipeline;
  std::shared_ptr<pumex::DescriptorPool>               instancedRenderDescriptorPool;
  std::shared_ptr<pumex::DescriptorSet>                instancedRenderDescriptorSet;

  std::shared_ptr<pumex::DescriptorSetLayout>          filterDescriptorSetLayout;
  std::shared_ptr<pumex::PipelineLayout>               filterPipelineLayout;
  std::shared_ptr<pumex::ComputePipeline>              filterPipeline;
  std::shared_ptr<pumex::DescriptorPool>               filterDescriptorPool;
  std::shared_ptr<pumex::DescriptorSet>                filterDescriptorSet;

  std::shared_ptr<pumex::QueryPool>                    timeStampQueryPool;


  ApplicationData(std::shared_ptr<pumex::Viewer> v)
	  : viewer{ v }, renderMethod{ 1 }, randomTime2NextTurn{ 0.25 }, randomRotation{ -180.0f, 180.0f }
  {
  }

  void setup(const glm::vec3& minAreaParam, const glm::vec3& maxAreaParam, float objectDensity)
  {
    minArea = minAreaParam;
    maxArea = maxAreaParam;
    std::shared_ptr<pumex::Viewer> viewerSh = viewer.lock();
    CHECK_LOG_THROW (viewerSh.get() == nullptr, "Cannot acces pumex viewer");

    pumex::AssetLoaderAssimp loader;

    std::vector<std::string> animationFileNames
    {
      "wmale1_bbox.dae",
      "wmale1_walk.dae",
      "wmale1_walk_easy.dae",
      "wmale1_walk_big_steps.dae",
      "wmale1_run.dae"
    };
    animationSpeed =  // in meters per sec
    {
      0.0f,
      1.0f,
      0.8f,
      1.2f,
      2.0f
    };

    // We assume that animations use the same skeleton as skeletal models
    for (uint32_t i = 0; i < animationFileNames.size(); ++i)
    {
      std::string fullAssetFileName = viewerSh->getFullFilePath(animationFileNames[i]);
      if (fullAssetFileName.empty())
      {
        LOG_WARNING << "Cannot find asset : " << animationFileNames[i] << std::endl;
        continue;
      }
      std::shared_ptr<pumex::Asset> asset(loader.load(fullAssetFileName,true));
      if (asset.get() == nullptr)
      {
        LOG_WARNING << "Cannot load asset : " << fullAssetFileName << std::endl;
        continue;
      }
      animations.push_back(asset->animations[0]);
    }

	randomAnimation = std::uniform_int_distribution<uint32_t>(1, animations.size() - 1);

    std::vector<pumex::VertexSemantic> vertexSemantic = { { pumex::VertexSemantic::Position, 3 }, { pumex::VertexSemantic::Normal, 3 }, { pumex::VertexSemantic::TexCoord, 3 }, { pumex::VertexSemantic::BoneWeight, 4 }, { pumex::VertexSemantic::BoneIndex, 4 } };
    skeletalAssetBuffer = std::make_shared<pumex::AssetBuffer>();
    skeletalAssetBuffer->registerVertexSemantic(1, vertexSemantic);

    textureRegistryArray = std::make_shared<pumex::TextureRegistryArray>();
    textureRegistryArray->setTargetTexture(0, std::make_shared<pumex::Texture>(gli::texture(gli::target::TARGET_2D_ARRAY, gli::format::FORMAT_RGBA_DXT1_UNORM_BLOCK8, gli::texture::extent_type(2048, 2048, 1), 24, 1, 12), pumex::TextureTraits()));
    std::vector<pumex::TextureSemantic> textureSemantic = { { pumex::TextureSemantic::Diffuse, 0 } };
    materialSet = std::make_shared<pumex::MaterialSet<MaterialData>>(viewerSh, textureRegistryArray, textureSemantic);

    std::vector<std::pair<std::string,bool>> skeletalNames
    {
      { "wmale1", true},
      { "wmale2", true},
      { "wmale3", true},
      { "wmale1_cloth1", false},
      { "wmale1_cloth2", false },
      { "wmale1_cloth3", false },
      { "wmale2_cloth1", false },
      { "wmale2_cloth2", false },
      { "wmale2_cloth3", false },
      { "wmale3_cloth1", false },
      { "wmale3_cloth2", false },
      { "wmale3_cloth3", false }
    };
    std::vector<std::string> skeletalModels
    {
      "wmale1_lod0.dae", "wmale1_lod1.dae", "wmale1_lod2.dae",
      "wmale2_lod0.dae", "wmale2_lod1.dae", "wmale2_lod2.dae",
      "wmale3_lod0.dae", "wmale3_lod1.dae", "wmale3_lod2.dae",
      "wmale1_cloth1.dae", "", "", // well, I don't have LODded cloths :(
      "wmale1_cloth2.dae", "", "",
      "wmale1_cloth3.dae", "", "",
      "wmale2_cloth1.dae", "", "",
      "wmale2_cloth2.dae", "", "",
      "wmale2_cloth3.dae", "", "",
      "wmale3_cloth1.dae", "", "",
      "wmale3_cloth2.dae", "", "",
      "wmale3_cloth3.dae", "", ""
    };
    std::vector<pumex::AssetLodDefinition> lodRanges
    {
      pumex::AssetLodDefinition(0.0f, 8.0f), pumex::AssetLodDefinition(8.0f, 16.0f), pumex::AssetLodDefinition(16.0f, 100.0f),
      pumex::AssetLodDefinition(0.0f, 8.0f), pumex::AssetLodDefinition(8.0f, 16.0f), pumex::AssetLodDefinition(16.0f, 100.0f),
      pumex::AssetLodDefinition(0.0f, 8.0f), pumex::AssetLodDefinition(8.0f, 16.0f), pumex::AssetLodDefinition(16.0f, 100.0f),
      pumex::AssetLodDefinition(0.0f, 100.0f), pumex::AssetLodDefinition(0.0f, 0.0f), pumex::AssetLodDefinition(0.0f, 0.0f),
      pumex::AssetLodDefinition(0.0f, 100.0f), pumex::AssetLodDefinition(0.0f, 0.0f), pumex::AssetLodDefinition(0.0f, 0.0f),
      pumex::AssetLodDefinition(0.0f, 100.0f), pumex::AssetLodDefinition(0.0f, 0.0f), pumex::AssetLodDefinition(0.0f, 0.0f),
      pumex::AssetLodDefinition(0.0f, 100.0f), pumex::AssetLodDefinition(0.0f, 0.0f), pumex::AssetLodDefinition(0.0f, 0.0f),
      pumex::AssetLodDefinition(0.0f, 100.0f), pumex::AssetLodDefinition(0.0f, 0.0f), pumex::AssetLodDefinition(0.0f, 0.0f),
      pumex::AssetLodDefinition(0.0f, 100.0f), pumex::AssetLodDefinition(0.0f, 0.0f), pumex::AssetLodDefinition(0.0f, 0.0f),
      pumex::AssetLodDefinition(0.0f, 100.0f), pumex::AssetLodDefinition(0.0f, 0.0f), pumex::AssetLodDefinition(0.0f, 0.0f),
      pumex::AssetLodDefinition(0.0f, 100.0f), pumex::AssetLodDefinition(0.0f, 0.0f), pumex::AssetLodDefinition(0.0f, 0.0f),
      pumex::AssetLodDefinition(0.0f, 100.0f), pumex::AssetLodDefinition(0.0f, 0.0f), pumex::AssetLodDefinition(0.0f, 0.0f)
   };
    std::multimap<std::string,std::vector<std::vector<std::string>>> materialVariants = 
    {
      { "wmale1", { { "body_mat", "young_lightskinned_male_diffuse_1.dds" } } },
      { "wmale1", { { "body_mat", "young_lightskinned_male_diffuse.dds" } } },
      { "wmale2", { { "body_mat", "young_lightskinned_male_diffuse3_1.dds" } } },
      { "wmale2", { { "body_mat", "dragon_female_white.dds" } } },
      { "wmale3", { { "body_mat", "middleage_lightskinned_male_diffuse_1.dds"} } },
      { "wmale3", { { "body_mat", "ork_texture.dds" } } }
    };
    std::multimap<std::string, std::vector<std::string>> clothVariants =
    {
      { "wmale1", { } },
      { "wmale1", { "wmale1_cloth1" } },
      { "wmale1", { "wmale1_cloth2" } },
      { "wmale1", { "wmale1_cloth3" } },
      { "wmale2", { } },
      { "wmale2", { "wmale2_cloth1" } },
      { "wmale2", { "wmale2_cloth2" } },
      { "wmale2", { "wmale2_cloth3" } },
      { "wmale3", { } },
      { "wmale3", { "wmale3_cloth1" } },
      { "wmale3", { "wmale3_cloth2" } },
      { "wmale3", { "wmale3_cloth3" } }
    };

    std::vector<uint32_t> mainObjectTypeID;
    std::vector<uint32_t> accessoryObjectTypeID;
    skeletons.push_back(pumex::Skeleton()); // empty skeleton for null type
    for (uint32_t i = 0; i < skeletalNames.size(); ++i)
    {
      uint32_t typeID = 0;
      for (uint32_t j = 0; j<3; ++j)
      {
        if (skeletalModels[3 * i + j].empty())
          continue;
        std::string fullAssetFileName = viewerSh->getFullFilePath(skeletalModels[3 * i + j]);
        if (fullAssetFileName.empty())
        {
          LOG_WARNING << "Cannot find asset : " << skeletalModels[3 * i + j] << std::endl;
          continue;
        }
        std::shared_ptr<pumex::Asset> asset(loader.load(fullAssetFileName,false,vertexSemantic));
        if (asset.get() == nullptr)
        {
          LOG_WARNING << "Cannot load asset : " << fullAssetFileName << std::endl;
          continue;
        }
        if( typeID == 0 )
        {
          skeletons.push_back(asset->skeleton);
          pumex::BoundingBox bbox = pumex::calculateBoundingBox(asset->skeleton, animations[0], true);
          typeID = skeletalAssetBuffer->registerType(skeletalNames[i].first, pumex::AssetTypeDefinition(bbox));
          if(skeletalNames[i].second)
            mainObjectTypeID.push_back(typeID);
          else
            accessoryObjectTypeID.push_back(typeID);
        }
        materialSet->registerMaterials(typeID, asset);
        skeletalAssetBuffer->registerObjectLOD(typeID, asset, lodRanges[3 * i + j]);
      }
      // register texture variants
      for (auto it = materialVariants.begin(), eit = materialVariants.end(); it != eit; ++it)
      {
        if (it->first == skeletalNames[i].first)
        {
          uint32_t variantCount = materialSet->getMaterialVariantCount(typeID);
          std::vector<pumex::Material> materials = materialSet->getMaterials(typeID);
          for (auto iit = it->second.begin(); iit != it->second.end(); ++iit)
          {
            for ( auto& mat : materials )
            {
              if (mat.name == (*iit)[0])
                mat.textures[pumex::TextureSemantic::Diffuse] = (*iit)[1];
            }
          }
          materialSet->setMaterialVariant(typeID, variantCount, materials);
        }
      }
    }
    materialSet->refreshMaterialStructures();
    std::vector<uint32_t> materialVariantCount(skeletalNames.size()+1);
    for (uint32_t i= 0; i<materialVariantCount.size(); ++i)
      materialVariantCount[i] = materialSet->getMaterialVariantCount(i);

    cameraUbo    = std::make_shared<pumex::UniformBuffer<pumex::Camera>>();
    positionSbo  = std::make_shared<pumex::StorageBuffer<PositionData>>();
    instanceSbo  = std::make_shared<pumex::StorageBuffer<InstanceData>>();
    resultsSbo   = std::make_shared<pumex::StorageBuffer<pumex::DrawIndexedIndirectCommand>>(VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    resultsSbo2  = std::make_shared<pumex::StorageBuffer<pumex::DrawIndexedIndirectCommand>>((VkBufferUsageFlagBits)(VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT));
    offValuesSbo = std::make_shared<pumex::StorageBuffer<uint32_t>>();

    pipelineCache = std::make_shared<pumex::PipelineCache>();

    std::vector<pumex::DescriptorSetLayoutBinding> simpleRenderLayoutBindings =
    {
      { 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 2, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 3, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 4, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 5, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT },
      { 6, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT }

    };
    simpleRenderDescriptorSetLayout = std::make_shared<pumex::DescriptorSetLayout>(simpleRenderLayoutBindings);
    simpleRenderDescriptorPool = std::make_shared<pumex::DescriptorPool>(1, simpleRenderLayoutBindings);
    // building pipeline layout
    simpleRenderPipelineLayout = std::make_shared<pumex::PipelineLayout>();
    simpleRenderPipelineLayout->descriptorSetLayouts.push_back(simpleRenderDescriptorSetLayout);
    simpleRenderPipeline = std::make_shared<pumex::GraphicsPipeline>(pipelineCache, simpleRenderPipelineLayout, defaultRenderPass, 0);
    simpleRenderPipeline->shaderStages =
    {
      { VK_SHADER_STAGE_VERTEX_BIT,   std::make_shared<pumex::ShaderModule>(viewerSh->getFullFilePath("crowd_simple_animation.vert.spv")), "main" },
      { VK_SHADER_STAGE_FRAGMENT_BIT, std::make_shared<pumex::ShaderModule>(viewerSh->getFullFilePath("crowd_simple_animation.frag.spv")), "main" }
    };
    simpleRenderPipeline->vertexInput =
    {
      { 0, VK_VERTEX_INPUT_RATE_VERTEX, vertexSemantic }
    };
    simpleRenderPipeline->blendAttachments =
    {
      { VK_FALSE, 0xF }
    };
    simpleRenderPipeline->dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

    simpleRenderDescriptorSet = std::make_shared<pumex::DescriptorSet>(simpleRenderDescriptorSetLayout, simpleRenderDescriptorPool);
    simpleRenderDescriptorSet->setSource(0, cameraUbo);
    simpleRenderDescriptorSet->setSource(1, positionSbo);
    simpleRenderDescriptorSet->setSource(2, instanceSbo);
    simpleRenderDescriptorSet->setSource(3, materialSet->getTypeBufferDescriptorSetSource());
    simpleRenderDescriptorSet->setSource(4, materialSet->getMaterialVariantBufferDescriptorSetSource());
    simpleRenderDescriptorSet->setSource(5, materialSet->getMaterialDefinitionBufferDescriptorSetSource());
    simpleRenderDescriptorSet->setSource(6, textureRegistryArray->getTargetTexture(0));

    std::vector<pumex::DescriptorSetLayoutBinding> instancedRenderLayoutBindings =
    {
      { 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 2, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
      { 3, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 4, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 5, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      { 6, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT },
      { 7, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT }
    };
    instancedRenderDescriptorSetLayout = std::make_shared<pumex::DescriptorSetLayout>(instancedRenderLayoutBindings);
    instancedRenderDescriptorPool = std::make_shared<pumex::DescriptorPool>(1, instancedRenderLayoutBindings);
    // building pipeline layout
    instancedRenderPipelineLayout = std::make_shared<pumex::PipelineLayout>();
    instancedRenderPipelineLayout->descriptorSetLayouts.push_back(instancedRenderDescriptorSetLayout);
    instancedRenderPipeline = std::make_shared<pumex::GraphicsPipeline>(pipelineCache, instancedRenderPipelineLayout, defaultRenderPass, 0);
    instancedRenderPipeline->shaderStages =
    {
      { VK_SHADER_STAGE_VERTEX_BIT,   std::make_shared<pumex::ShaderModule>(viewerSh->getFullFilePath("crowd_instanced_animation.vert.spv")), "main" },
      { VK_SHADER_STAGE_FRAGMENT_BIT, std::make_shared<pumex::ShaderModule>(viewerSh->getFullFilePath("crowd_instanced_animation.frag.spv")), "main" }
    };
    instancedRenderPipeline->vertexInput =
    {
      { 0, VK_VERTEX_INPUT_RATE_VERTEX, vertexSemantic }
    };
    instancedRenderPipeline->blendAttachments =
    {
      { VK_FALSE, 0xF }
    };
    instancedRenderPipeline->dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

    instancedRenderDescriptorSet = std::make_shared<pumex::DescriptorSet>(instancedRenderDescriptorSetLayout, instancedRenderDescriptorPool);
    instancedRenderDescriptorSet->setSource(0, cameraUbo);
    instancedRenderDescriptorSet->setSource(1, positionSbo);
    instancedRenderDescriptorSet->setSource(2, instanceSbo);
    instancedRenderDescriptorSet->setSource(3, offValuesSbo);
    instancedRenderDescriptorSet->setSource(4, materialSet->getTypeBufferDescriptorSetSource());
    instancedRenderDescriptorSet->setSource(5, materialSet->getMaterialVariantBufferDescriptorSetSource());
    instancedRenderDescriptorSet->setSource(6, materialSet->getMaterialDefinitionBufferDescriptorSetSource());
    instancedRenderDescriptorSet->setSource(7, textureRegistryArray->getTargetTexture(0));


    std::vector<pumex::DescriptorSetLayoutBinding> filterLayoutBindings =
    {
      { 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT },
      { 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT },
      { 2, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT },
      { 3, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT },
      { 4, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT },
      { 5, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT },
      { 6, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT }
    };
    filterDescriptorSetLayout = std::make_shared<pumex::DescriptorSetLayout>(filterLayoutBindings);
    filterDescriptorPool = std::make_shared<pumex::DescriptorPool>(1, filterLayoutBindings);
    // building pipeline layout
    filterPipelineLayout = std::make_shared<pumex::PipelineLayout>();
    filterPipelineLayout->descriptorSetLayouts.push_back(filterDescriptorSetLayout);
    filterPipeline = std::make_shared<pumex::ComputePipeline>(pipelineCache, filterPipelineLayout);
    filterPipeline->shaderStage = { VK_SHADER_STAGE_COMPUTE_BIT, std::make_shared<pumex::ShaderModule>(viewerSh->getFullFilePath("crowd_filter_instances.comp.spv")), "main" };

    filterDescriptorSet = std::make_shared<pumex::DescriptorSet>(filterDescriptorSetLayout, filterDescriptorPool);
    filterDescriptorSet->setSource(0, skeletalAssetBuffer->getTypeBufferDescriptorSetSource(1));
    filterDescriptorSet->setSource(1, skeletalAssetBuffer->getLODBufferDescriptorSetSource(1));
    filterDescriptorSet->setSource(2, cameraUbo);
    filterDescriptorSet->setSource(3, positionSbo);
    filterDescriptorSet->setSource(4, instanceSbo);
    filterDescriptorSet->setSource(5, resultsSbo);
    filterDescriptorSet->setSource(6, offValuesSbo);

    timeStampQueryPool = std::make_shared<pumex::QueryPool>(VK_QUERY_TYPE_TIMESTAMP,12);

    // initializing data
    float fullArea = (maxArea.x - minArea.x) * (maxArea.y - minArea.y);
    unsigned int objectQuantity = (unsigned int)floor(objectDensity * fullArea / 1000000.0f);

    std::uniform_real_distribution<float>   randomX(minArea.x, maxArea.x);
    std::uniform_real_distribution<float>   randomY(minArea.y, maxArea.y);
    std::uniform_int_distribution<uint32_t> randomType(0, mainObjectTypeID.size() - 1);
    std::uniform_real_distribution<float>   randomAnimationOffset(0.0f, 5.0f);

    // each object type has its own number of material variants
    std::vector<std::uniform_int_distribution<uint32_t>> randomMaterialVariant;
    for (uint32_t i = 0; i < materialVariantCount.size(); ++i)
      randomMaterialVariant.push_back(std::uniform_int_distribution<uint32_t>(0, materialVariantCount[i] - 1));

    for (unsigned int i = 0; i<objectQuantity; ++i)
    {
      glm::vec2 pos(randomX(randomEngine), randomY(randomEngine));
      float rot                      = randomRotation(randomEngine);
      uint32_t objectType            = mainObjectTypeID[randomType(randomEngine)];
      uint32_t objectMaterialVariant = randomMaterialVariant[objectType](randomEngine);
      uint32_t anim                  = randomAnimation(randomEngine);
      positionData.push_back(PositionData(glm::translate(glm::mat4(), glm::vec3(pos.x, pos.y, 0.0f)) * glm::rotate(glm::mat4(), rot, glm::vec3(0.0f, 0.0f, 1.0f))));
      instanceData.push_back(InstanceData(i, objectType, objectMaterialVariant, 1));
      instanceDataCPU.push_back(InstanceDataCPU(anim, pos, rot, animationSpeed[anim], randomTime2NextTurn(randomEngine), randomAnimationOffset(randomEngine)));
      auto clothPair = clothVariants.equal_range(skeletalAssetBuffer->getTypeName(objectType));
      auto clothCount = std::distance(clothPair.first, clothPair.second);
      if (clothCount > 0)
      {
        uint32_t clothIndex = i % clothCount;
        std::advance(clothPair.first, clothIndex);
        for( const auto& c : clothPair.first->second )
        {
          uint32_t clothType = skeletalAssetBuffer->getTypeID(c);
          instanceData.push_back(InstanceData(i, clothType, 0, 0));
          instanceDataCPU.push_back(InstanceDataCPU(anim, pos, rot, animationSpeed[anim], 0.0, 0.0));
        }
      }
    }
    positionSbo->set(positionData);
    instanceSbo->set(instanceData);

    std::vector<pumex::DrawIndexedIndirectCommand> results;
    skeletalAssetBuffer->prepareDrawIndexedIndirectCommandBuffer(1,results, resultsGeomToType);
    resultsSbo->set(results);
    resultsSbo2->set(results);
    offValuesSbo->set(std::vector<uint32_t>(1)); // FIXME
  }

  void update(float timeSinceStart, float timeSinceLastFrame)
  {
    // parallelized version of update
    tbb::parallel_for
    (
      tbb::blocked_range<size_t>(0, instanceData.size()), 
      [=](const tbb::blocked_range<size_t>& r) 
      {
        for (size_t i = r.begin(); i != r.end(); ++i)
          updateInstance(i, timeSinceStart, timeSinceLastFrame);
      }
    );
    // serial version of update
    //for (uint32_t i = 0; i < instanceData.size(); ++i)
    //{
    //  updateInstance(i, timeSinceStart, timeSinceLastFrame);
    //}

    positionSbo->set(positionData);
    instanceSbo->set(instanceData);

    if (renderMethod == 1)
    {
      std::vector<uint32_t> typeCount(skeletalAssetBuffer->getNumTypesID());
      std::fill(typeCount.begin(), typeCount.end(), 0);
      // compute how many instances of each type there is
      for (uint32_t i = 0; i<instanceData.size(); ++i)
        typeCount[instanceData[i].typeID]++;

      std::vector<uint32_t> offsets;
      for (uint32_t i = 0; i<resultsGeomToType.size(); ++i)
        offsets.push_back(typeCount[resultsGeomToType[i]]);

      std::vector<pumex::DrawIndexedIndirectCommand> results = resultsSbo->get();
      uint32_t offsetSum = 0;
      for (uint32_t i = 0; i<offsets.size(); ++i)
      {
        uint32_t tmp = offsetSum;
        offsetSum += offsets[i];
        offsets[i] = tmp;
        results[i].firstInstance = tmp;
      }
      resultsSbo->set(results);
      offValuesSbo->set(std::vector<uint32_t>(offsetSum));
    }

  }


  void updateInstance(uint32_t i, float timeSinceStart, float timeSinceLastFrame)
  {
    // skip animation calculations for instances that are not needed
    if (instanceData[i].mainInstance == 0)
      return;
    // change direction if bot is leaving designated area
    bool isOutside[] = 
    {
      instanceDataCPU[i].position.x < minArea.x ,
      instanceDataCPU[i].position.x > maxArea.x ,
      instanceDataCPU[i].position.y < minArea.y ,
      instanceDataCPU[i].position.y > maxArea.y
    };
    if (isOutside[0] || isOutside[1] || isOutside[2] || isOutside[3] )
    {
      instanceDataCPU[i].position.x = std::max(instanceDataCPU[i].position.x, minArea.x);
      instanceDataCPU[i].position.x = std::min(instanceDataCPU[i].position.x, maxArea.x);
      instanceDataCPU[i].position.y = std::max(instanceDataCPU[i].position.y, minArea.y);
      instanceDataCPU[i].position.y = std::min(instanceDataCPU[i].position.y, maxArea.y);
      glm::mat4 rotationMatrix = glm::rotate(glm::mat4(), glm::radians(instanceDataCPU[i].rotation), glm::vec3(0.0f, 0.0f, 1.0f));
      glm::vec4 direction = rotationMatrix * glm::rotate(glm::mat4(), glm::radians(-90.0f), glm::vec3(0.0f, 0.0f, 1.0f))  * glm::vec4(1, 0, 0, 1);// MakeHuman models are rotated looking at Y=-1, we have to rotate it
      if (isOutside[0] || isOutside[1])
        direction.x *= -1.0f;
      if (isOutside[2] || isOutside[3])
        direction.y *= -1.0f;
      direction = glm::rotate(glm::mat4(), glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f))  * direction;
      instanceDataCPU[i].rotation = glm::degrees(atan2f(direction.y, direction.x));
      instanceDataCPU[i].time2NextTurn = randomTime2NextTurn(randomEngine);
    }
    // change rotation, animation and speed if bot requires it
    instanceDataCPU[i].time2NextTurn -= timeSinceLastFrame;
    if (instanceDataCPU[i].time2NextTurn < 0.0f)
    {
      instanceDataCPU[i].rotation      = randomRotation(randomEngine);
      instanceDataCPU[i].time2NextTurn = randomTime2NextTurn(randomEngine);
      instanceDataCPU[i].animation     = randomAnimation(randomEngine);
      instanceDataCPU[i].speed         = animationSpeed[instanceDataCPU[i].animation];
    }
    // calculate new position
    glm::mat4 rotationMatrix = glm::rotate(glm::mat4(), glm::radians(instanceDataCPU[i].rotation), glm::vec3(0.0f, 0.0f, 1.0f));
    glm::vec4 direction = rotationMatrix * glm::rotate(glm::mat4(), glm::radians(-90.0f), glm::vec3(0.0f, 0.0f, 1.0f))  * glm::vec4(1, 0, 0, 1);// MakeHuman models are rotated looking at Y=-1, we have to rotate it
    glm::vec2 dir2(direction.x, direction.y);
    instanceDataCPU[i].position += dir2 * instanceDataCPU[i].speed * timeSinceLastFrame;
    positionData[instanceData[i].positionIndex].position = glm::translate(glm::mat4(), glm::vec3(instanceDataCPU[i].position.x, instanceDataCPU[i].position.y, 0.0f)) * rotationMatrix;

    // calculate bone matrices for the bots
    pumex::Animation& anim = animations[instanceDataCPU[i].animation];
    pumex::Skeleton&  skel = skeletons[instanceData[i].typeID];

    uint32_t numAnimChannels = anim.channels.size();
    uint32_t numSkelBones = skel.bones.size();
    SkelAnimKey saKey(instanceData[i].typeID, instanceDataCPU[i].animation);

    auto bmit = skelAnimBoneMapping.find(saKey);
    if (bmit == skelAnimBoneMapping.end())
    {
      std::vector<uint32_t> boneChannelMapping(numSkelBones);
      for (uint32_t boneIndex = 0; boneIndex < numSkelBones; ++boneIndex)
      {
        auto it = anim.invChannelNames.find(skel.boneNames[boneIndex]);
        boneChannelMapping[boneIndex] = (it != anim.invChannelNames.end()) ? it->second : UINT32_MAX;
      }
      bmit = skelAnimBoneMapping.insert({ saKey, boneChannelMapping }).first;
    }

    std::vector<glm::mat4> localTransforms(MAX_BONES);
    std::vector<glm::mat4> globalTransforms(MAX_BONES);

    const auto& boneChannelMapping = bmit->second;
    anim.calculateLocalTransforms(timeSinceStart + instanceDataCPU[i].animationOffset, localTransforms.data(), numAnimChannels);
    uint32_t bcVal = boneChannelMapping[0];
    glm::mat4 localCurrentTransform = (bcVal == UINT32_MAX) ? skel.bones[0].localTransformation : localTransforms[bcVal];
    globalTransforms[0] = skel.invGlobalTransform * localCurrentTransform;
    for (uint32_t boneIndex = 1; boneIndex < numSkelBones; ++boneIndex)
    {
      bcVal = boneChannelMapping[boneIndex];
      localCurrentTransform = (bcVal == UINT32_MAX) ? skel.bones[boneIndex].localTransformation : localTransforms[bcVal];
      globalTransforms[boneIndex] = globalTransforms[skel.bones[boneIndex].parentIndex] * localCurrentTransform;
    }
    PositionData& posData = positionData[instanceData[i].positionIndex];
    for (uint32_t boneIndex = 0; boneIndex < numSkelBones; ++boneIndex)
      posData.bones[boneIndex] = globalTransforms[boneIndex] * skel.bones[boneIndex].offsetMatrix;
  }
};

class CrowdRenderThread : public pumex::SurfaceThread
{
public:
  CrowdRenderThread( std::shared_ptr<ApplicationData> applicationData )
    : pumex::SurfaceThread(), appData(applicationData)
  {
  }

  void setup(std::shared_ptr<pumex::Surface> s) override
  {
    SurfaceThread::setup(s);

    std::shared_ptr<pumex::Surface> surfaceSh = surface.lock();
    std::shared_ptr<pumex::Device>  deviceSh  = surfaceSh->device.lock();
    VkDevice                        vkDevice  = deviceSh->device;

    myCmdBuffer = std::make_shared<pumex::CommandBuffer>(VK_COMMAND_BUFFER_LEVEL_PRIMARY, surfaceSh->commandPool);
    myCmdBuffer->validate(deviceSh);

    appData->pipelineCache->validate(deviceSh);

    appData->skeletalAssetBuffer->validate(deviceSh, true, surfaceSh->commandPool, surfaceSh->presentationQueue);
    appData->materialSet->validate(deviceSh, surfaceSh->commandPool, surfaceSh->presentationQueue);
    appData->simpleRenderDescriptorSetLayout->validate(deviceSh);
    appData->simpleRenderDescriptorPool->validate(deviceSh);
    appData->simpleRenderPipelineLayout->validate(deviceSh);
    appData->simpleRenderPipeline->validate(deviceSh);

    appData->instancedRenderDescriptorSetLayout->validate(deviceSh);
    appData->instancedRenderDescriptorPool->validate(deviceSh);
    appData->instancedRenderPipelineLayout->validate(deviceSh);
    appData->instancedRenderPipeline->validate(deviceSh);

    appData->filterDescriptorSetLayout->validate(deviceSh);
    appData->filterDescriptorPool->validate(deviceSh);
    appData->filterPipelineLayout->validate(deviceSh);
    appData->filterPipeline->validate(deviceSh);

    appData->timeStampQueryPool->validate(deviceSh);

    // preparing descriptor sets
    appData->cameraUbo->validate(deviceSh);
    appData->positionSbo->validate(deviceSh);
    appData->instanceSbo->validate(deviceSh);
    appData->resultsSbo->validate(deviceSh);
    appData->resultsSbo2->validate(deviceSh);
    appData->offValuesSbo->validate(deviceSh);

    cameraPosition              = glm::vec3(0.0f, 0.0f, 0.0f);
    cameraGeographicCoordinates = glm::vec2(0.0f, 0.0f);
    cameraDistance              = 1.0f;
    leftMouseKeyPressed         = false;
    rightMouseKeyPressed        = false;
    xKeyPressed                 = false;
  }

  void cleanup() override
  {
    SurfaceThread::cleanup();
  }
  ~CrowdRenderThread()
  {
    cleanup();
  }
  void draw() override
  {
    std::shared_ptr<pumex::Surface> surfaceSh = surface.lock();
    std::shared_ptr<pumex::Viewer>  viewerSh  = surface.lock()->viewer.lock();
    std::shared_ptr<pumex::Device>  deviceSh  = surfaceSh->device.lock();
    std::shared_ptr<pumex::Window>  windowSh  = surfaceSh->window.lock();
    VkDevice                        vkDevice  = deviceSh->device;

    double timeSinceStartInSeconds = std::chrono::duration<double, std::ratio<1,1>>(timeSinceStart).count();
    double lastFrameInSeconds      = std::chrono::duration<double, std::ratio<1,1>>(timeSinceLastFrame).count();

    // camera update
    std::vector<pumex::MouseEvent> mouseEvents = windowSh->getMouseEvents();
    glm::vec2 mouseMove = lastMousePos;
    for (const auto& m : mouseEvents)
    {
      switch (m.type)
      {
      case pumex::MouseEvent::KEY_PRESSED:
        if (m.button == pumex::MouseEvent::LEFT)
          leftMouseKeyPressed = true;
        if (m.button == pumex::MouseEvent::RIGHT)
          rightMouseKeyPressed = true;
        mouseMove.x = m.x;
        mouseMove.y = m.y;
        lastMousePos = mouseMove;
        break;
      case pumex::MouseEvent::KEY_RELEASED:
        if (m.button == pumex::MouseEvent::LEFT)
          leftMouseKeyPressed = false;
        if (m.button == pumex::MouseEvent::RIGHT)
          rightMouseKeyPressed = false;
        break;
      case pumex::MouseEvent::MOVE:
        if (leftMouseKeyPressed || rightMouseKeyPressed)
        {
          mouseMove.x = m.x;
          mouseMove.y = m.y;
        }
        break;
      }
    }
    if (leftMouseKeyPressed)
    {
      cameraGeographicCoordinates.x -= 100.0f*(mouseMove.x - lastMousePos.x);
      cameraGeographicCoordinates.y += 100.0f*(mouseMove.y - lastMousePos.y);
      while (cameraGeographicCoordinates.x < -180.0f)
        cameraGeographicCoordinates.x += 360.0f;
      while (cameraGeographicCoordinates.x>180.0f)
        cameraGeographicCoordinates.x -= 360.0f;
      cameraGeographicCoordinates.y = glm::clamp(cameraGeographicCoordinates.y, -90.0f, 90.0f);
      lastMousePos = mouseMove;
    }
    if (rightMouseKeyPressed)
    {
      cameraDistance += 10.0f*(lastMousePos.y - mouseMove.y);
      if (cameraDistance<0.1f)
        cameraDistance = 0.1f;
      lastMousePos = mouseMove;
    }

    glm::vec3 forward = glm::vec3(cos(cameraGeographicCoordinates.x * 3.1415f / 180.0f), sin(cameraGeographicCoordinates.x * 3.1415f / 180.0f), 0) * 0.2f;
    glm::vec3 right = glm::vec3(cos((cameraGeographicCoordinates.x + 90.0f) * 3.1415f / 180.0f), sin((cameraGeographicCoordinates.x + 90.0f) * 3.1415f / 180.0f), 0) * 0.2f;
    if (windowSh->isKeyPressed('W'))
      cameraPosition -= forward;
    if (windowSh->isKeyPressed('S'))
      cameraPosition += forward;
    if (windowSh->isKeyPressed('A'))
      cameraPosition -= right;
    if (windowSh->isKeyPressed('D'))
      cameraPosition += right;
    if (windowSh->isKeyPressed('X'))
    {
      if (!xKeyPressed)
      {
        appData->renderMethod = ( appData->renderMethod + 1 ) % 2;
        switch (appData->renderMethod)
        {
        case 0: LOG_INFO << "Rendering using simple method ( each entity uses its own vkCmdDrawIndexed )" << std::endl; break;
        case 1: LOG_INFO << "Rendering using instanced method ( all entities use only a single vkCmdDrawIndexedIndirect )" << std::endl; break;
        }
        xKeyPressed = true;
      }
    }
    else
      xKeyPressed = false;

    glm::vec3 eye
      (
      cameraDistance * cos(cameraGeographicCoordinates.x * 3.1415f / 180.0f) * cos(cameraGeographicCoordinates.y * 3.1415f / 180.0f),
      cameraDistance * sin(cameraGeographicCoordinates.x * 3.1415f / 180.0f) * cos(cameraGeographicCoordinates.y * 3.1415f / 180.0f),
      cameraDistance * sin(cameraGeographicCoordinates.y * 3.1415f / 180.0f)
      );
    glm::mat4 viewMatrix = glm::lookAt(eye + cameraPosition, cameraPosition, glm::vec3(0, 0, 1));

    uint32_t renderWidth  = surfaceSh->swapChainSize.width;
    uint32_t renderHeight = surfaceSh->swapChainSize.height;

    pumex::Camera camera = appData->cameraUbo->get();
    camera.setViewMatrix(viewMatrix);
    camera.setObserverPosition(eye + cameraPosition);
    camera.setProjectionMatrix(glm::perspective(glm::radians(60.0f), (float)renderWidth / (float)renderHeight, 0.1f, 100000.0f));
    appData->cameraUbo->set(camera);

#if defined(CROWD_MEASURE_TIME)
    auto updateStart = pumex::HPClock::now();
#endif
    appData->update(timeSinceStartInSeconds, lastFrameInSeconds);
#if defined(CROWD_MEASURE_TIME)
    auto updateEnd = pumex::HPClock::now();
    double updateDuration = std::chrono::duration<double, std::milli>(updateEnd - updateStart).count();
#endif

    appData->cameraUbo->validate(deviceSh);
    appData->positionSbo->validate(deviceSh);
    appData->instanceSbo->validate(deviceSh);
    appData->resultsSbo->validate(deviceSh);
    appData->offValuesSbo->validate(deviceSh);

    appData->simpleRenderDescriptorSet->validate(deviceSh);
    appData->instancedRenderDescriptorSet->validate(deviceSh);
    appData->filterDescriptorSet->validate(deviceSh);


#if defined(CROWD_MEASURE_TIME)
    auto drawStart = pumex::HPClock::now();
#endif
    myCmdBuffer->cmdBegin(deviceSh);
    appData->timeStampQueryPool->reset(deviceSh, myCmdBuffer, surfaceSh->swapChainImageIndex*4,4);

    pumex::DescriptorSetValue resultsBuffer = appData->resultsSbo->getDescriptorSetValue(vkDevice);
    pumex::DescriptorSetValue resultsBuffer2 = appData->resultsSbo2->getDescriptorSetValue(vkDevice);
    uint32_t drawCount = appData->resultsSbo->get().size();

    if (appData->renderMethod == 1)
    {
#if defined(CROWD_MEASURE_TIME)
      appData->timeStampQueryPool->queryTimeStamp(deviceSh, myCmdBuffer, surfaceSh->swapChainImageIndex * 4 + 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
#endif
      // Add memory barrier to ensure that the indirect commands have been consumed before the compute shader updates them
      pumex::PipelineBarrier beforeBufferBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, surfaceSh->presentationQueueFamilyIndex, surfaceSh->presentationQueueFamilyIndex, resultsBuffer.bufferInfo);
      myCmdBuffer->cmdPipelineBarrier(deviceSh, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, beforeBufferBarrier);

      myCmdBuffer->cmdBindPipeline(deviceSh, appData->filterPipeline);
      myCmdBuffer->cmdBindDescriptorSets(deviceSh, VK_PIPELINE_BIND_POINT_COMPUTE, appData->filterPipelineLayout, 0, appData->filterDescriptorSet);
      myCmdBuffer->cmdDispatch(deviceSh, appData->instanceData.size() / 16 + ((appData->instanceData.size() % 16>0) ? 1 : 0), 1, 1);

      pumex::PipelineBarrier afterBufferBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, surfaceSh->presentationQueueFamilyIndex, surfaceSh->presentationQueueFamilyIndex, resultsBuffer.bufferInfo);
      myCmdBuffer->cmdPipelineBarrier(deviceSh, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, afterBufferBarrier);

      VkBufferCopy copyRegion{};
        copyRegion.srcOffset = resultsBuffer.bufferInfo.offset;
        copyRegion.size      = resultsBuffer.bufferInfo.range;
        copyRegion.dstOffset = resultsBuffer2.bufferInfo.offset;
      myCmdBuffer->cmdCopyBuffer(deviceSh, resultsBuffer.bufferInfo.buffer, resultsBuffer2.bufferInfo.buffer, copyRegion);
      
      pumex::PipelineBarrier afterCopyBufferBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT, surfaceSh->presentationQueueFamilyIndex, surfaceSh->presentationQueueFamilyIndex, resultsBuffer2.bufferInfo);
      myCmdBuffer->cmdPipelineBarrier(deviceSh, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, afterCopyBufferBarrier);

#if defined(CROWD_MEASURE_TIME)
      appData->timeStampQueryPool->queryTimeStamp(deviceSh, myCmdBuffer, surfaceSh->swapChainImageIndex * 4 + 1, VK_PIPELINE_STAGE_TRANSFER_BIT);
#endif
    }

    std::vector<VkClearValue> clearValues = { pumex::makeColorClearValue(glm::vec4(0.3f, 0.3f, 0.3f, 1.0f)), pumex::makeDepthStencilClearValue(1.0f, 0) };
    myCmdBuffer->cmdBeginRenderPass(deviceSh, appData->defaultRenderPass, surfaceSh->getCurrentFrameBuffer(), pumex::makeVkRect2D(0, 0, renderWidth, renderHeight), clearValues);
    myCmdBuffer->cmdSetViewport(deviceSh, 0, { pumex::makeViewport(0, 0, renderWidth, renderHeight, 0.0f, 1.0f) });
    myCmdBuffer->cmdSetScissor(deviceSh, 0, { pumex::makeVkRect2D(0, 0, renderWidth, renderHeight) });

#if defined(CROWD_MEASURE_TIME)
    appData->timeStampQueryPool->queryTimeStamp(deviceSh, myCmdBuffer, surfaceSh->swapChainImageIndex * 4 + 2, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);
#endif
    switch (appData->renderMethod)
    {
    case 0: // simple rendering: no compute culling, no instancing
    {
      myCmdBuffer->cmdBindPipeline(deviceSh, appData->simpleRenderPipeline);
      myCmdBuffer->cmdBindDescriptorSets(deviceSh, VK_PIPELINE_BIND_POINT_GRAPHICS, appData->simpleRenderPipelineLayout, 0, appData->simpleRenderDescriptorSet);
      appData->skeletalAssetBuffer->cmdBindVertexIndexBuffer(deviceSh, myCmdBuffer, 1, 0);
      // Old method of LOD selecting - it works for normal cameras, but shadow cameras should have observerPosition defined by main camera
//      glm::vec4 cameraPos = appData->cameraUbo->get().getViewMatrixInverse() * glm::vec4(0,0,0,1);
      glm::vec4 cameraPos = appData->cameraUbo->get().getObserverPosition();
      for (uint32_t i = 0; i<appData->instanceData.size(); ++i)
      {
        glm::vec4 objectPos = appData->positionData[appData->instanceData[i].positionIndex].position[3];
        float distanceToCamera = glm::length(cameraPos - objectPos);
        appData->skeletalAssetBuffer->cmdDrawObject(deviceSh, myCmdBuffer, 1, appData->instanceData[i].typeID, i, distanceToCamera);
      }
      break;
    }
    case 1: // compute culling and instanced rendering
    {
      myCmdBuffer->cmdBindPipeline(deviceSh, appData->instancedRenderPipeline);
      myCmdBuffer->cmdBindDescriptorSets(deviceSh, VK_PIPELINE_BIND_POINT_GRAPHICS, appData->instancedRenderPipelineLayout, 0, appData->instancedRenderDescriptorSet);
      appData->skeletalAssetBuffer->cmdBindVertexIndexBuffer(deviceSh, myCmdBuffer, 1, 0);
      if (deviceSh->physical.lock()->features.multiDrawIndirect == 1)
        myCmdBuffer->cmdDrawIndexedIndirect(deviceSh, resultsBuffer2.bufferInfo.buffer, resultsBuffer2.bufferInfo.offset, drawCount, sizeof(pumex::DrawIndexedIndirectCommand));
      else
      {
        for (uint32_t i = 0; i < drawCount; ++i)
          myCmdBuffer->cmdDrawIndexedIndirect(deviceSh, resultsBuffer2.bufferInfo.buffer, resultsBuffer2.bufferInfo.offset + i*sizeof(pumex::DrawIndexedIndirectCommand), 1, sizeof(pumex::DrawIndexedIndirectCommand));
      }
      break;
    }
    }
#if defined(CROWD_MEASURE_TIME)
    appData->timeStampQueryPool->queryTimeStamp(deviceSh, myCmdBuffer, surfaceSh->swapChainImageIndex * 4 + 3, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
#endif

    myCmdBuffer->cmdEndRenderPass(deviceSh);
    myCmdBuffer->cmdEnd(deviceSh);
    myCmdBuffer->queueSubmit(deviceSh, surfaceSh->presentationQueue, { surfaceSh->imageAvailableSemaphore }, { VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT }, { surfaceSh->renderCompleteSemaphore }, VK_NULL_HANDLE);

#if defined(CROWD_MEASURE_TIME)
    auto drawEnd = pumex::HPClock::now();
    double drawDuration = std::chrono::duration<double, std::milli>(drawEnd - drawStart).count();
    LOG_ERROR << "Frame time                : " << 1000.0 * lastFrameInSeconds << " ms ( FPS = " << 1.0 / lastFrameInSeconds << " )" << std::endl;
    LOG_ERROR << "Update skeletons          : " << updateDuration << " ms" << std::endl;
    LOG_ERROR << "Fill command line buffers : " << drawDuration << " ms" << std::endl;

    // skip GPU statistics when user switches render methods
    if (!xKeyPressed)
    {
      float timeStampPeriod = deviceSh->physical.lock()->properties.limits.timestampPeriod / 1000000.0f;
      std::vector<uint64_t> queryResults;
      // We use swapChainImageIndex to get the time measurments from previous frame - timeStampQueryPool works like circular buffer
      if(appData->renderMethod==1)
      {
        queryResults = appData->timeStampQueryPool->getResults(deviceSh, ((surfaceSh->swapChainImageIndex + 2) % 3) * 4, 4, 0);
        LOG_ERROR << "GPU LOD compute shader    : " << (queryResults[1] - queryResults[0]) * timeStampPeriod  << " ms" << std::endl;
        LOG_ERROR << "GPU draw shader           : " << (queryResults[3] - queryResults[2]) * timeStampPeriod << " ms" << std::endl;
      }
      else
      {
        queryResults = appData->timeStampQueryPool->getResults(deviceSh, ((surfaceSh->swapChainImageIndex + 2) % 3) * 4 + 2, 2, 0);
        LOG_ERROR << "GPU draw duration " << (queryResults[1] - queryResults[0]) * timeStampPeriod << " ms" << std::endl;
      }
    }
    LOG_ERROR << std::endl;

#endif
  }

  std::shared_ptr<ApplicationData>      appData;
  std::shared_ptr<pumex::CommandBuffer> myCmdBuffer;

  glm::vec3 cameraPosition;
  glm::vec2 cameraGeographicCoordinates;
  float     cameraDistance;
  glm::vec2 lastMousePos;
  bool      leftMouseKeyPressed;
  bool      rightMouseKeyPressed;
  bool      xKeyPressed;


};

int main(void)
{
  SET_LOG_INFO;
  LOG_INFO << "Crowd rendering" << std::endl;
	
  const std::vector<std::string> requestDebugLayers = { { "VK_LAYER_LUNARG_standard_validation" } };
  pumex::ViewerTraits viewerTraits{ "Crowd rendering application", true, requestDebugLayers };
  viewerTraits.debugReportFlags = VK_DEBUG_REPORT_ERROR_BIT_EXT;// | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT | VK_DEBUG_REPORT_INFORMATION_BIT_EXT | VK_DEBUG_REPORT_DEBUG_BIT_EXT;

  std::shared_ptr<pumex::Viewer> viewer = std::make_shared<pumex::Viewer>(viewerTraits);
  try
  {

    std::vector<pumex::QueueTraits> requestQueues = { { VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0, { 0.75f } } };
    std::vector<const char*> requestDeviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    std::shared_ptr<pumex::Device> device = viewer->addDevice(0, requestQueues, requestDeviceExtensions);
    CHECK_LOG_THROW(!device->isValid(), "Cannot create logical device with requested parameters" );

    pumex::WindowTraits windowTraits{0, 100, 100, 640, 480, false, "Crowd rendering"};
    std::shared_ptr<pumex::Window> window = pumex::Window::createWindow(windowTraits);

    pumex::SurfaceTraits surfaceTraits{ 3, VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, 1, VK_FORMAT_D24_UNORM_S8_UINT, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR };
    surfaceTraits.definePresentationQueue(pumex::QueueTraits{ VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0, { 0.75f } });

    std::vector<pumex::AttachmentDefinition> renderPassAttachments = 
    {
      { VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0 },
      { VK_FORMAT_D24_UNORM_S8_UINT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0 }
    };
    std::vector<pumex::SubpassDefinition> renderPassSubpasses = 
    {
      {
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        {},
        { { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } },
        {},
        { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
        {},
        0
      }
    };
    std::vector<pumex::SubpassDependencyDefinition> renderPassDependencies; 
//    { { 0, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, 0 } };

    std::shared_ptr<pumex::RenderPass> renderPass = std::make_shared<pumex::RenderPass>(renderPassAttachments, renderPassSubpasses, renderPassDependencies);
    surfaceTraits.setDefaultRenderPass(renderPass);

    std::shared_ptr<ApplicationData> applicationData = std::make_shared<ApplicationData>(viewer);
    applicationData->defaultRenderPass = renderPass;
    applicationData->setup(glm::vec3(-25, -25, 0), glm::vec3(25, 25, 0), 200000);

    std::shared_ptr<pumex::SurfaceThread> thread0 = std::make_shared<CrowdRenderThread>(applicationData);
    std::shared_ptr<pumex::Surface> surface = viewer->addSurface(window, device, surfaceTraits, thread0);

    viewer->run();
  }
  catch (const std::exception e)
  {
#if defined(_DEBUG) && defined(_WIN32)
    OutputDebugStringA(e.what());
#endif
  }
  catch (...)
  {
#if defined(_DEBUG) && defined(_WIN32)
    OutputDebugStringA("Unknown error\n");
#endif
  }
  viewer->cleanup();
  FLUSH_LOG;
  return 0;
}

// Small hint : print spir-v in human readable format
// glslangvalidator -H instanced_animation.vert -o instanced_animation.vert.spv >>instanced_animation.vert.txt
// glslangvalidator -H instanced_animation.frag -o instanced_animation.frag.spv >>instanced_animation.frag.txt
