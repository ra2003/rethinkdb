#include "buffer_cache/mirrored/diff_out_of_core_storage.hpp"
#include "buffer_cache/mirrored/mirrored.hpp"
#include "buffer_cache/buffer_cache.hpp"

// TODO! Add a number of perfmons

diff_oocore_storage_t::diff_oocore_storage_t(mc_cache_t &cache) : cache(cache) {
    first_block = 0;
    number_of_blocks = 0;

    active_log_block = 0;
    next_patch_offset = 0;
}

diff_oocore_storage_t::~diff_oocore_storage_t() {
    rassert(log_block_bufs.size() == 0);
}

void diff_oocore_storage_t::shutdown() {
    for (size_t i = 0; i < log_block_bufs.size(); ++i)
        log_block_bufs[i]->release();
    log_block_bufs.clear();
}

void diff_oocore_storage_t::init(const block_id_t first_block, const block_id_t number_of_blocks) {
    cache.assert_thread();
    this->first_block = first_block;
    this->number_of_blocks = number_of_blocks;
    block_is_empty.resize(number_of_blocks, false);

    if (number_of_blocks == 0)
        return;

    // Load all log blocks into memory
    for (block_id_t current_block = first_block; current_block < first_block + number_of_blocks; ++current_block) {
        coro_t::move_to_thread(cache.serializer->home_thread);
        bool block_in_use = cache.serializer->block_in_use(current_block);
        coro_t::move_to_thread(cache.home_thread);
        if (block_in_use) {
            log_block_bufs.push_back(acquire_block_no_locking(current_block));

            // Check that this is a valid log block
            mc_buf_t *log_buf = log_block_bufs[current_block - first_block];
            void *buf_data = log_buf->get_data_major_write();
            guarantee(strncmp((char*)buf_data, LOG_BLOCK_MAGIC, sizeof(LOG_BLOCK_MAGIC)) == 0);
        } else {
            //fprintf(stderr, "Initializing a new log block with id %d\n", current_block);
            // Initialize a new log block here (we rely on the property block_id assignment properties)
            mc_inner_buf_t *new_ibuf = new mc_inner_buf_t(&cache);
            guarantee(new_ibuf->block_id == current_block);

            log_block_bufs.push_back(acquire_block_no_locking(current_block));

            init_log_block(current_block);
            block_is_empty[current_block - first_block] = true;
        }
    }
    rassert(log_block_bufs.size() == number_of_blocks);

    set_active_log_block(first_block);
}

// Loads on-disk data into memory
void diff_oocore_storage_t::load_patches(diff_core_storage_t &in_core_storage) {
    rassert(log_block_bufs.size() == number_of_blocks);
    cache.assert_thread();
    if (number_of_blocks == 0)
        return;

    std::map<block_id_t, std::list<buf_patch_t*> > patch_map;

    // Scan through all log blocks, build a map block_id -> patch list
    for (block_id_t current_block = first_block; current_block < first_block + number_of_blocks; ++current_block) {
        mc_buf_t *log_buf = log_block_bufs[current_block - first_block];
        const void *buf_data = log_buf->get_data_read();
        guarantee(strncmp((char*)buf_data, LOG_BLOCK_MAGIC, sizeof(LOG_BLOCK_MAGIC)) == 0);
        uint16_t current_offset = sizeof(LOG_BLOCK_MAGIC);
        while (current_offset + buf_patch_t::get_min_serialized_size() < cache.get_block_size().value()) {
            buf_patch_t *patch = buf_patch_t::load_patch((char*)buf_data + current_offset);
            if (!patch) {
                break;
            }
            else {
                current_offset += patch->get_serialized_size();
                // Only store the patch if the corresponding block still exists
                // (otherwise we'd get problems when flushing the log, as deleted blocks would cause an error)
                coro_t::move_to_thread(cache.serializer->home_thread);
                bool block_in_use = cache.serializer->block_in_use(patch->get_block_id());
                coro_t::move_to_thread(cache.home_thread);
                if (block_in_use)
                    patch_map[patch->get_block_id()].push_back(patch);
                else
                    delete patch;
            }
        }
    }

    for (std::map<block_id_t, std::list<buf_patch_t*> >::iterator patch_list = patch_map.begin(); patch_list != patch_map.end(); ++patch_list) {
        // Sort the list to get patches in the right order
        patch_list->second.sort(dereferencing_compare_t<buf_patch_t>());

        // Store list into in_core_storage
        in_core_storage.load_block_patch_list(patch_list->first, patch_list->second);
    }
}

