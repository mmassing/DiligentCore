/*
 *  Copyright 2019-2021 Diligent Graphics LLC
 *  Copyright 2015-2019 Egor Yusov
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  In no event and under no legal theory, whether in tort (including negligence),
 *  contract, or otherwise, unless required by applicable law (such as deliberate
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental,
 *  or consequential damages of any character arising as a result of this License or
 *  out of the use or inability to use the software (including but not limited to damages
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and
 *  all other commercial damages or losses), even if such Contributor has been advised
 *  of the possibility of such damages.
 */

#include "DynamicTextureAtlas.h"

#include <mutex>
#include <algorithm>
#include <atomic>
#include <unordered_map>

#include "DynamicAtlasManager.hpp"
#include "ObjectBase.hpp"
#include "RefCntAutoPtr.hpp"
#include "FixedBlockMemoryAllocator.hpp"
#include "DefaultRawMemoryAllocator.hpp"
#include "GraphicsAccessories.hpp"
#include "Align.hpp"

namespace Diligent
{

class DynamicTextureAtlasImpl;

class TextureAtlasSuballocationImpl final : public ObjectBase<ITextureAtlasSuballocation>
{
public:
    using TBase = ObjectBase<ITextureAtlasSuballocation>;
    TextureAtlasSuballocationImpl(IReferenceCounters*           pRefCounters,
                                  DynamicTextureAtlasImpl*      pParentAtlas,
                                  DynamicAtlasManager::Region&& Subregion,
                                  Uint32                        Slice,
                                  Uint32                        Alignment,
                                  const uint2&                  Size) noexcept :
        // clang-format off
        TBase         {pRefCounters},
        m_pParentAtlas{pParentAtlas},
        m_Subregion   {std::move(Subregion)},
        m_Slice       {Slice},
        m_Alignment   {Alignment},
        m_Size        {Size}
    // clang-format on
    {
        VERIFY_EXPR(m_pParentAtlas);
        VERIFY_EXPR(!m_Subregion.IsEmpty());
    }

    ~TextureAtlasSuballocationImpl();

    IMPLEMENT_QUERY_INTERFACE_IN_PLACE(IID_TextureAtlasSuballocation, TBase)

    virtual Atomics::Long DILIGENT_CALL_TYPE Release() override final
    {
        RefCntAutoPtr<DynamicTextureAtlasImpl> pAtlas;
        return TBase::Release(
            [&]() //
            {
                // We must keep parent alive while this object is being destroyed because
                // the parent keeps the memory allocator for the object.
                pAtlas = m_pParentAtlas;
            });
    }

    virtual uint2 GetOrigin() const override final
    {
        return uint2 //
            {
                m_Subregion.x * m_Alignment,
                m_Subregion.y * m_Alignment //
            };
    }

    virtual Uint32 GetSlice() const override final
    {
        return m_Slice;
    }

    virtual uint2 GetSize() const override final
    {
        return m_Size;
    }

    virtual float4 GetUVScaleBias() const override final;

    virtual IDynamicTextureAtlas* GetAtlas() override final;

    virtual void SetUserData(IObject* pUserData) override final
    {
        m_pUserData = pUserData;
    }

    virtual IObject* GetUserData() const override final
    {
        return m_pUserData.RawPtr<IObject>();
    }

private:
    RefCntAutoPtr<DynamicTextureAtlasImpl> m_pParentAtlas;

    DynamicAtlasManager::Region m_Subregion;

    const Uint32 m_Slice;
    const Uint32 m_Alignment;
    const uint2  m_Size;

    RefCntAutoPtr<IObject> m_pUserData;
};


class DynamicTextureAtlasImpl final : public ObjectBase<IDynamicTextureAtlas>
{
public:
    using TBase = ObjectBase<IDynamicTextureAtlas>;

