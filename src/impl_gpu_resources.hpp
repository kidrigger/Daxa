#pragma once

#include "impl_core.hpp"

#include <daxa/gpu_resources.hpp>

namespace daxa
{
    static const inline VkBufferUsageFlags BUFFER_USE_FLAGS =
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
        VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT |
        VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT |
        VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;

    static inline constexpr u32 BUFFER_BINDING = 0;
    static inline constexpr u32 STORAGE_IMAGE_BINDING = 1;
    static inline constexpr u32 SAMPLED_IMAGE_BINDING = 2;
    static inline constexpr u32 SAMPLER_BINDING = 3;
    static inline constexpr u32 BUFFER_DEVICE_ADDRESS_BUFFER_BINDING = 4;

    struct ImplBufferSlot
    {
        // Must be c version as these have ref counted dependencies that must be manually managed inside of daxa.
        daxa_BufferInfo info = {};
        VkBuffer vk_buffer = {};
        VmaAllocation vma_allocation = {};
        daxa_MemoryBlock opt_memory_block = {};
        VkDeviceAddress device_address = {};
        void * host_address = {};
    };

    static inline constexpr i32 NOT_OWNED_BY_SWAPCHAIN = -1;

    struct ImplImageViewSlot
    {
        // Must be c version as these have ref counted dependencies that must be manually managed inside of daxa.
        daxa_ImageViewInfo info = {};
        VkImageView vk_image_view = {};
    };

    struct ImplImageSlot
    {
        ImplImageViewSlot view_slot = {};
        // Must be c version as these have ref counted dependencies that must be manually managed inside of daxa.
        daxa_ImageInfo info = {};
        VkImage vk_image = {};
        VmaAllocation vma_allocation = {};
        daxa_MemoryBlock opt_memory_block = {};
        i32 swapchain_image_index = NOT_OWNED_BY_SWAPCHAIN;
        VkImageAspectFlags aspect_flags = {}; // Inferred from format.
    };

    struct ImplSamplerSlot
    {
        // Must be c version as these have ref counted dependencies that must be manually managed inside of daxa.
        daxa_SamplerInfo info = {};
        VkSampler vk_sampler = {};
    };

    /**
     * @brief GpuResourcePool is intended to be used akin to a specialized memory allocator, specific to gpu resource types (like image views).
     *
     * This struct is threadsafe if the following assumptions are met:
     * * never dereference a deleted resource
     * * never delete a resource twice
     * That means the function dereference_id can be used without synchronization, even calling get_new_slot or return_old_slot in parallel is safe.
     *
     * To check if these assumptions are met at runtime, the debug define DAXA_GPU_ID_VALIDATION can be enabled.
     * The define enables runtime checking to detect use after free and double free at the cost of performance.
     */
    template <typename ResourceT>
    struct GpuResourcePool
    {
        static constexpr inline usize MAX_RESOURCE_COUNT = 1u << 20u;
        static constexpr inline usize PAGE_BITS = 10u;
        static constexpr inline usize PAGE_SIZE = 1u << PAGE_BITS;
        static constexpr inline usize PAGE_MASK = PAGE_SIZE - 1u;
        static constexpr inline usize PAGE_COUNT = MAX_RESOURCE_COUNT / PAGE_SIZE;
        using VersionAndRefcntT = std::atomic_uint64_t;
        // TODO: split up slots into hot and cold data.
        using PageT = std::array<std::pair<ResourceT, VersionAndRefcntT>, PAGE_SIZE>;

        // TODO: replace with lockless queue.
        std::vector<u32> free_index_stack = {};
        u32 next_index = {};
        u32 max_resources = {};

        mutable std::mutex mut = {};
        std::mutex page_alloc_mtx = {};
        std::array<std::unique_ptr<PageT>, PAGE_COUNT> pages = {};
        std::atomic_uint32_t valid_page_count = {};