// Returns true on success, false if patch could not be stored (e.g. because of insufficient free space in log)
// This function never blocks and must only be called while the flush_lock is held.
bool diff_oocore_storage_t::store_patch(buf_patch_t &patch) {
    rassert(log_block_bufs.size() == number_of_blocks);
    cache.assert_thread();
    // TODO: assert flush_in_progress somehow?

    if (number_of_blocks == 0)
        return false;

    // Check if we have sufficient free space in the current log block to store the patch
    const size_t patch_serialized_size = patch.get_serialized_size();
    rassert(cache.get_block_size().value() >= (size_t)next_patch_offset);
    size_t free_space = cache.get_block_size().value() - (size_t)next_patch_offset;
    if (patch_serialized_size > free_space) {
        // Try reclaiming some space (this usually switches to another log block)
        const block_id_t initial_log_block = active_log_block;
        reclaim_space(patch_serialized_size);
        free_space = cache.get_block_size().value() - (size_t)next_patch_offset;

        // Check if enough space could be reclaimed
        if (patch_serialized_size > free_space) {
            // No success :-(
            // We go back to the initial block to make sure that this one gets flushed
            // when flush_n_oldest_blocks is called next (as it is obviously full)...
            set_active_log_block(initial_log_block);
            return false;
        }
    }

    // Serialize patch at next_patch_offset, increase offset
    mc_buf_t *log_buf = log_block_bufs[active_log_block - first_block];
    rassert(log_buf);
    block_is_empty[active_log_block - first_block] = false;

    //fprintf(stderr, "Stored to log block %d at offset %d\n", (int)active_log_block, (int)next_patch_offset);

    void *buf_data = log_buf->get_data_major_write();
    patch.serialize((char*)buf_data + next_patch_offset);
    next_patch_offset += patch_serialized_size;

    return true;
}

// This function might block while it acquires old blocks from disk.
void diff_oocore_storage_t::flush_n_oldest_blocks(unsigned int n) {
    rassert(log_block_bufs.size() == number_of_blocks);
    cache.assert_thread();
    // TODO: assert flush_in_progress somehow?

    if (number_of_blocks == 0)
        return;

    n = std::min(number_of_blocks, n);

    // Flush the n oldest blocks
    waiting_for_flushes = 0;
    for (block_id_t i = 1; i <= n; ++i) {
        block_id_t current_block = active_log_block + i;
        if (current_block >= first_block + number_of_blocks)
            current_block -= number_of_blocks;

        if (!block_is_empty[current_block - first_block]) {
            // Spawn one coroutine for each block
            if (n > 1) {
                ++waiting_for_flushes;
                coro_t::spawn(&diff_oocore_storage_t::flush_block, this, current_block, coro_t::self());
            }
            else {
                flush_block(current_block); // save one roundtrip
            }
        }
    }

    if (waiting_for_flushes > 0)
        coro_t::wait();

    // If we affected the active block, we have to reset next_patch_offset
    if (n == number_of_blocks)
        set_active_log_block(active_log_block);
}

void diff_oocore_storage_t::reclaim_space(const size_t space_required) {
    block_id_t compress_block_id = select_log_block_for_compression();
    compress_block(compress_block_id);
    set_active_log_block(compress_block_id);
}