    DynamicTextureAtlasImpl(IReferenceCounters*                  pRefCounters,
                            IRenderDevice*                       pDevice,
                            const DynamicTextureAtlasCreateInfo& CreateInfo) :
        // clang-format off
        TBase             {pRefCounters},
        m_Desc            {CreateInfo.Desc},
        m_Name            {CreateInfo.Desc.Name != nullptr ? CreateInfo.Desc.Name : "Dynamic texture atlas"},
        m_MinAlignment    {CreateInfo.MinAlignment},
        m_ExtraSliceCount {CreateInfo.ExtraSliceCount},
        m_MaxSliceCount   {CreateInfo.Desc.Type == RESOURCE_DIM_TEX_2D_ARRAY ? std::min(CreateInfo.MaxSliceCount, Uint32{2048}) : 1},
        m_SuballocationsAllocator
        {
            DefaultRawMemoryAllocator::GetAllocator(),
            sizeof(TextureAtlasSuballocationImpl),
            CreateInfo.SuballocationObjAllocationGranularity
        }
    // clang-format on
    {
        if (m_Desc.Type != RESOURCE_DIM_TEX_2D && m_Desc.Type != RESOURCE_DIM_TEX_2D_ARRAY)
            LOG_ERROR_AND_THROW(GetResourceDimString(m_Desc.Type), " is not a valid resource dimension. Only 2D and 2D array textures are allowed");

        if (m_Desc.Format == TEX_FORMAT_UNKNOWN)
            LOG_ERROR_AND_THROW("Texture format must not be UNKNOWN");

        if (m_Desc.Width == 0)
            LOG_ERROR_AND_THROW("Texture width must not be zero");

        if (m_Desc.Height == 0)
            LOG_ERROR_AND_THROW("Texture height must not be zero");

        if (m_MinAlignment != 0)
        {
            if (!IsPowerOfTwo(m_MinAlignment))
                LOG_ERROR_AND_THROW("Minimum alignment (", m_MinAlignment, ") is not a power of two");

            if ((m_Desc.Width % m_MinAlignment) != 0)
                LOG_ERROR_AND_THROW("Texture width (", m_Desc.Width, ") is not a multiple of minimum alignment (", m_MinAlignment, ")");

            if ((m_Desc.Height % m_MinAlignment) != 0)
                LOG_ERROR_AND_THROW("Texture height (", m_Desc.Height, ") is not a multiple of minimum alignment (", m_MinAlignment, ")");
        }

        m_Desc.Name = m_Name.c_str();
        m_Slices.resize(m_Desc.ArraySize);

        if (pDevice == nullptr)
            m_Desc.ArraySize = 0;
        if (m_Desc.ArraySize > 0)
        {
            pDevice->CreateTexture(m_Desc, nullptr, &m_pTexture);
            if (!m_pTexture)
                LOG_ERROR_AND_THROW("Failed to create texture atlas texture");
        }

        m_Version.store(0);
    }

    ~DynamicTextureAtlasImpl()
    {
        VERIFY_EXPR(m_AllocatedArea.load() == 0);
        VERIFY_EXPR(m_UsedArea.load() == 0);
        VERIFY_EXPR(m_AllocationCount.load() == 0);
    }

    IMPLEMENT_QUERY_INTERFACE_IN_PLACE(IID_DynamicTextureAtlas, TBase)

    virtual ITexture* GetTexture(IRenderDevice* pDevice, IDeviceContext* pContext) override final
    {
        Uint32 ArraySize = 0;
        {
            std::lock_guard<std::mutex> Lock{m_SlicesMtx};
            ArraySize = static_cast<Uint32>(m_Slices.size());
        }
        if (m_Desc.ArraySize != ArraySize)
        {
            DEV_CHECK_ERR(pDevice != nullptr && pContext != nullptr,
                          "Texture atlas must be resized, but pDevice or pContext is null");

            m_Desc.ArraySize = ArraySize;
            RefCntAutoPtr<ITexture> pNewTexture;
            pDevice->CreateTexture(m_Desc, nullptr, &pNewTexture);
            VERIFY_EXPR(pNewTexture);
            m_Version.fetch_add(1);

            LOG_INFO_MESSAGE("Dynamic texture atlas: expanding texture array '", m_Desc.Name,
                             "' (", m_Desc.Width, " x ", m_Desc.Height, " ", m_Desc.MipLevels, "-mip ",
                             GetTextureFormatAttribs(m_Desc.Format).Name, ") to ",
                             m_Desc.ArraySize, " slices. Version: ", GetVersion());

            if (m_pTexture)
            {
                const auto& StaleTexDesc = m_pTexture->GetDesc();

                CopyTextureAttribs CopyAttribs;
                CopyAttribs.pSrcTexture              = m_pTexture;
                CopyAttribs.pDstTexture              = pNewTexture;
                CopyAttribs.SrcTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
                CopyAttribs.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;

                for (Uint32 slice = 0; slice < StaleTexDesc.ArraySize; ++slice)
                {
                    for (Uint32 mip = 0; mip < StaleTexDesc.MipLevels; ++mip)
                    {
                        CopyAttribs.SrcSlice    = slice;
                        CopyAttribs.DstSlice    = slice;
                        CopyAttribs.SrcMipLevel = mip;
                        CopyAttribs.DstMipLevel = mip;
                        pContext->CopyTexture(CopyAttribs);
                    }
                }
            }

            m_pTexture = std::move(pNewTexture);
        }

        return m_pTexture;
    }