        // struct ExclusiveAccess
        // {
        //   private:
        //     GpuResourcePool<ResourceT> & self;
        //     std::lock_guard<std::shared_mutex> l;
        //   public:
        //     ExclusiveAccess(GpuResourcePool<ResourceT> & pool) : self{pool}, l{pool.mut} {}
        //     /**
        //      * @brief   Destroyes a slot.
        //      *          After calling this function, the id of the slot will be forever invalid.
        //      *          Index may be recycled but index + version pairs are always unique.
        //      *
        //      * Always threadsafe.
        //      * Calling this function with a non zombie id will result in undefined behavior.
        //      * WARNING: Not calling unsafe_destroy_zombie_slot at some point on a zombiefied resource causes slot leaking!
        //      */
        //     void unsafe_destroy_zombie_slot(GPUResourceId id)
        //     {
        //         auto const page = static_cast<usize>(id.index) >> PAGE_BITS;
        //         auto const offset = static_cast<usize>(id.index) & PAGE_MASK;
        //         auto const refcnt_version = std::atomic_ref{self.pages[page]->at(offset).second}.load(std::memory_order_relaxed);
        //         u64 const version = refcnt_version & DAXA_ID_VERSION_MASK;
        //         // Slots that reached max version CAN NOT be recycled.
        //         // That is because we can not guarantee uniqueness of ids when the version wraps back to 0.
        //         if (version != DAXA_ID_VERSION_MASK /* this is the maximum value a version is allowed to reach */)
        //         {
        //             self.free_index_stack.push_back(id.index);
        //         }
        //         // Clear slot:
        //         self.pages[page]->at(offset).first = {};
        //     }
        // };

        // /**
        //  * @brief   Returns exclusive accessor.
        //  *
        //  * Always threadsafe.
        //  * @return exclusive accessor.
        //  */
        // auto exclusive() -> ExclusiveAccess
        // {
        //     return ExclusiveAccess{*this};
        // }

        /**
         * @brief   Destroyes a slot.
         *          After calling this function, the id of the slot will be forever invalid.
         *          Index may be recycled but index + version pairs are always unique.
         *
         * Always threadsafe.
         * Calling this function with a non zombie id will result in undefined behavior.
         * WARNING: Not calling unsafe_destroy_zombie_slot at some point on a zombiefied resource causes slot leaking!
         */
        void unsafe_destroy_zombie_slot(GPUResourceId id)
        {
            auto const page = static_cast<usize>(id.index) >> PAGE_BITS;
            auto const offset = static_cast<usize>(id.index) & PAGE_MASK;
            auto const version = this->pages[page]->at(offset).second.load(std::memory_order_relaxed);
            // Slots that reached max version CAN NOT be recycled.
            // That is because we can not guarantee uniqueness of ids when the version wraps back to 0.
            if (version != DAXA_ID_VERSION_MASK /* this is the maximum value a version is allowed to reach */)
            {
                this->free_index_stack.push_back(id.index);
            }
            // Clear slot:
            this->pages[page]->at(offset).first = {};
        }

        /**
         * @brief   Creates a slot for a resource in the pool.
         *          Returned slots may be recycled but are guaranteed to have a unique index + version.
         *
         * Only Threadsafe when:
         * * mutable ptr to resource is only used in parallel
         *
         * @return The new resource slot and its id. Can fail if max resources is exceeded.
         */
        auto try_create_slot() -> std::optional<std::pair<GPUResourceId, ResourceT &>>
        {
            u32 index;
            if (this->free_index_stack.empty())
            {
                index = this->next_index++;
                if (index == this->max_resources || index == MAX_RESOURCE_COUNT)
                {
                    return std::nullopt;
                }
            }
            else
            {
                index = this->free_index_stack.back();
                this->free_index_stack.pop_back();
            }

            auto const page = static_cast<usize>(index) >> PAGE_BITS;
            auto const offset = static_cast<usize>(index) & PAGE_MASK;

            if (!this->pages[page])
            {
                this->pages[page] = std::make_unique<PageT>();
                for (u32 i = 0; i < PAGE_SIZE; ++i)
                {
                    this->pages[page]->at(i).second.store(1ull, std::memory_order_relaxed);
                }
                // Needs to be sequential, so that the 0 writes to the versions are visible before the atomic op.
                this->valid_page_count.fetch_add(1, std::memory_order_seq_cst);
            }

            u64 version = this->pages[page]->at(offset).second.load(std::memory_order_relaxed);

            auto const id = GPUResourceId{.index = static_cast<u64>(index), .version = version};
            return std::optional{std::pair<GPUResourceId, ResourceT &>(id, this->pages[page]->at(offset).first)};
        }

