/*******************************************************************************
    Copyright (c) 2015-2019 NVIDIA Corporation

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

        The above copyright notice and this permission notice shall be
        included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

*******************************************************************************/

#include "uvm8_api.h"
#include "uvm8_va_space.h"
#include "uvm8_va_range.h"
#include "uvm8_lock.h"
#include "uvm8_global.h"
#include "uvm8_kvmalloc.h"
#include "uvm8_perf_heuristics.h"
#include "uvm8_user_channel.h"
#include "uvm8_tools.h"
#include "uvm8_thread_context.h"
#include "uvm8_hal.h"
#include "uvm8_map_external.h"
#include "uvm8_ats_ibm.h"
#include "uvm8_gpu_access_counters.h"
#include "uvm8_hmm.h"
#include "uvm8_test.h"
#include "uvm_common.h"
#include "nv_uvm_interface.h"
#include "nv-kthread-q.h"
#include "uvm8_nvmgpu.h"

static bool processor_mask_array_test(const uvm_processor_mask_t *mask,
                                      uvm_processor_id_t mask_id,
                                      uvm_processor_id_t id)
{
    return uvm_processor_mask_test(&mask[uvm_id_value(mask_id)], id);
}

static void processor_mask_array_clear(uvm_processor_mask_t *mask,
                                       uvm_processor_id_t mask_id,
                                       uvm_processor_id_t id)
{
    uvm_processor_mask_clear(&mask[uvm_id_value(mask_id)], id);
}

static void processor_mask_array_set(uvm_processor_mask_t *mask,
                                     uvm_processor_id_t mask_id,
                                     uvm_processor_id_t id)
{
    uvm_processor_mask_set(&mask[uvm_id_value(mask_id)], id);
}

static bool processor_mask_array_empty(const uvm_processor_mask_t *mask, uvm_processor_id_t mask_id)
{
    return uvm_processor_mask_empty(&mask[uvm_id_value(mask_id)]);
}

static NV_STATUS enable_peers(uvm_va_space_t *va_space, uvm_gpu_t *gpu0, uvm_gpu_t *gpu1);
static void disable_peers(uvm_va_space_t *va_space,
                          uvm_gpu_t *gpu0,
                          uvm_gpu_t *gpu1,
                          struct list_head *deferred_free_list);
static void remove_gpu_va_space(uvm_gpu_va_space_t *gpu_va_space, struct list_head *deferred_free_list);
static void va_space_remove_dummy_thread_contexts(uvm_va_space_t *va_space);

static void init_tools_data(uvm_va_space_t *va_space)
{
    int i;

    uvm_init_rwsem(&va_space->tools.lock, UVM_LOCK_ORDER_VA_SPACE_TOOLS);

    for (i = 0; i < ARRAY_SIZE(va_space->tools.counters); i++)
        INIT_LIST_HEAD(va_space->tools.counters + i);
    for (i = 0; i < ARRAY_SIZE(va_space->tools.queues); i++)
        INIT_LIST_HEAD(va_space->tools.queues + i);
}

static NV_STATUS register_gpu_nvlink_peers(uvm_va_space_t *va_space, uvm_gpu_t *gpu)
{
    uvm_gpu_t *other_gpu;

    uvm_assert_rwsem_locked(&va_space->lock);

    for_each_va_space_gpu(other_gpu, va_space) {
        uvm_gpu_peer_t *peer_caps;

        if (uvm_id_equal(other_gpu->id, gpu->id))
            continue;

        peer_caps = uvm_gpu_peer_caps(gpu, other_gpu);

        if (peer_caps->link_type >= UVM_GPU_LINK_NVLINK_1) {
            NV_STATUS status = enable_peers(va_space, gpu, other_gpu);
            if (status != NV_OK)
                return status;
        }
    }

    return NV_OK;
}

NV_STATUS uvm_va_space_create(struct inode *inode, struct file *filp)
{
    NV_STATUS status;
    uvm_va_space_t *va_space = uvm_kvmalloc_zero(sizeof(*va_space));
    uvm_gpu_id_t gpu_id;

    if (!va_space)
        return NV_ERR_NO_MEMORY;

    uvm_init_rwsem(&va_space->lock, UVM_LOCK_ORDER_VA_SPACE);
    uvm_mutex_init(&va_space->serialize_writers_lock, UVM_LOCK_ORDER_VA_SPACE_SERIALIZE_WRITERS);
    uvm_mutex_init(&va_space->read_acquire_write_release_lock,
                   UVM_LOCK_ORDER_VA_SPACE_READ_ACQUIRE_WRITE_RELEASE_LOCK);
    uvm_spin_lock_init(&va_space->va_space_mm.lock, UVM_LOCK_ORDER_LEAF);
    uvm_range_tree_init(&va_space->va_range_tree);
    uvm_rwlock_irqsave_init(&va_space->ats.rwlock, UVM_LOCK_ORDER_LEAF);

    // By default all struct files on the same inode share the same
    // address_space structure (the inode's) across all processes. This means
    // unmap_mapping_range would unmap virtual mappings across all processes on
    // that inode.
    //
    // Since the UVM driver uses the mapping offset as the VA of the file's
    // process, we need to isolate the mappings to each process.
    address_space_init_once(&va_space->mapping);
    va_space->mapping.host = inode;

    // Some paths in the kernel, for example force_page_cache_readahead which
    // can be invoked from user-space via madvise MADV_WILLNEED and fadvise
    // POSIX_FADV_WILLNEED, check the function pointers within
    // file->f_mapping->a_ops for validity. However, those paths assume that a_ops
    // itself is always valid. Handle that by using the inode's a_ops pointer,
    // which is what f_mapping->a_ops would point to anyway if we weren't re-
    // assigning f_mapping.
    va_space->mapping.a_ops = inode->i_mapping->a_ops;

#if defined(NV_ADDRESS_SPACE_HAS_BACKING_DEV_INFO)
    va_space->mapping.backing_dev_info = inode->i_mapping->backing_dev_info;
#endif

    // Init to 0 since we rely on atomic_inc_return behavior to return 1 as the first ID
    atomic64_set(&va_space->range_group_id_counter, 0);

    INIT_RADIX_TREE(&va_space->range_groups, NV_UVM_GFP_FLAGS);
    uvm_range_tree_init(&va_space->range_group_ranges);

    bitmap_zero(va_space->enabled_peers, UVM_MAX_UNIQUE_GPU_PAIRS);

    // CPU is not explicitly registered in the va space
    processor_mask_array_set(va_space->can_access, UVM_ID_CPU, UVM_ID_CPU);
    processor_mask_array_set(va_space->accessible_from, UVM_ID_CPU, UVM_ID_CPU);
    processor_mask_array_set(va_space->can_copy_from, UVM_ID_CPU, UVM_ID_CPU);
    processor_mask_array_set(va_space->has_native_atomics, UVM_ID_CPU, UVM_ID_CPU);

    // CPU always participates in system-wide atomics
    uvm_processor_mask_set(&va_space->system_wide_atomics_enabled_processors, UVM_ID_CPU);
    uvm_processor_mask_set(&va_space->faultable_processors, UVM_ID_CPU);

    // Initialize the CPU/GPU affinity array. New CPU NUMA nodes are added at
    // GPU registration time, but they are never freed on unregister_gpu
    // (although the GPU is removed from the corresponding mask).
    for_each_gpu_id(gpu_id) {
        uvm_cpu_gpu_affinity_t *affinity = &va_space->gpu_cpu_numa_affinity[uvm_id_gpu_index(gpu_id)];

        affinity->numa_node = -1;
        uvm_processor_mask_zero(&affinity->gpus);
    }

    init_waitqueue_head(&va_space->va_space_mm.last_retainer_wait_queue);
    init_waitqueue_head(&va_space->gpu_va_space_deferred_free.wait_queue);

    filp->private_data = va_space;
    filp->f_mapping = &va_space->mapping;

    va_space->test.page_prefetch_enabled = true;

    init_tools_data(va_space);

    uvm_va_space_down_write(va_space);

    status = uvm_perf_init_va_space_events(va_space, &va_space->perf_events);
    if (status != NV_OK)
        goto fail;

    status = uvm_perf_heuristics_load(va_space);
    if (status != NV_OK)
        goto fail;

    status = uvm_gpu_init_va_space(va_space);
    if (status != NV_OK)
        goto fail;

    uvm_va_space_up_write(va_space);

    uvm_mutex_lock(&g_uvm_global.va_spaces.lock);
    list_add_tail(&va_space->list_node, &g_uvm_global.va_spaces.list);
    uvm_mutex_unlock(&g_uvm_global.va_spaces.lock);

    va_space->nvmgpu_va_space.fd_pending = -1;

    return NV_OK;

fail:
    uvm_perf_heuristics_unload(va_space);
    uvm_perf_destroy_va_space_events(&va_space->perf_events);
    uvm_va_space_up_write(va_space);

    uvm_kvfree(va_space);

    return status;
}