    virtual void Allocate(Uint32                       Width,
                          Uint32                       Height,
                          ITextureAtlasSuballocation** ppSuballocation) override final
    {
        if (Width == 0 || Height == 0)
        {
            UNEXPECTED("Subregion size must not be zero");
            return;
        }

        if (Width > m_Desc.Width || Height > m_Desc.Height)
        {
            LOG_ERROR_MESSAGE("Requested region size ", Width, " x ", Height, " exceeds atlas dimensions ", m_Desc.Width, " x ", m_Desc.Height);
            return;
        }

        Uint32 Alignment = m_MinAlignment;
        if (Alignment > 0)
        {
            while (std::min(Width, Height) > Alignment)
                Alignment *= 2;
        }
        else
        {
            Alignment = 1;
        }
        const auto AlignedWidth  = AlignUp(Width, Alignment);
        const auto AlignedHeight = AlignUp(Height, Alignment);

        DynamicAtlasManager::Region Subregion;

        Uint32 Slice = 0;
        while (Slice < m_MaxSliceCount)
        {
            SliceManager* pSliceMgr = nullptr;
            {
                std::lock_guard<std::mutex> Lock{m_SlicesMtx};

                // Get the list of slices for this alignment
                auto SlicesIt = m_AlignmentToSlice.find(Alignment);
                if (SlicesIt == m_AlignmentToSlice.end())
                    SlicesIt = m_AlignmentToSlice.emplace(Alignment, std::vector<Uint32>{}).first;

                // Find the next slice in the list
                auto& Slices  = SlicesIt->second;
                auto  SliceIt = Slices.begin();
                while (SliceIt != Slices.end() && Slice > *SliceIt)
                    ++SliceIt;
                Slice = SliceIt != Slices.end() ? *SliceIt : m_NextUnusedSlice;

                if (Slice == m_NextUnusedSlice)
                {
                    if (Slice == m_MaxSliceCount)
                        break;

                    while (Slice >= m_Slices.size())
                    {
                        const auto ExtraSliceCount = m_ExtraSliceCount != 0 ?
                            m_ExtraSliceCount :
                            static_cast<Uint32>(m_Slices.size());

                        m_Slices.resize(std::min(Slice + ExtraSliceCount, m_MaxSliceCount));
                    }

                    VERIFY(std::find(Slices.begin(), Slices.end(), Slice) == Slices.end(), "Slice ", Slice, " is already in the list for this alignment. This is a bug");
                    Slices.push_back(Slice);

                    VERIFY(!m_Slices[Slice], "Slice ", Slice, " has already been initialized. This is a bug.");
                    VERIFY_EXPR(m_Desc.Width >= Alignment && m_Desc.Height >= Alignment);
                    m_Slices[Slice] = std::make_unique<SliceManager>(m_Desc.Width / Alignment, m_Desc.Height / Alignment);

                    ++m_NextUnusedSlice;
                }

                pSliceMgr = m_Slices[Slice].get();
            }

            if (pSliceMgr != nullptr)
            {
                Subregion = pSliceMgr->Allocate(AlignedWidth / Alignment, AlignedHeight / Alignment);
                if (!Subregion.IsEmpty())
                    break;
            }

            ++Slice;
        }

        if (Subregion.IsEmpty())
        {
            LOG_ERROR_MESSAGE("Failed to suballocate texture subregion ", Width, " x ", Height, " from texture atlas");
            return;
        }

        m_AllocatedArea.fetch_add(Int64{Width} * Int64{Height});
        m_UsedArea.fetch_add(Int64{AlignedWidth} * Int64{AlignedHeight});
        m_AllocationCount.fetch_add(1);

        // clang-format off
        TextureAtlasSuballocationImpl* pSuballocation{
            NEW_RC_OBJ(m_SuballocationsAllocator, "TextureAtlasSuballocationImpl instance", TextureAtlasSuballocationImpl)
            (
                this,
                std::move(Subregion),
                Slice,
                Alignment,
                uint2{Width, Height}
            )
        };
        // clang-format on

        pSuballocation->QueryInterface(IID_TextureAtlasSuballocation, reinterpret_cast<IObject**>(ppSuballocation));
    }