        auto try_zombiefy(GPUResourceId id) -> bool
        {
            auto const page = static_cast<usize>(id.index) >> PAGE_BITS;
            if (page >= this->valid_page_count.load(std::memory_order_relaxed))
            {
                return false;
            }
            auto const offset = static_cast<usize>(id.index) & PAGE_MASK;
            u64 version = id.version;
            u64 const new_version = version + 1;
            return (*this->pages[page])[offset].second.compare_exchange_strong(
                version, new_version,
                std::memory_order_relaxed,
                std::memory_order_relaxed);
        }

        /**
         * @brief   Checks if an id referes to a valid resource.
         *
         * Always threadsafe.
         * @returns if the given id is valid.
         */
        auto is_id_valid(GPUResourceId id) const -> bool
        {
            auto const page = static_cast<usize>(id.index) >> PAGE_BITS;
            auto const offset = static_cast<usize>(id.index) & PAGE_MASK;
            if (id.version == 0 || page >= this->valid_page_count.load(std::memory_order_relaxed))
            {
                return false;
            }
            u64 const slot_version = (*this->pages[page])[offset].second.load(std::memory_order_relaxed);
            return slot_version == id.version;
        }

        /**
         * @brief   Checks if an id referes to a valid resource.
         *          May return a random slot if the id is invalid.
         *
         * Only Threadsafe when:
         * * resource is not destroyed before the reference is used for the last time.
         *
         * @returns resource.
         */
        auto unsafe_get(GPUResourceId id) const -> ResourceT const &
        {
            auto page = static_cast<usize>(id.index) >> PAGE_BITS;
            // Even in an unsafe read we never want to read memory we do not own!
            // Clamp so we get some random slot in error case but never invalid memory!
            page = std::min(static_cast<usize>(this->valid_page_count.load(std::memory_order_relaxed)) - 1, page);
            auto const offset = static_cast<usize>(id.index) & PAGE_MASK;
            return pages[page]->at(offset).first;
        }
    };

    struct GPUShaderResourceTable
    {
        std::shared_mutex lifetime_lock = {};
        GpuResourcePool<ImplBufferSlot> buffer_slots = {};
        GpuResourcePool<ImplImageSlot> image_slots = {};
        GpuResourcePool<ImplSamplerSlot> sampler_slots = {};

        VkDescriptorSetLayout vk_descriptor_set_layout = {};
        VkDescriptorSetLayout uniform_buffer_descriptor_set_layout = {};
        VkDescriptorSet vk_descriptor_set = {};
        VkDescriptorPool vk_descriptor_pool = {};

        // Contains pipeline layouts with varying push constant range size.
        // The first size is 0 word, second is 1 word, all others are a power of two (maximum is MAX_PUSH_CONSTANT_BYTE_SIZE).
        std::array<VkPipelineLayout, PIPELINE_LAYOUT_COUNT> pipeline_layouts = {};

        void initialize(u32 max_buffers, u32 max_images, u32 max_samplers, VkDevice device, VkBuffer device_address_buffer, PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT);
        void cleanup(VkDevice device);
    };

    void write_descriptor_set_sampler(VkDevice vk_device, VkDescriptorSet vk_descriptor_set, VkSampler vk_sampler, u32 index);

    void write_descriptor_set_buffer(VkDevice vk_device, VkDescriptorSet vk_descriptor_set, VkBuffer vk_buffer, VkDeviceSize offset, VkDeviceSize range, u32 index);

    void write_descriptor_set_image(VkDevice vk_device, VkDescriptorSet vk_descriptor_set, VkImageView vk_image_view, ImageUsageFlags usage, u32 index);
} // namespace daxa