// This function does *not* release the GPU, nor the GPU's PCIE peer pairings.
// Those are returned so the caller can do it after dropping the VA space lock.
static void unregister_gpu(uvm_va_space_t *va_space,
                           uvm_gpu_t *gpu,
                           struct list_head *deferred_free_list,
                           uvm_global_processor_mask_t *peers_to_release)
{
    uvm_gpu_t *peer_gpu;
    uvm_va_range_t *va_range;
    NvU32 peer_table_index;

    uvm_assert_rwsem_locked_write(&va_space->lock);

    if (peers_to_release)
        uvm_global_processor_mask_zero(peers_to_release);

    // If a GPU VA Space was explicitly registered, but not explicitly
    // unregistered, unregister it and add all of its objects to the free list.
    remove_gpu_va_space(uvm_gpu_va_space_get(va_space, gpu), deferred_free_list);

    uvm_for_each_va_range(va_range, va_space)
        uvm_va_range_unregister_gpu(va_range, gpu, deferred_free_list);

    // If this GPU has any peer-to-peer pair that was explicitly enabled, but
    // not explicitly disabled, disable it.
    // Notably do this only after unregistering the GPU from VA ranges to make
    // sure there is no pending work using the peer mappings within the VA
    // blocks (in particular migrations using the peer identity mappings).
    for_each_va_space_gpu(peer_gpu, va_space) {
        if (gpu == peer_gpu)
            continue;

        peer_table_index = uvm_gpu_peer_table_index(gpu->id, peer_gpu->id);
        if (test_bit(peer_table_index, va_space->enabled_peers)) {
            disable_peers(va_space, gpu, peer_gpu, deferred_free_list);

            // Only PCIE peers need to be globally released. NVLINK peers are
            // brought up and torn down automatically within add_gpu and
            // remove_gpu.
            if (peers_to_release && g_uvm_global.peers[peer_table_index].link_type == UVM_GPU_LINK_PCIE)
                uvm_global_processor_mask_set(peers_to_release, peer_gpu->global_id);
        }
    }

    if (gpu->isr.replayable_faults.handling)
        uvm_processor_mask_clear(&va_space->faultable_processors, gpu->id);

    uvm_processor_mask_clear(&va_space->system_wide_atomics_enabled_processors, gpu->id);

    processor_mask_array_clear(va_space->can_access, gpu->id, gpu->id);
    processor_mask_array_clear(va_space->can_access, gpu->id, UVM_ID_CPU);
    processor_mask_array_clear(va_space->can_access, UVM_ID_CPU, gpu->id);
    UVM_ASSERT(processor_mask_array_empty(va_space->can_access, gpu->id));

    processor_mask_array_clear(va_space->accessible_from, gpu->id, gpu->id);
    processor_mask_array_clear(va_space->accessible_from, gpu->id, UVM_ID_CPU);
    processor_mask_array_clear(va_space->accessible_from, UVM_ID_CPU, gpu->id);
    UVM_ASSERT(processor_mask_array_empty(va_space->accessible_from, gpu->id));

    processor_mask_array_clear(va_space->can_copy_from, gpu->id, gpu->id);
    processor_mask_array_clear(va_space->can_copy_from, gpu->id, UVM_ID_CPU);
    processor_mask_array_clear(va_space->can_copy_from, UVM_ID_CPU, gpu->id);
    UVM_ASSERT(processor_mask_array_empty(va_space->can_copy_from, gpu->id));

    processor_mask_array_clear(va_space->has_nvlink, gpu->id, UVM_ID_CPU);
    processor_mask_array_clear(va_space->has_nvlink, UVM_ID_CPU, gpu->id);
    UVM_ASSERT(processor_mask_array_empty(va_space->has_nvlink, gpu->id));

    UVM_ASSERT(processor_mask_array_empty(va_space->indirect_peers, gpu->id));

    processor_mask_array_clear(va_space->has_native_atomics, gpu->id, gpu->id);
    processor_mask_array_clear(va_space->has_native_atomics, gpu->id, UVM_ID_CPU);
    processor_mask_array_clear(va_space->has_native_atomics, UVM_ID_CPU, gpu->id);
    UVM_ASSERT(processor_mask_array_empty(va_space->has_native_atomics, gpu->id));

    uvm_processor_mask_clear(&va_space->registered_gpus, gpu->id);
    va_space->registered_gpus_table[uvm_id_gpu_index(gpu->id)] = NULL;

    // Remove the GPU from the CPU/GPU affinity masks
    if (gpu->closest_cpu_numa_node != -1) {
        uvm_gpu_id_t gpu_id;

        for_each_gpu_id(gpu_id) {
            uvm_cpu_gpu_affinity_t *affinity = &va_space->gpu_cpu_numa_affinity[uvm_id_gpu_index(gpu_id)];

            if (affinity->numa_node == gpu->closest_cpu_numa_node) {
                uvm_processor_mask_clear(&affinity->gpus, gpu->id);
                break;
            }
        }
    }
}

static void gpu_va_space_stop_all_channels(uvm_gpu_va_space_t *gpu_va_space)
{
    uvm_user_channel_t *user_channel;

    list_for_each_entry(user_channel, &gpu_va_space->registered_channels, list_node)
        uvm_user_channel_stop(user_channel);

    // Prevent new channels from being registered since we'll be dropping the
    // VA space lock shortly with the expectation that no more channels will
    // arrive.
    atomic_set(&gpu_va_space->disallow_new_channels, 1);
}

// Detaches (unregisters) all user channels in a GPU VA space. The channels must
// have previously been stopped.
//
// The detached channels are added to the input list. The caller is expected to
// drop the VA space lock and call uvm_deferred_free_object_list to complete the
// destroy operation.
static void uvm_gpu_va_space_detach_all_user_channels(uvm_gpu_va_space_t *gpu_va_space,
                                                      struct list_head *deferred_free_list)
{
    uvm_user_channel_t *user_channel, *next_channel;
    list_for_each_entry_safe(user_channel, next_channel, &gpu_va_space->registered_channels, list_node)
        uvm_user_channel_detach(user_channel, deferred_free_list);
}

void uvm_va_space_detach_all_user_channels(uvm_va_space_t *va_space, struct list_head *deferred_free_list)
{
    uvm_gpu_va_space_t *gpu_va_space;
    for_each_gpu_va_space(gpu_va_space, va_space)
        uvm_gpu_va_space_detach_all_user_channels(gpu_va_space, deferred_free_list);
}

void uvm_va_space_destroy(uvm_va_space_t *va_space)
{
    uvm_va_range_t *va_range, *va_range_next;
    uvm_gpu_t *gpu;
    uvm_gpu_id_t gpu_id;
    uvm_global_gpu_id_t global_gpu_id;
    uvm_global_processor_mask_t retained_gpus;
    LIST_HEAD(deferred_free_list);

    stop_pagecache_reducer(va_space);

    // Remove the VA space from the global list before we start tearing things
    // down so other threads can't see the VA space in a partially-valid state.
    uvm_mutex_lock(&g_uvm_global.va_spaces.lock);
    list_del(&va_space->list_node);
    uvm_mutex_unlock(&g_uvm_global.va_spaces.lock);

    uvm_perf_heuristics_stop(va_space);

    // Stop all channels before unmapping anything. This kills the channels and
    // prevents spurious MMU faults from being generated (bug 1722021), but
    // doesn't prevent the bottom half from servicing old faults for those
    // channels.
    //
    // This involves making RM calls, so we have to do that with the VA space
    // lock in read mode.
    uvm_va_space_down_read_rm(va_space);
    uvm_va_space_stop_all_user_channels(va_space);
    uvm_va_space_up_read_rm(va_space);

    // The bottom half GPU page fault handler(s) could still look up and use
    // this va_space via the GPU's instance_ptr_table. Lock them out while we
    // tear down. Once we're done, the bottom half will fail to find any
    // registered GPUs in the VA space, so those faults will be canceled.
    uvm_va_space_down_write(va_space);

    uvm_hmm_mirror_unregister(va_space);

    uvm_va_space_global_gpus(va_space, &retained_gpus);

    bitmap_copy(va_space->enabled_peers_teardown, va_space->enabled_peers, UVM_MAX_UNIQUE_GPU_PAIRS);

    uvm_va_space_detach_all_user_channels(va_space, &deferred_free_list);

    // Destroy all VA ranges. We do this before unregistering the GPUs for
    // performance, since GPU unregister will walk all VA ranges in the VA space
    // multiple times.
    uvm_for_each_va_range_safe(va_range, va_range_next, va_space) {
        // All channel ranges should've been destroyed by the channel unregister
        // above
        UVM_ASSERT(va_range->type != UVM_VA_RANGE_TYPE_CHANNEL);
        uvm_va_range_destroy(va_range, &deferred_free_list);
    }

    uvm_range_group_radix_tree_destroy(va_space);

    // Unregister all GPUs in the VA space. Note that this does not release the
    // GPUs nor peers. We do that below.
    for_each_va_space_gpu(gpu, va_space)
        unregister_gpu(va_space, gpu, &deferred_free_list, NULL);

    uvm_perf_heuristics_unload(va_space);
    uvm_perf_destroy_va_space_events(&va_space->perf_events);

    va_space_remove_dummy_thread_contexts(va_space);

    uvm_va_space_up_write(va_space);

    UVM_ASSERT(uvm_processor_mask_empty(&va_space->registered_gpus));
    UVM_ASSERT(uvm_processor_mask_empty(&va_space->registered_gpu_va_spaces));

    for_each_gpu_id(gpu_id)
        UVM_ASSERT(va_space->registered_gpus_table[uvm_id_gpu_index(gpu_id)] == NULL);

    // The instance pointer mappings for this VA space have been removed so no
    // new bottom halves can get to this VA space, but there could still be
    // bottom halves running from before we removed the mapping. Rather than
    // ref-count the VA space, just wait for them to finish.
    //
    // This is also required to synchronize any pending
    // block_deferred_accessed_by() work items.

    nv_kthread_q_flush(&g_uvm_global.global_q);

    for_each_global_gpu_in_mask(gpu, &retained_gpus) {
        if (!gpu->isr.replayable_faults.handling) {
            UVM_ASSERT(!gpu->isr.non_replayable_faults.handling);
            continue;
        }

        nv_kthread_q_flush(&gpu->isr.bottom_half_q);

        // The same applies to the kill channel kthreads. However, they need to
        // be flushed after their bottom-half counterparts since the latter may
        // schedule a channel kill.
        if (gpu->isr.non_replayable_faults.handling)
            nv_kthread_q_flush(&gpu->isr.kill_channel_q);

        if (gpu->access_counters_supported)
            uvm_gpu_access_counters_disable(gpu, va_space);
    }

    // Check that all CPU/GPU affinity masks are empty
    for_each_gpu_id(gpu_id) {
        const uvm_cpu_gpu_affinity_t *affinity = &va_space->gpu_cpu_numa_affinity[uvm_id_gpu_index(gpu_id)];

        UVM_ASSERT(uvm_processor_mask_empty(&affinity->gpus));
    }

    // ensure that there are no pending events that refer to this va_space
    uvm_tools_flush_events();

    // Perform cleanup we can't do while holding the VA space lock

    uvm_deferred_free_object_list(&deferred_free_list);

    // Remove the mm_struct association on this VA space, if any. This may
    // invoke uvm_va_space_mm_shutdown(), which in turn will disable all
    // channels and wait for any retainers to finish, so it has to be done
    // outside of the VA space lock.
    //
    // Since we must already handle mm shutdown being called at any point prior
    // to this call, this call can be made at any point in
    // uvm_va_space_destroy(). It's beneficial to do it late after doing all
    // deferred frees for GPU VA spaces and channels, because then
    // uvm_va_space_mm_shutdown() will have minimal work to do.
    uvm_va_space_mm_unregister(va_space);

    uvm_mutex_lock(&g_uvm_global.global_lock);

    // Release the GPUs and their peer counts. Do not use
    // for_each_global_gpu_in_mask for the outer loop as it reads the GPU
    // state, which might get destroyed.
    for_each_global_gpu_id_in_mask(global_gpu_id, &retained_gpus) {
        uvm_gpu_t *peer_gpu;

        gpu = uvm_gpu_get(global_gpu_id);

        uvm_global_processor_mask_clear(&retained_gpus, global_gpu_id);

        for_each_global_gpu_in_mask(peer_gpu, &retained_gpus) {
            NvU32 peer_table_index = uvm_gpu_peer_table_index(gpu->id, peer_gpu->id);
            if (test_bit(peer_table_index, va_space->enabled_peers_teardown)) {
                uvm_gpu_peer_t *peer_caps = &g_uvm_global.peers[peer_table_index];

                if (peer_caps->link_type == UVM_GPU_LINK_PCIE)
                    uvm_gpu_release_pcie_peer_access(gpu, peer_gpu);

                __clear_bit(peer_table_index, va_space->enabled_peers_teardown);
            }
        }

        uvm_gpu_release_locked(gpu);
    }

    UVM_ASSERT(bitmap_empty(va_space->enabled_peers, UVM_MAX_UNIQUE_GPU_PAIRS));
    UVM_ASSERT(bitmap_empty(va_space->enabled_peers_teardown, UVM_MAX_UNIQUE_GPU_PAIRS));

    uvm_mutex_unlock(&g_uvm_global.global_lock);

    uvm_kvfree(va_space);
}

