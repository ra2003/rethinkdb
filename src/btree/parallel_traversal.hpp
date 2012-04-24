#ifndef BTREE_PARALLEL_TRAVERSAL_HPP_
#define BTREE_PARALLEL_TRAVERSAL_HPP_

#include "utils.hpp"
#include <boost/shared_ptr.hpp>
#include <boost/ptr_container/ptr_vector.hpp>

#include "buffer_cache/types.hpp"
#include "concurrency/access.hpp"
#include "concurrency/rwi_lock.hpp"
#include "containers/scoped_malloc.hpp"

struct btree_superblock_t;
class traversal_state_t;
class parent_releaser_t;
class btree_slice_t;
struct btree_key_t;
struct internal_node_t;
class got_superblock_t;


// HEY: Make this an abstract class, have two separate implementation
// classes for the "forced block id" case and the internal node case.
// (Hint: I don't really care.)
class ranged_block_ids_t {
public:
    ranged_block_ids_t(block_size_t bs, const internal_node_t *node,
                       const btree_key_t *left_exclusive_or_null,
                       const btree_key_t *right_inclusive_or_null)
        : node_(bs.value()),
          left_exclusive_or_null_(left_exclusive_or_null),
          right_inclusive_or_null_(right_inclusive_or_null) {
        memcpy(node_.get(), node, bs.value());
    }
    ranged_block_ids_t(block_id_t forced_block_id,
                       const btree_key_t *left_exclusive_or_null,
                       const btree_key_t *right_inclusive_or_null)
        : node_(),
          forced_block_id_(forced_block_id),
          left_exclusive_or_null_(left_exclusive_or_null),
          right_inclusive_or_null_(right_inclusive_or_null) { }

    int num_block_ids() const;
    void get_block_id_and_bounding_interval(int index,
                                            block_id_t *block_id_out,
                                            const btree_key_t **left_excl_bound_out,
                                            const btree_key_t **right_incl_bound_out) const;

private:
    scoped_malloc<internal_node_t> node_;
    block_id_t forced_block_id_;
    const btree_key_t *left_exclusive_or_null_;
    const btree_key_t *right_inclusive_or_null_;
};

class interesting_children_callback_t : public lock_in_line_callback_t {
public:
    // Call these function in filter_interesting_children.
    void receive_interesting_child(int child_index);
    void no_more_interesting_children();

    // internal
    void on_in_line();
    void decr_acquisition_countdown();

    interesting_children_callback_t(traversal_state_t *_state, parent_releaser_t *_releaser, int _level, boost::shared_ptr<ranged_block_ids_t>& _ids_source)
        : state(_state), releaser(_releaser), level(_level), acquisition_countdown(1), ids_source(_ids_source) { }

private:
    traversal_state_t *state;
    parent_releaser_t *releaser;
    int level;
    int acquisition_countdown;
    boost::shared_ptr<ranged_block_ids_t> ids_source;

    DISABLE_COPYING(interesting_children_callback_t);
};

class traversal_progress_t;

struct btree_traversal_helper_t {
    btree_traversal_helper_t()
        : progress(NULL)
    { }

    // This is free to call mark_deleted.
    virtual void process_a_leaf(transaction_t *txn, buf_lock_t *leaf_node_buf,
                                const btree_key_t *left_exclusive_or_null,
                                const btree_key_t *right_inclusive_or_null) = 0;

    virtual void postprocess_internal_node(buf_lock_t *internal_node_buf) = 0;

    virtual void postprocess_btree_superblock(buf_lock_t *superblock_buf) = 0;

    virtual void filter_interesting_children(transaction_t *txn, ranged_block_ids_t *ids_source, interesting_children_callback_t *cb) = 0;

    virtual access_t btree_superblock_mode() = 0;
    virtual access_t btree_node_mode() = 0;

    virtual ~btree_traversal_helper_t() { }

    traversal_progress_t *progress;
};

void btree_parallel_traversal(transaction_t *txn, btree_slice_t *slice, btree_traversal_helper_t *helper);
void btree_parallel_traversal(transaction_t *txn, got_superblock_t &superblock, btree_slice_t *slice, btree_traversal_helper_t *helper);

class traversal_progress_t : public home_thread_mixin_t {
public:
    traversal_progress_t()
        : height(-1), print_counter(0)
    { }

    enum action_t {
        LEARN,
        ACQUIRE,
        RELEASE
    };

    enum node_type_t {
        UNKNOWN,
        INTERNAL,
        LEAF
    };

    void inform(int level, action_t, node_type_t);

    std::pair<int, int> guess_completion();

private:
    std::vector<int> learned; //How many nodes at each level we believe exist
    std::vector<int> acquired; //How many nodes at each level we've acquired
    std::vector<int> released; //How many nodes at each level we've released

    int height; //The height we've learned the tree has. Or -1 if we're still unsure;

    int print_counter;
};

class traversal_progress_combiner_t : public home_thread_mixin_t {
public:
    traversal_progress_combiner_t() { }

    void add_constituent(traversal_progress_t *);
    std::pair<int, int> guess_completion();

private:
    boost::ptr_vector<traversal_progress_t> constituents;
};

#endif  // BTREE_PARALLEL_TRAVERSAL_HPP_