block_id_t diff_oocore_storage_t::select_log_block_for_compression() {
    block_id_t result = active_log_block + 1;
    if (result >= first_block + number_of_blocks) {
        result -= number_of_blocks;
    }
    return result;
}

void diff_oocore_storage_t::compress_block(const block_id_t log_block_id) {
    //fprintf(stderr, "Compressing log block %d...", (int)log_block_id);

    cache.assert_thread();

    std::vector<buf_patch_t*> live_patches;
    live_patches.reserve(cache.get_block_size().value() / 30);

    // Scan over the block and save patches that we want to preserve
    mc_buf_t *log_buf = log_block_bufs[log_block_id - first_block];
    void *buf_data = log_buf->get_data_major_write();
    guarantee(strncmp((char*)buf_data, LOG_BLOCK_MAGIC, sizeof(LOG_BLOCK_MAGIC)) == 0);
    uint16_t current_offset = sizeof(LOG_BLOCK_MAGIC);
    bool log_block_changed = false;
    while (current_offset + buf_patch_t::get_min_serialized_size() < cache.get_block_size().value()) {
        buf_patch_t *patch = buf_patch_t::load_patch((char*)buf_data + current_offset);
        if (!patch) {
            break;
        }
        else {
            current_offset += patch->get_serialized_size();

            // We want to preserve this patch iff it is >= the oldest patch that we have in the in-core storage
            const std::list<buf_patch_t*>* patches = cache.diff_core_storage.get_patches(patch->get_block_id());
            rassert(!patches || patches->size() > 0);
            if (patches && !(*patch < *patches->front()))
                live_patches.push_back(patch);
            else {
                delete patch;
                log_block_changed = true;
            }
        }
    }

    if (log_block_changed) {
        // Wipe the log block
        init_log_block(log_block_id);

        // Write back live patches
        rassert(log_buf);
        buf_data = log_buf->get_data_major_write();

        guarantee(strncmp((char*)buf_data, LOG_BLOCK_MAGIC, sizeof(LOG_BLOCK_MAGIC)) == 0);
        current_offset = sizeof(LOG_BLOCK_MAGIC);
        for (std::vector<buf_patch_t*>::iterator patch = live_patches.begin(); patch != live_patches.end(); ++patch) {
            (*patch)->serialize((char*)buf_data + current_offset);
            current_offset += (*patch)->get_serialized_size();
            delete *patch;
        }
    } else {
        for (std::vector<buf_patch_t*>::iterator patch = live_patches.begin(); patch != live_patches.end(); ++patch) {
            delete *patch;
        }
    }

    //fprintf(stderr, " done\n");
}

void diff_oocore_storage_t::flush_block(const block_id_t log_block_id, coro_t* notify_coro) {
    //fprintf(stderr, "Flushing log block %d...", (int)log_block_id);
    cache.assert_thread();

    // TODO: Parallelize block reads

    // Scan over the block
    mc_buf_t *log_buf = log_block_bufs[log_block_id - first_block];;
    const void *buf_data = log_buf->get_data_read();
    guarantee(strncmp((char*)buf_data, LOG_BLOCK_MAGIC, sizeof(LOG_BLOCK_MAGIC)) == 0);
    uint16_t current_offset = sizeof(LOG_BLOCK_MAGIC);
    while (current_offset + buf_patch_t::get_min_serialized_size() < cache.get_block_size().value()) {
        buf_patch_t *patch = buf_patch_t::load_patch((char*)buf_data + current_offset);
        if (!patch) {
            break;
        }
        else {
            current_offset += patch->get_serialized_size();

            // For each patch, acquire the affected block and call ensure_flush()
            // We have to do this only if there is any potentially applicable patch in the in-core storage...
            // (Note: we rely on the fact that deleted blocks never show up in the in-core diff storage)
            if (cache.diff_core_storage.get_patches(patch->get_block_id())) {
                // We never have to lock the buffer, as we neither really read nor write any data
                // We just have to make sure that the buffer cache loads the block into memory
                // and then make writeback write it back in the next flush
                mc_buf_t *data_buf = acquire_block_no_locking(patch->get_block_id());
                // Check in-core storage again, now that the block has been acquired (old patches might have been evicted from it by doing so)
                if (cache.diff_core_storage.get_patches(patch->get_block_id())) {
                    data_buf->ensure_flush();
                }

                data_buf->release();
            }

            delete patch;
        }
    }

    // Wipe the log block
    init_log_block(log_block_id);
    block_is_empty[log_block_id - first_block] = true;

    //fprintf(stderr, " done\n");
    if (notify_coro) {
        --waiting_for_flushes;
        if (waiting_for_flushes == 0)
            notify_coro->notify();
    }
}