NV_STATUS uvm_va_space_initialize(uvm_va_space_t *va_space, NvU64 flags)
{
    NV_STATUS status = NV_OK;

    if (flags & ~UVM_INIT_FLAGS_MASK)
        return NV_ERR_INVALID_ARGUMENT;

    uvm_down_write_mmap_sem(&current->mm->mmap_sem);
    uvm_va_space_down_write(va_space);

    if (atomic_read(&va_space->initialized)) {
        // Already initialized - check if parameters match
        if (flags != va_space->initialization_flags)
            status = NV_ERR_INVALID_ARGUMENT;
    }
    else {
        va_space->initialization_flags = flags;

        status = uvm_va_space_mm_register(va_space);
        if (status != NV_OK)
            goto out;

        status = uvm_hmm_mirror_register(va_space);
        if (status != NV_OK) {
            uvm_va_space_mm_unregister(va_space);
            goto out;
        }

        // Use release semantics to match the acquire semantics in
        // uvm_va_space_initialized. See that function for details. All
        // initialization must be complete by this point.
        atomic_set_release(&va_space->initialized, 1);
    }

out:
    uvm_va_space_up_write(va_space);
    uvm_up_write_mmap_sem(&current->mm->mmap_sem);

    return status;
}

void uvm_va_space_stop_all_user_channels(uvm_va_space_t *va_space)
{
    uvm_gpu_va_space_t *gpu_va_space;
    uvm_user_channel_t *user_channel;

    // Skip if all channels have been already stopped.
    if (atomic_read(&va_space->user_channels_stopped))
        return;

    uvm_assert_rwsem_locked_read(&va_space->lock);

    for_each_gpu_va_space(gpu_va_space, va_space) {
        list_for_each_entry(user_channel, &gpu_va_space->registered_channels, list_node)
            uvm_user_channel_stop(user_channel);
    }

    // Since we're holding the VA space lock in read mode, multiple threads
    // could set this concurrently. user_channels_stopped never transitions back
    // to 0 after being set to 1 so that's not a problem.
    atomic_set(&va_space->user_channels_stopped, 1);
}

uvm_gpu_t *uvm_va_space_get_gpu_by_uuid(uvm_va_space_t *va_space, const NvProcessorUuid *gpu_uuid)
{
    uvm_gpu_t *gpu;

    for_each_va_space_gpu(gpu, va_space) {
        if (uvm_processor_uuid_eq(&gpu->uuid, gpu_uuid))
            return gpu;
    }

    return NULL;
}

uvm_gpu_t *uvm_va_space_get_gpu_by_uuid_with_gpu_va_space(uvm_va_space_t *va_space,
                                                          const NvProcessorUuid *gpu_uuid)
{
    uvm_gpu_t *gpu;

    gpu = uvm_va_space_get_gpu_by_uuid(va_space, gpu_uuid);
    if (!gpu || !uvm_processor_mask_test(&va_space->registered_gpu_va_spaces, gpu->id))
        return NULL;

    return gpu;
}

uvm_gpu_t *uvm_va_space_retain_gpu_by_uuid(uvm_va_space_t *va_space, const NvProcessorUuid *gpu_uuid)
{
    uvm_gpu_t *gpu;

    uvm_va_space_down_read(va_space);

    gpu = uvm_va_space_get_gpu_by_uuid(va_space, gpu_uuid);
    if (gpu)
        uvm_gpu_retain(gpu);

    uvm_va_space_up_read(va_space);

    return gpu;
}

bool uvm_va_space_can_read_duplicate(uvm_va_space_t *va_space, uvm_gpu_t *changing_gpu)
{
    uvm_processor_mask_t changing_gpu_mask;
    uvm_processor_mask_t non_faultable_gpus;
    uvm_processor_mask_t registered_gpu_va_spaces;

    uvm_processor_mask_zero(&changing_gpu_mask);

    if (changing_gpu)
        uvm_processor_mask_set(&changing_gpu_mask, changing_gpu->id);

    // flip the bit of the changing GPU to represent the state change in progress
    uvm_processor_mask_xor(&registered_gpu_va_spaces, &changing_gpu_mask, &va_space->registered_gpu_va_spaces);

    // Can't enable read-duplication if any non-fault-capable GPUs have GPU VA spaces registered
    return !uvm_processor_mask_andnot(&non_faultable_gpus, &registered_gpu_va_spaces, &va_space->faultable_processors);
}

// Note that the "VA space" in the function name refers to a UVM per-process VA space.
// (This is different from a per-GPU VA space.)
NV_STATUS uvm_va_space_register_gpu(uvm_va_space_t *va_space,
                                    const NvProcessorUuid *gpu_uuid,
                                    const uvm_rm_user_object_t *user_rm_device,
                                    NvBool *numa_enabled,
                                    NvS32 *numa_node_id)
{
    NV_STATUS status;
    uvm_gpu_t *gpu;
    uvm_gpu_t *other_gpu;

    status = uvm_gpu_retain_by_uuid(gpu_uuid, user_rm_device, &gpu);
    if (status != NV_OK)
        return status;

    // Enabling access counters requires taking the ISR lock, so it is done
    // without holding the (deeper order) VA space lock. Enabling the counters
    // after dropping the VA space lock would create a window of time in which
    // another thread could see the GPU as registered, but access counters would
    // be disabled. Therefore, the counters are enabled before taking the VA
    // space lock.
    if (uvm_gpu_access_counters_required(gpu)) {
        status = uvm_gpu_access_counters_enable(gpu, va_space);
        if (status != NV_OK) {
            uvm_gpu_release(gpu);
            return status;
        }
    }

    uvm_va_space_down_write(va_space);

    // Make sure the gpu hasn't been already registered in this va space
    if (uvm_processor_mask_test(&va_space->registered_gpus, gpu->id)) {
        status = NV_ERR_INVALID_DEVICE;
        goto done;
    }

    // Mixing Volta and Pascal GPUs is not supported on P9 systems.
    for_each_va_space_gpu(other_gpu, va_space) {
        if ((gpu->sysmem_link >= UVM_GPU_LINK_NVLINK_2 && other_gpu->sysmem_link <  UVM_GPU_LINK_NVLINK_2) ||
            (gpu->sysmem_link <  UVM_GPU_LINK_NVLINK_2 && other_gpu->sysmem_link >= UVM_GPU_LINK_NVLINK_2)) {
            status = NV_ERR_INVALID_DEVICE;
            goto done;
        }
    }

    // The VA space's mm is being torn down, so don't allow more work
    if (va_space->disallow_new_registers) {
        status = NV_ERR_PAGE_TABLE_NOT_AVAIL;
        goto done;
    }

    uvm_processor_mask_set(&va_space->registered_gpus, gpu->id);
    va_space->registered_gpus_table[uvm_id_gpu_index(gpu->id)] = gpu;

    if (gpu->isr.replayable_faults.handling) {
        uvm_processor_mask_set(&va_space->faultable_processors, gpu->id);
        // System-wide atomics are enabled by default
        uvm_processor_mask_set(&va_space->system_wide_atomics_enabled_processors, gpu->id);
    }

    // All GPUs have native atomics on their own memory
    processor_mask_array_set(va_space->has_native_atomics, gpu->id, gpu->id);

    if (gpu->sysmem_link >= UVM_GPU_LINK_NVLINK_1) {
        processor_mask_array_set(va_space->has_nvlink, gpu->id, UVM_ID_CPU);
        processor_mask_array_set(va_space->has_nvlink, UVM_ID_CPU, gpu->id);
    }

    if (gpu->sysmem_link >= UVM_GPU_LINK_NVLINK_2) {
        processor_mask_array_set(va_space->has_native_atomics, gpu->id, UVM_ID_CPU);

        if (gpu->numa_info.enabled) {
            processor_mask_array_set(va_space->can_access, UVM_ID_CPU, gpu->id);
            processor_mask_array_set(va_space->accessible_from, gpu->id, UVM_ID_CPU);
            processor_mask_array_set(va_space->has_native_atomics, UVM_ID_CPU, gpu->id);
        }
    }

    // All processors have direct access to their own memory
    processor_mask_array_set(va_space->can_access, gpu->id, gpu->id);
    processor_mask_array_set(va_space->accessible_from, gpu->id, gpu->id);

    // All GPUs have direct access to sysmem
    processor_mask_array_set(va_space->can_access, gpu->id, UVM_ID_CPU);
    processor_mask_array_set(va_space->accessible_from, UVM_ID_CPU, gpu->id);

    processor_mask_array_set(va_space->can_copy_from, gpu->id, gpu->id);
    processor_mask_array_set(va_space->can_copy_from, gpu->id, UVM_ID_CPU);
    processor_mask_array_set(va_space->can_copy_from, UVM_ID_CPU, gpu->id);

    // Update the CPU/GPU affinity masks
    if (gpu->closest_cpu_numa_node != -1) {
        uvm_gpu_id_t gpu_id;

        for_each_gpu_id(gpu_id) {
            uvm_cpu_gpu_affinity_t *affinity = &va_space->gpu_cpu_numa_affinity[uvm_id_gpu_index(gpu_id)];

            // If this is the first time this node is seen, take a new entry of
            // the array. Entries are never released in order to avoid having
            // to deal with holes.
            if (affinity->numa_node == -1) {
                UVM_ASSERT(uvm_processor_mask_empty(&affinity->gpus));
                affinity->numa_node = gpu->closest_cpu_numa_node;
            }

            if (affinity->numa_node == gpu->closest_cpu_numa_node) {
                uvm_processor_mask_set(&affinity->gpus, gpu->id);
                break;
            }
        }
    }

    status = register_gpu_nvlink_peers(va_space, gpu);
    if (status == NV_OK)
        status = uvm_perf_heuristics_register_gpu(va_space, gpu);

    if (status != NV_OK) {
        // Clear out all of the processor mask bits. No VA ranges have mapped or
        // allocated anything on this GPU yet if we fail here, so we don't need
        // a deferred_free_list.
        unregister_gpu(va_space, gpu, NULL, NULL);
    }
    else if (gpu->numa_info.enabled) {
        *numa_enabled = NV_TRUE;
        *numa_node_id = (NvS32)gpu->numa_info.node_id;
    }
    else {
        *numa_enabled = NV_FALSE;
        *numa_node_id = -1;
    }

done:
    uvm_va_space_up_write(va_space);

    if (status != NV_OK) {
        // There is no risk of disabling access counters on a previously
        // registered GPU: the enablement step would have failed before even
        // discovering that the GPU is already registed.
        if (uvm_gpu_access_counters_required(gpu))
            uvm_gpu_access_counters_disable(gpu, va_space);

        uvm_gpu_release(gpu);
    }

    return status;
}