    void Free(Uint32 Slice, Uint32 Alignment, DynamicAtlasManager::Region&& Subregion, Uint32 Width, Uint32 Height)
    {
        m_AllocatedArea.fetch_add(-Int64{Width} * Int64{Height});
        m_UsedArea.fetch_add(-Int64{Subregion.width * Alignment} * Int64{Subregion.height * Alignment});
        m_AllocationCount.fetch_add(-1);

        SliceManager* pSliceMgr = nullptr;
        {
            std::lock_guard<std::mutex> Lock{m_SlicesMtx};
#ifdef DILIGENT_DEBUG
            {
                auto SlicesIt = m_AlignmentToSlice.find(Alignment);
                VERIFY(SlicesIt != m_AlignmentToSlice.end(), "There are no slices with alignment ", Alignment);
                const auto& Slices = SlicesIt->second;
                VERIFY(std::find(Slices.begin(), Slices.end(), Slice) != Slices.end(), "Slice ", Slice, " does not use alignment ", Alignment);
            }
#endif
            pSliceMgr = m_Slices[Slice].get();
        }
        pSliceMgr->Free(std::move(Subregion));
    }

    virtual const TextureDesc& GetAtlasDesc() const override final
    {
        return m_Desc;
    }

    virtual Uint32 GetVersion() const override final
    {
        return m_Version.load();
    }

    void GetUsageStats(DynamicTextureAtlasUsageStats& Stats) const override final
    {
        Stats.Size = 0;
        for (Uint32 mip = 0; mip < m_Desc.MipLevels; ++mip)
            Stats.Size += GetMipLevelProperties(m_Desc, mip).MipSize;
        Stats.Size *= m_Desc.ArraySize;

        Stats.AllocationCount = m_AllocationCount.load();

        Stats.TotalArea     = Uint64{m_Desc.Width} * Uint64{m_Desc.Height} * Uint64{m_Desc.ArraySize};
        Stats.AllocatedArea = m_AllocatedArea.load();
        Stats.UsedArea      = m_UsedArea.load();
    }

private:
    TextureDesc       m_Desc;
    const std::string m_Name;

    const Uint32 m_MinAlignment;
    const Uint32 m_ExtraSliceCount;
    const Uint32 m_MaxSliceCount;

    RefCntAutoPtr<ITexture> m_pTexture;

    FixedBlockMemoryAllocator m_SuballocationsAllocator;

    std::atomic<Uint32> m_Version{0};
    std::atomic<Int32>  m_AllocationCount{0};

    std::atomic<Int64> m_AllocatedArea{0};
    std::atomic<Int64> m_UsedArea{0};


    struct SliceManager
    {
        SliceManager(Uint32 Width, Uint32 Height) :
            Mgr{Width, Height}
        {}

        DynamicAtlasManager::Region Allocate(Uint32 Width, Uint32 Height)
        {
            std::lock_guard<std::mutex> Lock{Mtx};
            return Mgr.Allocate(Width, Height);
        }
        void Free(DynamicAtlasManager::Region&& Region)
        {
            std::lock_guard<std::mutex> Lock{Mtx};
            Mgr.Free(std::move(Region));
        }

    private:
        std::mutex          Mtx;
        DynamicAtlasManager Mgr;
    };
    std::mutex                                      m_SlicesMtx;
    std::vector<std::unique_ptr<SliceManager>>      m_Slices;
    std::unordered_map<Uint32, std::vector<Uint32>> m_AlignmentToSlice;
    Uint32                                          m_NextUnusedSlice = 0;
};


TextureAtlasSuballocationImpl::~TextureAtlasSuballocationImpl()
{
    m_pParentAtlas->Free(m_Slice, m_Alignment, std::move(m_Subregion), m_Size.x, m_Size.y);
}

IDynamicTextureAtlas* TextureAtlasSuballocationImpl::GetAtlas()
{
    return m_pParentAtlas;
}

float4 TextureAtlasSuballocationImpl::GetUVScaleBias() const
{
    const auto  Origin    = GetOrigin().Recast<float>();
    const auto  Size      = GetSize().Recast<float>();
    const auto& AtlasDesc = m_pParentAtlas->GetAtlasDesc();
    return float4 //
        {
            Size.x / static_cast<float>(AtlasDesc.Width),
            Size.y / static_cast<float>(AtlasDesc.Height),
            Origin.x / static_cast<float>(AtlasDesc.Width),
            Origin.y / static_cast<float>(AtlasDesc.Height) //
        };
}


void CreateDynamicTextureAtlas(IRenderDevice*                       pDevice,
                               const DynamicTextureAtlasCreateInfo& CreateInfo,
                               IDynamicTextureAtlas**               ppAtlas)
{
    try
    {
        auto* pAllocator = MakeNewRCObj<DynamicTextureAtlasImpl>()(pDevice, CreateInfo);
        pAllocator->QueryInterface(IID_DynamicTextureAtlas, reinterpret_cast<IObject**>(ppAtlas));
    }
    catch (...)
    {
        LOG_ERROR_MESSAGE("Failed to create buffer suballocator");
    }
}

} // namespace Diligent