void diff_oocore_storage_t::set_active_log_block(const block_id_t log_block_id) {
    rassert (log_block_id >= first_block && log_block_id < first_block + number_of_blocks);
    active_log_block = log_block_id;

    // Scan through the block to determine next_patch_offset
    mc_buf_t *log_buf = log_block_bufs[active_log_block - first_block];
    const void *buf_data = log_buf->get_data_read();
    guarantee(strncmp((char*)buf_data, LOG_BLOCK_MAGIC, sizeof(LOG_BLOCK_MAGIC)) == 0);
    uint16_t current_offset = sizeof(LOG_BLOCK_MAGIC);
    while (current_offset + buf_patch_t::get_min_serialized_size() < cache.get_block_size().value()) {
        buf_patch_t *tmp_patch = buf_patch_t::load_patch((char*)buf_data + current_offset);
        if (!tmp_patch) {
            break;
        }
        else {
            current_offset += tmp_patch->get_serialized_size();
            delete tmp_patch;
        }
    }
    
    next_patch_offset = current_offset;
}

void diff_oocore_storage_t::init_log_block(const block_id_t log_block_id) {
    mc_buf_t *log_buf = log_block_bufs[log_block_id - first_block];;
    void *buf_data = log_buf->get_data_major_write();

    memcpy(buf_data, LOG_BLOCK_MAGIC, sizeof(LOG_BLOCK_MAGIC));
    bzero((char*)buf_data + sizeof(LOG_BLOCK_MAGIC), cache.serializer->get_block_size().value() - sizeof(LOG_BLOCK_MAGIC));
}

// Just the same as in buffer_cache/co_functions.cc (TODO: Refactor)
struct co_block_available_callback_2_t : public mc_block_available_callback_t {
    coro_t *self;
    mc_buf_t *value;

    virtual void on_block_available(mc_buf_t *block) {
        value = block;
        self->notify();
    }

    mc_buf_t *join() {
        self = coro_t::self();
        coro_t::wait();
        return value;
    }
};

mc_buf_t* diff_oocore_storage_t::acquire_block_no_locking(const block_id_t block_id) {
    cache.assert_thread();

    mc_inner_buf_t *inner_buf = cache.page_map.find(block_id);
    if (!inner_buf) {
        /* The buf isn't in the cache and must be loaded from disk */
        inner_buf = new mc_inner_buf_t(&cache, block_id);
    }
    
    // We still have to acquire the lock once to wait for the buf to get ready
    mc_buf_t *buf = new mc_buf_t(inner_buf, rwi_read);

    if (buf->ready) {
        // Release the lock we've got
        buf->inner_buf->lock.unlock();
        buf->non_locking_access = true;
        buf->mode = rwi_write;
        return buf;
    } else {
        co_block_available_callback_2_t cb;
        buf->callback = &cb;
        buf = cb.join();
        // Release the lock we've got
        buf->inner_buf->lock.unlock();
        buf->non_locking_access = true;
        buf->mode = rwi_write;
        return buf;
    }
}