NV_STATUS uvm_va_space_unregister_gpu(uvm_va_space_t *va_space, const NvProcessorUuid *gpu_uuid)
{
    uvm_gpu_t *gpu;
    uvm_gpu_va_space_t *gpu_va_space;
    uvm_global_gpu_id_t peer_gpu_id;
    uvm_global_processor_mask_t peers_to_release;
    LIST_HEAD(deferred_free_list);

    // Stopping channels requires holding the VA space lock in read mode, so do
    // it first. We start in write mode then drop to read in order to flush out
    // other threads which are in the read-mode portion of any of the register
    // or unregister operations.
    uvm_va_space_down_write(va_space);

    gpu = uvm_va_space_get_gpu_by_uuid(va_space, gpu_uuid);
    if (!gpu) {
        uvm_va_space_up_write(va_space);
        return NV_ERR_INVALID_DEVICE;
    }

    // We have to drop the VA space lock below mid-unregister. We have to
    // prevent any other threads from coming in during that window and allowing
    // new channels to enter the GPU. That means we must disallow:
    // - GPU VA space register
    // - GPU unregister (which would allow new GPU registers)
    if (uvm_processor_mask_test(&va_space->gpu_unregister_in_progress, gpu->id)) {
        uvm_va_space_up_write(va_space);
        return NV_ERR_INVALID_DEVICE;
    }

    uvm_processor_mask_set(&va_space->gpu_unregister_in_progress, gpu->id);

    uvm_va_space_downgrade_write_rm(va_space);

    gpu_va_space = uvm_gpu_va_space_get(va_space, gpu);
    if (gpu_va_space)
        gpu_va_space_stop_all_channels(gpu_va_space);

    // We need to drop the lock to re-take it in write mode. We don't have to
    // retain the GPU because we've prevented other threads from unregistering
    // it from the VA space until we're done.
    uvm_va_space_up_read_rm(va_space);

    // If uvm_gpu_access_counters_required(gpu) is true, a concurrent
    // registration could enable access counters after they are disabled here.
    // The concurrent registration will fail later on if it acquires the VA
    // space lock before the unregistration does (because the GPU is still
    // registered) and undo the access counters enablement, or succeed if it
    // acquires the VA space lock after the unregistration does. Both outcomes
    // result on valid states.
    if (gpu->access_counters_supported)
        uvm_gpu_access_counters_disable(gpu, va_space);

    // The mmap_sem lock is needed to establish CPU mappings to any pages
    // evicted from the GPU if accessed by CPU is set for them.
    uvm_down_read_mmap_sem(&current->mm->mmap_sem);

    uvm_va_space_down_write(va_space);

    // We blocked out other GPU unregisters, so this GPU must still be
    // registered. However, the GPU VA space might have been unregistered on us.
    UVM_ASSERT(uvm_processor_mask_test(&va_space->registered_gpus, gpu->id));
    if (uvm_processor_mask_test(&va_space->registered_gpu_va_spaces, gpu->id))
        UVM_ASSERT(uvm_gpu_va_space_get(va_space, gpu) == gpu_va_space);

    // This will call disable_peers for all GPU's peers, including NVLink
    unregister_gpu(va_space, gpu, &deferred_free_list, &peers_to_release);

    UVM_ASSERT(uvm_processor_mask_test(&va_space->gpu_unregister_in_progress, gpu->id));
    uvm_processor_mask_clear(&va_space->gpu_unregister_in_progress, gpu->id);

    uvm_va_space_up_write(va_space);
    uvm_up_read_mmap_sem(&current->mm->mmap_sem);

    uvm_deferred_free_object_list(&deferred_free_list);

    // Release the VA space's GPU and peer counts
    uvm_mutex_lock(&g_uvm_global.global_lock);

    // Do not use for_each_global_gpu_in_mask as it reads the peer GPU state,
    // which might get destroyed when we release the peer entry.
    for_each_global_gpu_id_in_mask(peer_gpu_id, &peers_to_release) {
        uvm_gpu_t *peer_gpu = uvm_gpu_get(peer_gpu_id);
        UVM_ASSERT(uvm_gpu_peer_caps(gpu, peer_gpu)->link_type == UVM_GPU_LINK_PCIE);
        uvm_gpu_release_pcie_peer_access(gpu, peer_gpu);
    }

    uvm_gpu_release_locked(gpu);

    uvm_mutex_unlock(&g_uvm_global.global_lock);

    return NV_OK;
}

// This does *not* release the global GPU peer entry
static void disable_peers(uvm_va_space_t *va_space,
                          uvm_gpu_t *gpu0,
                          uvm_gpu_t *gpu1,
                          struct list_head *deferred_free_list)
{
    NvU32 table_index = uvm_gpu_peer_table_index(gpu0->id, gpu1->id);
    uvm_va_range_t *va_range;

    if (!test_bit(table_index, va_space->enabled_peers))
        return;

    // Unmap all page tables in this VA space which have peer mappings between
    // these two GPUs.
    uvm_for_each_va_range(va_range, va_space)
        uvm_va_range_disable_peer(va_range, gpu0, gpu1, deferred_free_list);

    processor_mask_array_clear(va_space->can_access, gpu0->id, gpu1->id);
    processor_mask_array_clear(va_space->can_access, gpu1->id, gpu0->id);
    processor_mask_array_clear(va_space->accessible_from, gpu0->id, gpu1->id);
    processor_mask_array_clear(va_space->accessible_from, gpu1->id, gpu0->id);
    processor_mask_array_clear(va_space->can_copy_from, gpu0->id, gpu1->id);
    processor_mask_array_clear(va_space->can_copy_from, gpu1->id, gpu0->id);
    processor_mask_array_clear(va_space->has_nvlink, gpu0->id, gpu1->id);
    processor_mask_array_clear(va_space->has_nvlink, gpu1->id, gpu0->id);
    processor_mask_array_clear(va_space->indirect_peers, gpu0->id, gpu1->id);
    processor_mask_array_clear(va_space->indirect_peers, gpu1->id, gpu0->id);
    processor_mask_array_clear(va_space->has_native_atomics, gpu0->id, gpu1->id);
    processor_mask_array_clear(va_space->has_native_atomics, gpu1->id, gpu0->id);

    __clear_bit(table_index, va_space->enabled_peers);
}

static NV_STATUS enable_peers(uvm_va_space_t *va_space, uvm_gpu_t *gpu0, uvm_gpu_t *gpu1)
{
    NV_STATUS status = NV_OK;
    uvm_gpu_va_space_t *gpu_va_space0, *gpu_va_space1;
    NvU32 table_index = 0;
    uvm_gpu_peer_t *peer_caps;
    uvm_va_range_t *va_range;
    LIST_HEAD(deferred_free_list);

    uvm_assert_rwsem_locked_write(&va_space->lock);

    // We know the GPUs were retained already, so now verify that they've been
    // registered by this specific VA space.
    if (!uvm_processor_mask_test(&va_space->registered_gpus, gpu0->id) ||
        !uvm_processor_mask_test(&va_space->registered_gpus, gpu1->id)) {
        return NV_ERR_INVALID_DEVICE;
    }

    table_index = uvm_gpu_peer_table_index(gpu0->id, gpu1->id);
    peer_caps = &g_uvm_global.peers[table_index];

    UVM_ASSERT(!test_bit(table_index, va_space->enabled_peers));

    // If both GPUs have registered GPU VA spaces already, their big page sizes
    // must match.
    gpu_va_space0 = uvm_gpu_va_space_get(va_space, gpu0);
    gpu_va_space1 = uvm_gpu_va_space_get(va_space, gpu1);
    if (gpu_va_space0 &&
        gpu_va_space1 &&
        gpu_va_space0->page_tables.big_page_size != gpu_va_space1->page_tables.big_page_size) {
        return NV_ERR_NOT_COMPATIBLE;
    }

    processor_mask_array_set(va_space->can_access, gpu0->id, gpu1->id);
    processor_mask_array_set(va_space->can_access, gpu1->id, gpu0->id);
    processor_mask_array_set(va_space->accessible_from, gpu0->id, gpu1->id);
    processor_mask_array_set(va_space->accessible_from, gpu1->id, gpu0->id);

    if (gpu0->peer_copy_mode != UVM_GPU_PEER_COPY_MODE_UNSUPPORTED) {
        UVM_ASSERT_MSG(gpu1->peer_copy_mode == gpu0->peer_copy_mode, "GPU %s GPU %s\n", gpu0->name, gpu1->name);

        processor_mask_array_set(va_space->can_copy_from, gpu1->id, gpu0->id);
        processor_mask_array_set(va_space->can_copy_from, gpu0->id, gpu1->id);
    }

    // Pre-compute nvlink and native atomic masks for the new peers
    if (peer_caps->link_type >= UVM_GPU_LINK_NVLINK_1) {
        processor_mask_array_set(va_space->has_nvlink, gpu0->id, gpu1->id);
        processor_mask_array_set(va_space->has_nvlink, gpu1->id, gpu0->id);

        processor_mask_array_set(va_space->has_native_atomics, gpu0->id, gpu1->id);
        processor_mask_array_set(va_space->has_native_atomics, gpu1->id, gpu0->id);

        if (peer_caps->is_indirect_peer) {
            UVM_ASSERT(peer_caps->link_type >= UVM_GPU_LINK_NVLINK_2);
            UVM_ASSERT(gpu0->numa_info.enabled);
            UVM_ASSERT(gpu1->numa_info.enabled);

            processor_mask_array_set(va_space->indirect_peers, gpu0->id, gpu1->id);
            processor_mask_array_set(va_space->indirect_peers, gpu1->id, gpu0->id);
        }
    }

    __set_bit(table_index, va_space->enabled_peers);

    uvm_for_each_va_range(va_range, va_space) {
        status = uvm_va_range_enable_peer(va_range, gpu0, gpu1);
        if (status != NV_OK)
            break;
    }

    if (status != NV_OK) {
        disable_peers(va_space, gpu0, gpu1, &deferred_free_list);

        // uvm_va_range_disable_peer adds only external allocations to the list,
        // but uvm_va_range_enable_peer doesn't do anything for them.
        UVM_ASSERT(list_empty(&deferred_free_list));
    }

    return status;
}

// On success the GPUs and the P2P access have been retained, but the caller
// must not assume that the GPUs are still registered in the VA space after the
// call since the VA space lock is dropped.
static NV_STATUS retain_pcie_peers_from_uuids(uvm_va_space_t *va_space,
                                              const NvProcessorUuid *gpu_uuid_1,
                                              const NvProcessorUuid *gpu_uuid_2,
                                              uvm_gpu_t **gpu0,
                                              uvm_gpu_t **gpu1)
{
    NV_STATUS status = NV_OK;

    uvm_va_space_down_read_rm(va_space);

    // The UUIDs should have already been registered
    *gpu0 = uvm_va_space_get_gpu_by_uuid(va_space, gpu_uuid_1);
    *gpu1 = uvm_va_space_get_gpu_by_uuid(va_space, gpu_uuid_2);

    if (*gpu0 && *gpu1 && !uvm_id_equal((*gpu0)->id, (*gpu1)->id))
        status = uvm_gpu_retain_pcie_peer_access(*gpu0, *gpu1);
    else
        status = NV_ERR_INVALID_DEVICE;

    uvm_va_space_up_read_rm(va_space);

    return status;
}

static bool uvm_va_space_pcie_peer_enabled(uvm_va_space_t *va_space, uvm_gpu_t *gpu0, uvm_gpu_t *gpu1)
{
    return !processor_mask_array_test(va_space->has_nvlink, gpu0->id, gpu1->id) &&
           uvm_va_space_peer_enabled(va_space, gpu0, gpu1);
}

static bool uvm_va_space_nvlink_peer_enabled(uvm_va_space_t *va_space, uvm_gpu_t *gpu0, uvm_gpu_t *gpu1)
{
    return processor_mask_array_test(va_space->has_nvlink, gpu0->id, gpu1->id);
}

static void free_gpu_va_space(nv_kref_t *nv_kref)
{
    uvm_gpu_va_space_t *gpu_va_space = container_of(nv_kref, uvm_gpu_va_space_t, kref);
    uvm_gpu_va_space_state_t state = uvm_gpu_va_space_state(gpu_va_space);
    UVM_ASSERT(state == UVM_GPU_VA_SPACE_STATE_INIT || state == UVM_GPU_VA_SPACE_STATE_DEAD);
    uvm_kvfree(gpu_va_space);
}

void uvm_gpu_va_space_release(uvm_gpu_va_space_t *gpu_va_space)
{
    if (gpu_va_space)
        nv_kref_put(&gpu_va_space->kref, free_gpu_va_space);
}

void uvm_gpu_va_space_unset_page_dir(uvm_gpu_va_space_t *gpu_va_space)
{
    if (gpu_va_space->va_space)
        uvm_assert_rwsem_locked_read(&gpu_va_space->va_space->lock);

    if (gpu_va_space->did_set_page_directory) {
        NV_STATUS status = uvm_rm_locked_call(nvUvmInterfaceUnsetPageDirectory(gpu_va_space->duped_gpu_va_space));
        UVM_ASSERT_MSG(status == NV_OK,
                       "nvUvmInterfaceUnsetPageDirectory() failed: %s, GPU %s\n",
                       nvstatusToString(status),
                       gpu_va_space->gpu->name);
        gpu_va_space->did_set_page_directory = false;
    }
}

static void destroy_gpu_va_space(uvm_gpu_va_space_t *gpu_va_space)
{
    uvm_va_space_t *va_space;
    uvm_gpu_va_space_state_t state;

    if (!gpu_va_space)
        return;

    state = uvm_gpu_va_space_state(gpu_va_space);
    UVM_ASSERT(state == UVM_GPU_VA_SPACE_STATE_INIT || state == UVM_GPU_VA_SPACE_STATE_DEAD);
    va_space = gpu_va_space->va_space;

    // Serialize this uvm_gpu_va_space_unset_page_dir call with the one in
    // uvm_va_space_mm_shutdown, which also starts with the VA space lock in
    // write mode. RM will serialize the calls internally, so we lock here only
    // to avoid getting benign errors from nvUvmInterfaceUnsetPageDirectory.
    //
    // It is possible that there is no va_space yet did_set_page_directory is
    // set. This can happen if create_gpu_va_space succeeded but
    // add_gpu_va_space failed (or we never got to add_gpu_va_space). In those
    // cases, the gpu_va_space was never registered within the va_space, so
    // uvm_va_space_mm_shutdown couldn't see it and we don't have to take the
    // lock.
    if (va_space) {
        uvm_va_space_down_write(va_space);
        uvm_va_space_downgrade_write_rm(va_space);
    }

    uvm_gpu_va_space_unset_page_dir(gpu_va_space);

    if (va_space)
        uvm_va_space_up_read_rm(va_space);

    if (gpu_va_space->page_tables.root)
        uvm_page_tree_deinit(&gpu_va_space->page_tables);

    if (gpu_va_space->duped_gpu_va_space)
        uvm_rm_locked_call_void(nvUvmInterfaceAddressSpaceDestroy(gpu_va_space->duped_gpu_va_space));

    // If the state is DEAD, then this GPU VA space is tracked in
    // va_space->gpu_va_space_deferred_free. uvm_ats_ibm_unregister_gpu_va_space
    // may wait for this count to go to 0 via uvm_va_space_mm_shutdown, so we
    // must decrement it before calling that function.
    if (gpu_va_space->state == UVM_GPU_VA_SPACE_STATE_DEAD) {
        int num_pending = atomic_dec_return(&va_space->gpu_va_space_deferred_free.num_pending);
        UVM_ASSERT(va_space);
        if (num_pending == 0)
            wake_up_all(&va_space->gpu_va_space_deferred_free.wait_queue);
        else
            UVM_ASSERT(num_pending > 0);
    }

    // Note that this call may wait for faults to finish being serviced, which
    // means it may depend on the VA space lock and mmap_sem.
    uvm_ats_ibm_unregister_gpu_va_space(gpu_va_space);

    uvm_gpu_va_space_release(gpu_va_space);
}

static NV_STATUS create_gpu_va_space(uvm_gpu_t *gpu,
                                     uvm_rm_user_object_t *user_rm_va_space,
                                     uvm_gpu_va_space_t **out_gpu_va_space)
{
    NV_STATUS status;
    uvm_gpu_va_space_t *gpu_va_space;
    uvm_gpu_phys_address_t pdb_phys;
    NvU64 num_pdes;
    UvmGpuAddressSpaceInfo gpu_address_space_info;

    *out_gpu_va_space = NULL;

    gpu_va_space = uvm_kvmalloc_zero(sizeof(*gpu_va_space));
    if (!gpu_va_space)
        return NV_ERR_NO_MEMORY;

    gpu_va_space->gpu = gpu;
    INIT_LIST_HEAD(&gpu_va_space->registered_channels);
    INIT_LIST_HEAD(&gpu_va_space->channel_va_ranges);
    nv_kref_init(&gpu_va_space->kref);

    // TODO: Bug 1624521: This interface needs to use rm_control_fd to do
    //       validation.
    (void)user_rm_va_space->rm_control_fd;
    status = uvm_rm_locked_call(nvUvmInterfaceDupAddressSpace(gpu->rm_device,
                                                              user_rm_va_space->user_client,
                                                              user_rm_va_space->user_object,
                                                              &gpu_va_space->duped_gpu_va_space,
                                                              &gpu_address_space_info));
    if (status != NV_OK) {
        UVM_DBG_PRINT("failed to dup address space with error: %s, for GPU:%s \n",
                nvstatusToString(status), gpu->name);
        goto error;
    }

    gpu_va_space->ats.enabled = gpu_address_space_info.atsEnabled;

    // If ATS support in the UVM driver isn't enabled, fail registration of GPU
    // VA spaces which have ATS enabled.
    if (!g_uvm_global.ats.enabled && gpu_va_space->ats.enabled) {
        UVM_INFO_PRINT("GPU VA space requires ATS, but ATS is not supported or enabled\n");
        status = NV_ERR_INVALID_FLAGS;
        goto error;
    }

    // RM allows the creation of VA spaces on Pascal with 128k big pages. We
    // don't support that, so just fail those attempts.
    //
    // TODO: Bug 1789555: Remove this check once RM disallows this case.
    if (!gpu->arch_hal->mmu_mode_hal(gpu_address_space_info.bigPageSize)) {
        status = NV_ERR_INVALID_FLAGS;
        goto error;
    }

    // Set up this GPU's page tables
    UVM_ASSERT(gpu_va_space->page_tables.root == NULL);
    status = uvm_page_tree_init(gpu,
                                UVM_PAGE_TREE_TYPE_USER,
                                gpu_address_space_info.bigPageSize,
                                UVM_APERTURE_DEFAULT,
                                &gpu_va_space->page_tables);
    if (status != NV_OK) {
        UVM_ERR_PRINT("Initializing the page tree failed: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        goto error;
    }

    // Replace the existing PDB, if present, with the new one allocated by UVM.
    // This will fail if nvUvmInterfaceSetPageDirectory has already been called
    // on the RM VA space object, which prevents the user from registering twice
    // and corrupting our state.
    //
    // TODO: Bug 1733664: RM needs to preempt and disable channels during this
    //       operation.
    pdb_phys = uvm_page_tree_pdb(&gpu_va_space->page_tables)->addr;
    num_pdes = uvm_mmu_page_tree_entries(&gpu_va_space->page_tables, 0, UVM_PAGE_SIZE_AGNOSTIC);
    status = uvm_rm_locked_call(nvUvmInterfaceSetPageDirectory(gpu_va_space->duped_gpu_va_space,
                                                               pdb_phys.address,
                                                               num_pdes,
                                                               pdb_phys.aperture == UVM_APERTURE_VID));
    if (status != NV_OK) {
        UVM_DBG_PRINT("nvUvmInterfaceSetPageDirectory() failed: %s, GPU %s\n", nvstatusToString(status), gpu->name);

        // Convert to the return code specified by uvm.h for already-registered
        // PDBs.
        if (status == NV_ERR_NOT_SUPPORTED)
            status = NV_ERR_INVALID_DEVICE;

        goto error;
    }

    gpu_va_space->did_set_page_directory = true;

    *out_gpu_va_space = gpu_va_space;
    return NV_OK;

error:
    destroy_gpu_va_space(gpu_va_space);
    return status;
}

static NV_STATUS add_gpu_va_space(uvm_va_space_t *va_space, uvm_gpu_va_space_t *gpu_va_space)
{
    uvm_gpu_t *gpu = gpu_va_space->gpu;
    uvm_gpu_t *other_gpu;
    uvm_gpu_va_space_t *other_gpu_va_space;

    uvm_assert_rwsem_locked_write(&va_space->lock);

    // If this GPU VA space uses ATS then pageable memory access must not have
    // been disabled in the VA space.
    if (gpu_va_space->ats.enabled && !uvm_va_space_pageable_mem_access_supported(va_space))
        return NV_ERR_INVALID_FLAGS;

    // This GPU VA space must match its big page size with all enabled peers.
    // Also, the new GPU VA space must have the same ATS setting as previously-
    // registered GPU VA spaces
    for_each_va_space_gpu_in_mask(other_gpu, va_space, &va_space->registered_gpu_va_spaces) {
        UVM_ASSERT(other_gpu != gpu);

        other_gpu_va_space = uvm_gpu_va_space_get(va_space, other_gpu);
        if (other_gpu_va_space->ats.enabled != gpu_va_space->ats.enabled)
            return NV_ERR_INVALID_FLAGS;

        if (!test_bit(uvm_gpu_peer_table_index(gpu->id, other_gpu->id), va_space->enabled_peers))
            continue;

        if (gpu_va_space->page_tables.big_page_size != other_gpu_va_space->page_tables.big_page_size)
            return NV_ERR_NOT_COMPATIBLE;
    }

    uvm_processor_mask_set(&va_space->registered_gpu_va_spaces, gpu->id);
    va_space->gpu_va_spaces[uvm_id_gpu_index(gpu->id)] = gpu_va_space;
    gpu_va_space->va_space = va_space;
    gpu_va_space->state = UVM_GPU_VA_SPACE_STATE_ACTIVE;

    return NV_OK;
}

NV_STATUS uvm_va_space_register_gpu_va_space(uvm_va_space_t *va_space,
                                             uvm_rm_user_object_t *user_rm_va_space,
                                             const NvProcessorUuid *gpu_uuid)
{
    NV_STATUS status;
    uvm_gpu_t *gpu;
    uvm_gpu_va_space_t *gpu_va_space;
    uvm_va_range_t *va_range;
    LIST_HEAD(deferred_free_list);

    gpu = uvm_va_space_retain_gpu_by_uuid(va_space, gpu_uuid);
    if (!gpu)
        return NV_ERR_INVALID_DEVICE;

    status = create_gpu_va_space(gpu, user_rm_va_space, &gpu_va_space);
    if (status != NV_OK) {
        uvm_gpu_release(gpu);
        return status;
    }

    // uvm_ats_ibm_register_gpu_va_space() requires mmap_sem to be held in write
    // mode if ATS support is provided through the kernel. Otherwise we only
    // need mmap_sem in read mode to handle potential CPU mapping changes in
    // uvm_va_range_add_gpu_va_space().
    if (UVM_ATS_IBM_SUPPORTED_IN_KERNEL())
        uvm_down_write_mmap_sem(&current->mm->mmap_sem);
    else
        uvm_down_read_mmap_sem(&current->mm->mmap_sem);

    uvm_va_space_down_write(va_space);

    if (!uvm_processor_mask_test(&va_space->registered_gpus, gpu->id)) {
        status = NV_ERR_INVALID_DEVICE;
        goto error;
    }

    // RM will return an error from create_gpu_va_space if the given RM VA space
    // object has already been registered by any VA space. Now we just need to
    // check if a different VA space has already been registered.
    if (uvm_processor_mask_test(&va_space->registered_gpu_va_spaces, gpu->id)) {
        status = NV_ERR_INVALID_DEVICE;
        goto error;
    }

    // If a GPU unregister is in progress but temporarily dropped the VA space
    // lock, we can't register new GPU VA spaces.
    if (uvm_processor_mask_test(&va_space->gpu_unregister_in_progress, gpu->id)) {
        status = NV_ERR_INVALID_DEVICE;
        goto error;
    }

    // The VA space's mm is being torn down, so don't allow more work
    if (va_space->disallow_new_registers) {
        status = NV_ERR_PAGE_TABLE_NOT_AVAIL;
        goto error;
    }

    status = add_gpu_va_space(va_space, gpu_va_space);
    if (status != NV_OK)
        goto error;

    // This call needs to happen after add_gpu_va_space() since invalidation
    // callbacks might be triggered by the calls below before we drop the VA
    // space lock, and we want those to see the gpu_va_space fully set up.
    status = uvm_ats_ibm_register_gpu_va_space(gpu_va_space);
    if (status != NV_OK)
        goto error;

    // Tell the VA ranges that they can map this GPU, if they need to.
    //
    // Ideally we'd downgrade the VA space lock to read mode while adding new
    // mappings, but that would complicate error handling since we have to
    // remove the GPU VA space if any of these mappings fail.
    uvm_for_each_va_range(va_range, va_space) {
        status = uvm_va_range_add_gpu_va_space(va_range, gpu_va_space);
        if (status != NV_OK)
            goto error;
    }

    uvm_va_space_up_write(va_space);

    if (UVM_ATS_IBM_SUPPORTED_IN_KERNEL())
        uvm_up_write_mmap_sem(&current->mm->mmap_sem);
    else
        uvm_up_read_mmap_sem(&current->mm->mmap_sem);

    uvm_gpu_release(gpu);
    return NV_OK;

error:
    if (gpu_va_space->va_space) {
        remove_gpu_va_space(gpu_va_space, &deferred_free_list);

        // Nothing else could've been attached to this gpu_va_space (channels,
        // external allocations) since we're still holding the VA space lock.
        // Therefore the GPU VA space itself should be the only item in the
        // list, and we can just destroy it directly below.
        UVM_ASSERT(list_is_singular(&deferred_free_list));
    }

    uvm_va_space_up_write(va_space);

    if (UVM_ATS_IBM_SUPPORTED_IN_KERNEL())
        uvm_up_write_mmap_sem(&current->mm->mmap_sem);
    else
        uvm_up_read_mmap_sem(&current->mm->mmap_sem);

    destroy_gpu_va_space(gpu_va_space);

    uvm_gpu_release(gpu);
    return status;
}

// The caller must have stopped all channels under this gpu_va_space before
// calling this function.
static void remove_gpu_va_space(uvm_gpu_va_space_t *gpu_va_space, struct list_head *deferred_free_list)
{
    uvm_va_space_t *va_space;
    uvm_va_range_t *va_range;

    if (!gpu_va_space || uvm_gpu_va_space_state(gpu_va_space) != UVM_GPU_VA_SPACE_STATE_ACTIVE)
        return;

    va_space = gpu_va_space->va_space;
    uvm_assert_rwsem_locked_write(&va_space->lock);

    uvm_gpu_va_space_detach_all_user_channels(gpu_va_space, deferred_free_list);

    // Removing all registered channels should've removed all VA ranges used by
    // those channels.
    UVM_ASSERT(list_empty(&gpu_va_space->channel_va_ranges));

    // Unmap all page tables in this VA space on this GPU.
    // TODO: Bug 1799173: This will need to add objects to deferred_free_list
    uvm_for_each_va_range(va_range, va_space)
        uvm_va_range_remove_gpu_va_space(va_range, gpu_va_space, deferred_free_list);

    uvm_deferred_free_object_add(deferred_free_list,
                                 &gpu_va_space->deferred_free,
                                 UVM_DEFERRED_FREE_OBJECT_GPU_VA_SPACE);

    // Let uvm_va_space_mm_shutdown know that it has to wait for this GPU VA
    // space to be destroyed.
    atomic_inc(&va_space->gpu_va_space_deferred_free.num_pending);

    uvm_processor_mask_clear(&va_space->registered_gpu_va_spaces, gpu_va_space->gpu->id);
    va_space->gpu_va_spaces[uvm_id_gpu_index(gpu_va_space->gpu->id)] = NULL;
    gpu_va_space->state = UVM_GPU_VA_SPACE_STATE_DEAD;
}

NV_STATUS uvm_va_space_unregister_gpu_va_space(uvm_va_space_t *va_space, const NvProcessorUuid *gpu_uuid)
{
    NV_STATUS status = NV_OK;
    uvm_gpu_t *gpu;
    uvm_gpu_va_space_t *gpu_va_space;
    LIST_HEAD(deferred_free_list);

    // Stopping channels requires holding the VA space lock in read mode, so do
    // it first. This also takes the serialize_writers_lock, so we'll serialize
    // with other threads about to perform channel binds in
    // uvm_register_channel since.
    uvm_va_space_down_read_rm(va_space);

    gpu = uvm_va_space_get_gpu_by_uuid_with_gpu_va_space(va_space, gpu_uuid);
    if (!gpu) {
        uvm_va_space_up_read_rm(va_space);
        return NV_ERR_INVALID_DEVICE;
    }

    gpu_va_space = uvm_gpu_va_space_get(va_space, gpu);
    UVM_ASSERT(gpu_va_space);

    gpu_va_space_stop_all_channels(gpu_va_space);

    // We need to drop the lock to re-take it in write mode
    uvm_gpu_va_space_retain(gpu_va_space);
    uvm_gpu_retain(gpu);
    uvm_va_space_up_read_rm(va_space);

    uvm_down_read_mmap_sem(&current->mm->mmap_sem);
    uvm_va_space_down_write(va_space);

    // We dropped the lock so we have to re-verify that this gpu_va_space is
    // still valid. If so, then the GPU is also still registered under the VA
    // space. If not, we raced with another unregister thread, so return an
    // an error for double-unregister.
    if (uvm_gpu_va_space_state(gpu_va_space) == UVM_GPU_VA_SPACE_STATE_DEAD) {
        status = NV_ERR_INVALID_DEVICE;
    }
    else {
        UVM_ASSERT(gpu == uvm_va_space_get_gpu_by_uuid_with_gpu_va_space(va_space, gpu_uuid));
        UVM_ASSERT(gpu_va_space == uvm_gpu_va_space_get(va_space, gpu));

        remove_gpu_va_space(gpu_va_space, &deferred_free_list);
    }

    uvm_va_space_up_write(va_space);
    uvm_up_read_mmap_sem(&current->mm->mmap_sem);

    uvm_deferred_free_object_list(&deferred_free_list);
    uvm_gpu_va_space_release(gpu_va_space);
    uvm_gpu_release(gpu);
    return status;
}

bool uvm_va_space_peer_enabled(uvm_va_space_t *va_space, uvm_gpu_t *gpu1, uvm_gpu_t *gpu2)
{
    size_t table_index;

    UVM_ASSERT(uvm_processor_mask_test(&va_space->registered_gpus, gpu1->id));
    UVM_ASSERT(uvm_processor_mask_test(&va_space->registered_gpus, gpu2->id));

    table_index = uvm_gpu_peer_table_index(gpu1->id, gpu2->id);
    return !!test_bit(table_index, va_space->enabled_peers);
}

uvm_processor_id_t uvm_processor_mask_find_closest_id(uvm_va_space_t *va_space,
                                                      const uvm_processor_mask_t *candidates,
                                                      uvm_processor_id_t src)
{
    uvm_processor_mask_t mask;
    uvm_processor_id_t id;

    // Highest priority: the local processor itself
    if (uvm_processor_mask_test(candidates, src))
        return src;

    // NvLink peers
    if (uvm_processor_mask_and(&mask, candidates, &va_space->has_nvlink[uvm_id_value(src)])) {
        uvm_processor_mask_t *indirect_peers;
        uvm_processor_mask_t direct_peers;

        indirect_peers = &va_space->indirect_peers[uvm_id_value(src)];

        // Direct peers, prioritizing GPU peers over CPU
        if (uvm_processor_mask_andnot(&direct_peers, &mask, indirect_peers)) {
            id = uvm_processor_mask_find_first_gpu_id(&direct_peers);
            return UVM_ID_IS_INVALID(id)? UVM_ID_CPU : id;
        }

        // Indirect peers
        UVM_ASSERT(UVM_ID_IS_GPU(src));
        UVM_ASSERT(!uvm_processor_mask_test(&mask, UVM_ID_CPU));

        return uvm_processor_mask_find_first_gpu_id(&mask);
    }

    // If source is GPU, prioritize PCIe peers over CPU
    if (uvm_processor_mask_and(&mask, candidates, &va_space->can_access[uvm_id_value(src)])) {
        // CPUs only have direct access to GPU memory over NVLINK, not PCIe, and
        // should have been selected above
        UVM_ASSERT(UVM_ID_IS_GPU(src));

        id = uvm_processor_mask_find_first_gpu_id(&mask);
        return UVM_ID_IS_INVALID(id)? UVM_ID_CPU : id;
    }

    // If the CPU would be a candidate, it would have been chosen by now since
    // it is directly accessible from all processors
    UVM_ASSERT(!uvm_processor_mask_test(candidates, UVM_ID_CPU));

    // No GPUs with direct access, or the CPU, are in the mask. Just pick the
    // first GPU in the mask, if any.
    return uvm_processor_mask_find_first_gpu_id(candidates);
}

static void uvm_deferred_free_object_channel(uvm_deferred_free_object_t *object, uvm_processor_mask_t *flushed_gpus)
{
    uvm_user_channel_t *channel = container_of(object, uvm_user_channel_t, deferred_free);
    uvm_gpu_t *gpu = channel->gpu;

    // Flush out any faults with this instance pointer still in the buffer. This
    // prevents us from re-allocating the same instance pointer for a new
    // channel and mis-attributing old faults to it.
    if (gpu->replayable_faults_supported && !uvm_processor_mask_test(flushed_gpus, gpu->id)) {
        uvm_gpu_fault_buffer_flush(gpu);
        uvm_processor_mask_set(flushed_gpus, gpu->id);
    }

    uvm_user_channel_destroy_detached(channel);
}

void uvm_deferred_free_object_list(struct list_head *deferred_free_list)
{
    uvm_deferred_free_object_t *object, *next;
    uvm_processor_mask_t flushed_gpus;

    // Used if there are any channels in the list
    uvm_processor_mask_zero(&flushed_gpus);

    list_for_each_entry_safe(object, next, deferred_free_list, list_node) {
        list_del(&object->list_node);

        switch (object->type) {
            case UVM_DEFERRED_FREE_OBJECT_TYPE_CHANNEL:
                uvm_deferred_free_object_channel(object, &flushed_gpus);
                break;
            case UVM_DEFERRED_FREE_OBJECT_GPU_VA_SPACE:
                destroy_gpu_va_space(container_of(object, uvm_gpu_va_space_t, deferred_free));
                break;
            case UVM_DEFERRED_FREE_OBJECT_TYPE_EXTERNAL_ALLOCATION:
                uvm_ext_gpu_map_free(container_of(object, uvm_ext_gpu_map_t, deferred_free));
                break;
            default:
                UVM_ASSERT_MSG(0, "Invalid type %d\n", object->type);
        }
    }
}

uvm_user_channel_t *uvm_gpu_va_space_get_user_channel(uvm_gpu_va_space_t *gpu_va_space,
                                                      uvm_gpu_phys_address_t instance_ptr)
{
    uvm_user_channel_t *user_channel;
    uvm_va_space_t *va_space = gpu_va_space->va_space;

    UVM_ASSERT(uvm_gpu_va_space_state(gpu_va_space) == UVM_GPU_VA_SPACE_STATE_ACTIVE);
    uvm_assert_rwsem_locked(&va_space->lock);

    // TODO: Bug 1880191: This is called on every non-replayable fault service.
    // Evaluate the performance impact of this list traversal and potentially
    // replace it with something better.
    list_for_each_entry(user_channel, &gpu_va_space->registered_channels, list_node) {
        if (user_channel->instance_ptr.address == instance_ptr.address &&
            user_channel->instance_ptr.aperture == instance_ptr.aperture) {
            return user_channel;
        }
    }

    return NULL;
}

NV_STATUS uvm_api_enable_peer_access(UVM_ENABLE_PEER_ACCESS_PARAMS *params, struct file *filp)
{
    uvm_va_space_t *va_space = uvm_va_space_get(filp);
    NV_STATUS status = NV_OK;
    uvm_gpu_t *gpu0 = NULL;
    uvm_gpu_t *gpu1 = NULL;
    size_t table_index;

    uvm_mutex_lock(&g_uvm_global.global_lock);
    status = retain_pcie_peers_from_uuids(va_space, &params->gpuUuidA, &params->gpuUuidB, &gpu0, &gpu1);
    uvm_mutex_unlock(&g_uvm_global.global_lock);
    if (status != NV_OK)
        return status;

    uvm_va_space_down_write(va_space);

    table_index = uvm_gpu_peer_table_index(gpu0->id, gpu1->id);
    if (test_bit(table_index, va_space->enabled_peers))
        status = NV_ERR_INVALID_DEVICE;
    else
        status = enable_peers(va_space, gpu0, gpu1);

    uvm_va_space_up_write(va_space);

    if (status != NV_OK) {
        uvm_mutex_lock(&g_uvm_global.global_lock);
        uvm_gpu_release_pcie_peer_access(gpu0, gpu1);
        uvm_mutex_unlock(&g_uvm_global.global_lock);
    }

    return status;
}

NV_STATUS uvm_api_disable_peer_access(UVM_DISABLE_PEER_ACCESS_PARAMS *params, struct file *filp)
{
    uvm_va_space_t *va_space = uvm_va_space_get(filp);
    NV_STATUS status = NV_OK;
    uvm_gpu_t *gpu0, *gpu1;
    LIST_HEAD(deferred_free_list);

    uvm_va_space_down_write(va_space);

    gpu0 = uvm_va_space_get_gpu_by_uuid(va_space, &params->gpuUuidA);
    gpu1 = uvm_va_space_get_gpu_by_uuid(va_space, &params->gpuUuidB);

    if (!gpu0 || !gpu1) {
        status = NV_ERR_INVALID_DEVICE;
        goto error;
    }

    if (uvm_id_equal(gpu0->id, gpu1->id)) {
        status = NV_ERR_INVALID_DEVICE;
        goto error;
    }

    if (!uvm_va_space_pcie_peer_enabled(va_space, gpu0, gpu1)) {
        status = NV_ERR_INVALID_DEVICE;
        goto error;
    }

    disable_peers(va_space, gpu0, gpu1, &deferred_free_list);

    // disable_peers doesn't release the GPU peer ref count, which means the two
    // GPUs will remain retained even if another thread unregisters them from
    // this VA space after we drop the lock.
    uvm_va_space_up_write(va_space);

    uvm_deferred_free_object_list(&deferred_free_list);

    uvm_mutex_lock(&g_uvm_global.global_lock);
    uvm_gpu_release_pcie_peer_access(gpu0, gpu1);
    uvm_mutex_unlock(&g_uvm_global.global_lock);

    return NV_OK;

error:
    uvm_va_space_up_write(va_space);
    return status;
}

bool uvm_va_space_pageable_mem_access_supported(uvm_va_space_t *va_space)
{
    UVM_ASSERT(uvm_va_space_initialized(va_space) == NV_OK);

    // Any pageable memory access requires that we have mm_struct association
    // via va_space_mm.
    if (!uvm_va_space_mm_enabled(va_space))
        return false;

    // We might have systems with both ATS and HMM support. ATS gets priority.
    if (g_uvm_global.ats.supported)
        return g_uvm_global.ats.enabled;

    return uvm_hmm_is_enabled(va_space);
}

NV_STATUS uvm8_test_get_pageable_mem_access_type(UVM_TEST_GET_PAGEABLE_MEM_ACCESS_TYPE_PARAMS *params,
                                                 struct file *filp)
{
    uvm_va_space_t *va_space = uvm_va_space_get(filp);

    params->type = UVM_TEST_PAGEABLE_MEM_ACCESS_TYPE_NONE;

    if (uvm_va_space_pageable_mem_access_supported(va_space)) {
        if (g_uvm_global.ats.enabled) {
            if (UVM_ATS_IBM_SUPPORTED_IN_KERNEL())
                params->type = UVM_TEST_PAGEABLE_MEM_ACCESS_TYPE_ATS_KERNEL;
            else
                params->type = UVM_TEST_PAGEABLE_MEM_ACCESS_TYPE_ATS_DRIVER;
        }
        else {
            params->type = UVM_TEST_PAGEABLE_MEM_ACCESS_TYPE_HMM;
        }
    }
    else if (uvm_va_space_mm_enabled(va_space)) {
        params->type = UVM_TEST_PAGEABLE_MEM_ACCESS_TYPE_MMU_NOTIFIER;
    }

    return NV_OK;
}

NV_STATUS uvm8_test_flush_deferred_work(UVM_TEST_FLUSH_DEFERRED_WORK_PARAMS *params, struct file *filp)
{
    UvmTestDeferredWorkType work_type = params->work_type;

    switch (work_type) {
        case UvmTestDeferredWorkTypeAcessedByMappings:
            nv_kthread_q_flush(&g_uvm_global.global_q);
            return NV_OK;
        default:
            return NV_ERR_INVALID_ARGUMENT;
    }
}

NV_STATUS uvm8_test_enable_nvlink_peer_access(UVM_TEST_ENABLE_NVLINK_PEER_ACCESS_PARAMS *params, struct file *filp)
{
    uvm_va_space_t *va_space = uvm_va_space_get(filp);
    NV_STATUS status = NV_OK;
    uvm_gpu_t *gpu0 = NULL;
    uvm_gpu_t *gpu1 = NULL;
    size_t table_index;
    uvm_gpu_peer_t *peer_caps = NULL;

    uvm_va_space_down_write(va_space);

    gpu0 = uvm_va_space_get_gpu_by_uuid(va_space, &params->gpuUuidA);
    gpu1 = uvm_va_space_get_gpu_by_uuid(va_space, &params->gpuUuidB);

    if (gpu0 && gpu1 && !uvm_id_equal(gpu0->id, gpu1->id))
        peer_caps = uvm_gpu_peer_caps(gpu0, gpu1);

    if (!peer_caps || peer_caps->link_type < UVM_GPU_LINK_NVLINK_1) {
        uvm_va_space_up_write(va_space);
        return NV_ERR_INVALID_DEVICE;
    }

    table_index = uvm_gpu_peer_table_index(gpu0->id, gpu1->id);

    // NVLink peers are automatically enabled in the VA space at VA space
    // registration time. In order to avoid tests having to keep track of the
    // different initial state for PCIe and NVLink peers, we just return NV_OK
    // if NVLink peer were already enabled.
    if (test_bit(table_index, va_space->enabled_peers))
        status = NV_OK;
    else
        status = enable_peers(va_space, gpu0, gpu1);

    uvm_va_space_up_write(va_space);

    return status;
}

NV_STATUS uvm8_test_disable_nvlink_peer_access(UVM_TEST_DISABLE_NVLINK_PEER_ACCESS_PARAMS *params, struct file *filp)
{
    uvm_va_space_t *va_space = uvm_va_space_get(filp);
    NV_STATUS status = NV_OK;
    uvm_gpu_t *gpu0, *gpu1;
    LIST_HEAD(deferred_free_list);

    uvm_va_space_down_write(va_space);

    gpu0 = uvm_va_space_get_gpu_by_uuid(va_space, &params->gpuUuidA);
    gpu1 = uvm_va_space_get_gpu_by_uuid(va_space, &params->gpuUuidB);

    if (!gpu0 || !gpu1) {
        status = NV_ERR_INVALID_DEVICE;
        goto error;
    }

    if (uvm_id_equal(gpu0->id, gpu1->id)) {
        status = NV_ERR_INVALID_DEVICE;
        goto error;
    }

    if (!uvm_va_space_nvlink_peer_enabled(va_space, gpu0, gpu1)) {
        status = NV_ERR_INVALID_DEVICE;
        goto error;
    }

    disable_peers(va_space, gpu0, gpu1, &deferred_free_list);

    uvm_va_space_up_write(va_space);

    uvm_deferred_free_object_list(&deferred_free_list);

    return NV_OK;

error:
    uvm_va_space_up_write(va_space);
    return status;
}

NV_STATUS uvm8_test_va_space_inject_error(UVM_TEST_VA_SPACE_INJECT_ERROR_PARAMS *params, struct file *filp)
{
    uvm_va_space_t *va_space = uvm_va_space_get(filp);

    atomic_set(&va_space->test.migrate_vma_allocation_fail_nth, params->migrate_vma_allocation_fail_nth);

    return NV_OK;
}

// Add a fixed number of dummy thread contexts to each thread context table.
// The newly added thread contexts are removed by calling
// uvm8_test_va_space_remove_dummy_thread_contexts, or during VA space shutdown.
NV_STATUS uvm8_test_va_space_add_dummy_thread_contexts(UVM_TEST_VA_SPACE_ADD_DUMMY_THREAD_CONTEXTS_PARAMS *params,
                                                       struct file *filp)
{
    size_t i;
    uvm_va_space_t *va_space;
    size_t total_dummy_thread_contexts = params->num_dummy_thread_contexts * UVM_THREAD_CONTEXT_TABLE_SIZE;
    NV_STATUS status = NV_OK;

    if (params->num_dummy_thread_contexts == 0)
        return NV_OK;

    va_space = uvm_va_space_get(filp);

    uvm_va_space_down_write(va_space);

    if (va_space->test.dummy_thread_context_wrappers != NULL) {
        status = NV_ERR_INVALID_STATE;
        goto out;
    }

    if (va_space->test.num_dummy_thread_context_wrappers > 0) {
        status = NV_ERR_INVALID_STATE;
        goto out;
    }

    if (!uvm_thread_context_wrapper_is_used()) {
        status = NV_ERR_INVALID_STATE;
        goto out;
    }

    va_space->test.dummy_thread_context_wrappers = uvm_kvmalloc(sizeof(*va_space->test.dummy_thread_context_wrappers) *
                                                                total_dummy_thread_contexts);
    if (va_space->test.dummy_thread_context_wrappers == NULL) {
        status = NV_ERR_NO_MEMORY;
        goto out;
    }

    va_space->test.num_dummy_thread_context_wrappers = total_dummy_thread_contexts;

    for (i = 0; i < total_dummy_thread_contexts; i++) {
        uvm_thread_context_t *thread_context = &va_space->test.dummy_thread_context_wrappers[i].context;

        // The context pointer is used to fill the task.
        thread_context->task = (struct task_struct *) thread_context;

        uvm_thread_context_add_at(thread_context, i % UVM_THREAD_CONTEXT_TABLE_SIZE);
    }

out:
    uvm_va_space_up_write(va_space);

    return status;
}

static void va_space_remove_dummy_thread_contexts(uvm_va_space_t *va_space)
{
    size_t i;

    uvm_assert_rwsem_locked_write(&va_space->lock);

    if (va_space->test.dummy_thread_context_wrappers == NULL) {
        UVM_ASSERT(va_space->test.num_dummy_thread_context_wrappers == 0);
        return;
    }

    UVM_ASSERT(uvm_thread_context_wrapper_is_used());
    UVM_ASSERT(uvm_enable_builtin_tests != 0);
    UVM_ASSERT(va_space->test.num_dummy_thread_context_wrappers > 0);

    for (i = 0; i < va_space->test.num_dummy_thread_context_wrappers; i++) {
        uvm_thread_context_t *thread_context = &va_space->test.dummy_thread_context_wrappers[i].context;

        uvm_thread_context_remove_at(thread_context, i % UVM_THREAD_CONTEXT_TABLE_SIZE);
    }

    uvm_kvfree(va_space->test.dummy_thread_context_wrappers);
    va_space->test.dummy_thread_context_wrappers = NULL;
    va_space->test.num_dummy_thread_context_wrappers = 0;
}

NV_STATUS uvm8_test_va_space_remove_dummy_thread_contexts(UVM_TEST_VA_SPACE_REMOVE_DUMMY_THREAD_CONTEXTS_PARAMS *params,
                                                          struct file *filp)
{
    uvm_va_space_t *va_space = uvm_va_space_get(filp);

    uvm_va_space_down_write(va_space);

    va_space_remove_dummy_thread_contexts(va_space);

    uvm_va_space_up_write(va_space);

    return NV_OK;
}
