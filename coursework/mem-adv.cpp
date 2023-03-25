/*
 * The Buddy-Fibonacci Chimera Page Allocator
 * Basically, 
 *  - lower half of memory uses binary buddy allocator
 *  - upper half of memory uses fibonacci buddy allocator
 * FOR TASK 2 ADV.
 * 
 * B171926 -- Unfinished
 */

#include <infos/mm/page-allocator.h>
#include <infos/mm/mm.h>
#include <infos/kernel/kernel.h>
#include <infos/kernel/log.h>
#include <infos/util/math.h>
#include <infos/util/printf.h>

using namespace infos::kernel;
using namespace infos::mm;
using namespace infos::util;

#define BUDDY_MAX_ORDER 18
#define TWO_POW(order) 1ull << order

/* [RANT]
 * `#pragma region` is a godsend -- scrolling over 1,000 lines of code gives me a wicked bad 
 * migraine because apparently for whatever reason we cannot introduce header files (was it 
 * because how makefile was written?!) we defined on our own. 
 */

/** 
 * Helper functions defined for buddy subset, mostly arithmetic-related.
 */
namespace buddy_helper {
    /**
     * Preprocessor defines doesn't bide well with static analysis for pointer arithmetics. 
     * This is a wrapper for TWO_POW.
     */
    inline constexpr uint64_t two_pow(uint32_t order) {
        return TWO_POW(order); 
    }

    /**
     * Checks if `elem_ptr` is in an contiguous "array" -- delineated by `base_ptr` and `len`. 
     */
    template<typename T>
    inline const bool in_ptr_bound(
        const T* elem_ptr, 
        const T* base_ptr, 
        const uint64_t len
    ) {
        return base_ptr <= elem_ptr && elem_ptr < base_ptr + len; 
    }

    /**
     * Finds whether `block_ptr` is aligned left wrt `base_ptr` by given order. 
     * i.e., whether `block_ptr - base_ptr` is a multiple of 2^order.
     */
    template<typename T>
    inline bool aligned_by_order(
        T* const block_ptr, 
        T* const base_ptr, 
        const uint64_t order
    ) {
        const size_t alignment = TWO_POW(order); 
        const size_t idx_of_block = block_ptr - base_ptr; 
        return (idx_of_block % alignment == 0); 
    }

    /**
     * Finds the pointer to previous 2^order-block given `block_ptr` wrt `base_ptr[len]`. 
     * 
     * @returns block_ptr - alignment, or NULL if out of bounds
     */
    template<typename T>
    inline T* prev_block_ptr(
        T* const block_ptr, 
        T* const base_ptr, 
        const uint64_t len, 
        const uint64_t order
    ) {
        assert(aligned_by_order(block_ptr, base_ptr, order));  
        const size_t alignment = TWO_POW(order); 
        T* result_ptr = block_ptr - alignment; 
        if (!in_ptr_bound(result_ptr, base_ptr, len)) {
            return NULL; 
        } else {
            return result_ptr; 
        }
    }

    /**
     * Finds the pointer to next 2^order-block given `block_ptr` wrt `base_ptr[len]`. 
     * 
     * @returns block_ptr + alignment, or NULL if out of bounds
     */
    template<typename T>
    inline T* next_block_ptr(
        T* const block_ptr, 
        T* const base_ptr, 
        const uint64_t len, 
        const uint64_t order
    ) {
        assert(aligned_by_order(block_ptr, base_ptr, order)); 
        const size_t alignment = TWO_POW(order); 
        T* result_ptr = block_ptr + alignment; 
        if (!in_ptr_bound(result_ptr, base_ptr, len)) {
            return NULL; 
        } else {
            return result_ptr; 
        }
    }
}; 

/**
 * Helper functions defined for fibonacci subset, mostly arithmetic-related. 
 */
namespace fib_helper {
	struct PgdPtrPair {
		PageDescriptor* left; 
		PageDescriptor* right; 
	}; 

    /**  
     * Knuth's matrix method for converting index to _fib_free_areas dynamic allocation 
     * to their corresponding fibonacci number (i.e., block size) in O(lg(idx)) time. 
     * 
     * i.e., Computes (idx + 1)-th fibinacci number. 
     * 
     * Matrix method can be slow for small idxs, as shown in https://habr.com/en/post/148336/ . 
     * Therefore had it not been a coursework that took me too much time to write I would prob. 
     * have optimized this accordingly. 
     * 
     * ISTG this coursework is eating into my time for SDP & stuff so much -_-
     */
    inline uint32_t idx_to_fib(size_t idx) {
        uint32_t f_mat[4] = {
            1, 1, 
            1, 0
        }; 

        /* Compute f_mat^n */
        bool idx_is_pow_of_two = (idx & (idx - 1)) == 0; 
        if (idx_is_pow_of_two) {
            // => Well-formed binary tree dependency
            size_t exponent = 1; 
            while (exponent < idx) {
                const uint32_t temp[4] {
                    f_mat[0], f_mat[1], 
                    f_mat[2], f_mat[3]
                }; 
                f_mat[0] = temp[0] * temp[0] + temp[1] * temp[2];
                f_mat[1] = temp[0] * temp[1] + temp[1] * temp[3]; 
                f_mat[2] = temp[2] * temp[0] + temp[2] * temp[3]; 
                f_mat[3] = temp[2] * temp[1] + temp[3] * temp[3]; 
                exponent <<= 1; 
            }
            return f_mat[0]; 
        } else {
            // => Destructure pow-of-two component of idx
            size_t pow_of_two_component = 1 << ilog2_floor(idx); 
            return idx_to_fib(pow_of_two_component) * idx_to_fib(idx - pow_of_two_component); 
        }
    }

    /** 
     * Inverse of idx_to_fib; converts a given fibonacci number to its index form in fibonacci 
     * sequence through binary search. 
     */
    inline size_t fib_to_idx(uint32_t fib_x) {
        size_t idx = 1; 
        size_t shift_at = 0; 
        size_t shift_lim = sizeof(size_t); 

        while (true) {
            auto curr_fib = fib_to_idx(idx); 
            if (curr_fib == fib_x) {
                return idx - 1; 
            } else if (curr_fib < fib_x) {
                if (shift_at != -1) idx -= (1 << shift_at); // Undo current trailing 1, if available
                shift_at++; 
                assert(shift_at < shift_lim); 
                idx += (1 << shift_at);     // Add new trailing 1
            } else if (curr_fib > fib_x) {
                idx -= (1 << shift_at);     // Undo current trailing 1
                idx += (1 << (--shift_at)); // Redo second largest trailing 1
                shift_lim = shift_at;       // Set new "fixed" digit limit
                shift_at = -1; 
            } 
        }
    }

	/**
	 * Finds the first fibonacci number >= pg_count.
	 * 
	 * Lazy implementation. A better implementation could be to do fixed-point arithmetics, but ah 
     * well... 
	 */
    inline uint32_t count_to_fib_ceil(size_t pg_count) {
        assert(pg_count < __UINT32_MAX__); 
        size_t curr_idx = 0; 
        uint32_t curr_fib = idx_to_fib(0); 
        while (curr_fib < pg_count) {
            // Since fibonacci numbers grow exponentially, 
            // This loop take log(pg_count) time. 
            curr_idx++; 
            curr_fib = idx_to_fib(curr_idx); 
        }
        return curr_fib; 
    }

	/** 
     * Finds the first fibonacci number >= 2^order.
     */
    inline uint32_t order_to_fib_ceil(size_t order) {
        size_t pg_count = TWO_POW(order); 
        return count_to_fib_ceil(pg_count); 
    }

	/**
	 * Finds the last fibonacci number <= pg_count. 
	 * 
	 * Lazy implementation. A better implementation could be to do fixed-point arithmetics, but ah 
	 * well...
	 */
	inline uint32_t count_to_fib_floor(size_t pg_count) {
		assert(pg_count < __UINT32_MAX__); 
		size_t curr_idx = 1;
		uint32_t last_fib = idx_to_fib(0); 
		uint32_t curr_fib = idx_to_fib(1); 
		while (curr_fib <= pg_count) {
			curr_idx++;
			last_fib = curr_fib;  
			curr_fib = idx_to_fib(curr_idx); 
		}
		return last_fib; 
	}
}; 

/**
 * A hybrid buddy-like page allocation algorithm where: 
 * - Lower half of the memory is allocated by binary buddy allocator. 
 * - Upper half of the memory is allocated by Fibonacci buddy allocator. 
 * - Kernel may only reserve lower half of the memory. 
 */
class ChimeraPageAllocator : public PageAllocatorAlgorithm {
private: 
    /* Base pointer for the page descriptor table. */
	PageDescriptor* _pgds_base; 

	/* 
	 * Length of the page descriptor table. 
	 * i.e., number of page (descriptors) available in system. 
	 */
	uint64_t _pgds_len; 

    /* 
     * Limit pointer for the page descriptor table. Points to hypothetical (last + 1) element. 
     * 
     * [DANGER] NEVER dereference -- out of bounds. 
     */
    uintptr_t _pgds_lim; 

#pragma region buddy_subset
	/* 
	 * Base pointer for the page descriptor table visible to buddy subset
	 */
	PageDescriptor* _buddy_pgds_base = _pgds_base; 

    /* 
     * Length of the page descriptor table for the buddy subset of this allocator. 
     */
    uint64_t _buddy_pgds_len = _pgds_len / 2; 

	/* 
     * Limit pointer for the buddy subset of this allocator. 
     * 
     * [DANGER] NEVER dereference -- out of bounds. 
     */
    uintptr_t _buddy_pgds_lim = (uintptr_t)_pgds_base + (_pgds_lim - (uintptr_t)_pgds_base) / 2; 

	/* 
     * Array of page descriptor pointers for caching the beginning of first pgd for contiguous 
     * 2^order page cluster allocations. 
     * 
     * Used by the binary buddy allocator. 
     */
    PageDescriptor *_buddy_free_areas[BUDDY_MAX_ORDER + 1] { NULL }; 

    /** 
     * Given a page descriptor, and an order, returns the buddy PGD.  The buddy could either be
	 * to the left or the right of PGD, in the given order.
     * 
     * @attention
     * This function is expected to work with the buddy subset of this allocator. 
     * 
	 * @param pgd The page descriptor to find the buddy for.
	 * @param order The order in which the page descriptor lives.
	 * @return Returns the buddy of the given page descriptor, in the given order, or NULL if no 
	 * buddy exists (e.g., odd-divided pgd table). 
	 */
	PageDescriptor* buddy_of(PageDescriptor* pgd, int order)
	{
        const char* const _FN_IDENT = "[chimera(buddy)::buddy_of]"; 

		const size_t pfn_of_pgd = sys.mm().pgalloc().pgd_to_pfn(pgd); 
		if ((pfn_of_pgd >> order) % 2) {
			// => odd "block idx", return previous block
			return buddy_helper::prev_block_ptr(pgd, _buddy_pgds_base, _buddy_pgds_len, order); 
		} else {
			// => even "block idx", return next block
			return buddy_helper::next_block_ptr(pgd, _buddy_pgds_base, _buddy_pgds_len, order); 
		}
	}

    /**
	 * Helper function.
	 * 
	 * Alters a block of given order to RESERVED status and deletes it from _free_areas[order] LL. 
	 * This function performs no check for memory safety whatsoever. Use with caution.
     * 
     * @attention
     * This function is expected to work with the buddy subset of this allocator. 
	 */
	void buddy_reserve_block(PageDescriptor* block_base, size_t order) {
        const char* const _FN_IDENT = "[chimera(buddy)::reserve_block]"; 

		PageDescriptor* block_lim = block_base + buddy_helper::two_pow(order);
		PageDescriptor* block_buddy = buddy_of(block_base, order); 
		assert(buddy_helper::in_ptr_bound(block_buddy, _buddy_pgds_base, _buddy_pgds_len)); 
		assert(block_buddy != block_base); 

		if (_buddy_free_areas[order] == block_base) {
			// => Top of _free_areas[order] to be reserved
			/* Alter _free_areas linkage */
			assert(block_base->prev_free == NULL); 
			_buddy_free_areas[order] = block_base->next_free; 
			if (block_base->next_free != NULL) block_base->next_free->prev_free = NULL; 
			
			/* Store buddy ptr */
			if (block_buddy < block_base) {
				block_base->prev_free = block_buddy; 
				block_base->next_free = NULL; 
			} else {
				block_base->prev_free = NULL; 
				block_base->next_free = block_buddy; 
			}
		} else {
			// => otherwise...
			/* Alter _free_areas linkage */
			assert(block_base->prev_free != NULL); 
			block_base->prev_free->next_free = block_base->next_free; 
			if (block_base->next_free != NULL) {
				block_base->next_free->prev_free = block_base->prev_free; 
			}

			/* Store buddy ptr */
			if (block_buddy < block_base) {
				block_base->prev_free = block_buddy; 
				block_base->next_free = NULL; 
			} else {
				block_base->prev_free = NULL; 
				block_base->next_free = block_buddy; 
			}
		}

		for (PageDescriptor* pgd = block_base; pgd < block_lim; pgd++) {
			block_base->type = PageDescriptorType::RESERVED; 
		}

		mm_log.messagef(
			LogLevel::INFO, 
			"%s Reserved block [pgn@0x%lx (%lx), pgn@0x%lx (%lx)).", 
            _FN_IDENT, 
			block_base, sys.mm().pgalloc().pgd_to_pfn(block_base), 
			block_lim, sys.mm().pgalloc().pgd_to_pfn(block_lim)
		);
	}

    /**
	 * Given a pointer to a block of free memory in the order "source_order", this function will
	 * split the block in half, and insert it into the order below.
     * 
     * @attention
     * This function is expected to work with the buddy subset of this allocator. 
     * 
	 * @param block_pointer A pointer to a pointer containing the beginning of a block of free memory.
	 * @param source_order The order in which the block of free memory exists.  Naturally,
	 * the split will insert the two new blocks into the order below.
	 * @return Returns the left-hand-side of the new block.
	 */
	PageDescriptor* buddy_split_block(PageDescriptor** block_pointer, int source_order)
	{
        const char* const _FN_IDENT = "[chimera(buddy)::buddy_split_block]"; 

		PageDescriptor* block_ptr = *block_pointer; 
		assert(
			block_ptr->next_free == NULL || 
			block_ptr->next_free - block_ptr >= TWO_POW(source_order)
		);

		/* Maintain pgd & _free_areas state */
		if (_buddy_free_areas[source_order] == block_ptr) {
			_buddy_free_areas[source_order] = block_ptr->next_free; 
		}
		if (block_ptr->prev_free != NULL) {
			block_ptr->prev_free->next_free = block_ptr->next_free; 
		}
		if (block_ptr->next_free != NULL) {
			block_ptr->next_free->prev_free = block_ptr->prev_free; 
		}
		block_ptr->prev_free = NULL; 
		block_ptr->next_free = NULL; 

		/* Split in half, update new state */
		const size_t tgt_order = source_order - 1; 
		PageDescriptor* half_left = block_ptr; 
		PageDescriptor* half_right = buddy_of(half_left, tgt_order);
		mm_log.messagef(
			LogLevel::DEBUG, 
			"%s Halves: L@0x%lx (%lx), R@0x%lx (%lx). Order: %d->%d", 
            _FN_IDENT, 
			half_left, sys.mm().pgalloc().pgd_to_pfn(half_left),
			half_right, sys.mm().pgalloc().pgd_to_pfn(half_right), 
			source_order, tgt_order
		);
		assert(__min((uintptr_t)half_left, (uintptr_t)half_right) == (uintptr_t)half_left); 

		// assert(half_left->type != PageDescriptorType::INVALID); 
		half_left->type = PageDescriptorType::AVAILABLE; 

		// assert(half_right->type != PageDescriptorType::INVALID); 
		half_right->type = PageDescriptorType::AVAILABLE; 

		half_left->next_free = half_right; 
		half_right->prev_free = half_left; 

		size_t tgt_alignment = TWO_POW(tgt_order);
		if (_buddy_free_areas[tgt_order] == NULL) {
			// => First time splitting into tgt_order
			half_left->prev_free = NULL; 
			half_right->next_free = NULL; 
			_buddy_free_areas[tgt_order] = half_left; 

		} else if (_buddy_free_areas[tgt_order] >= half_right + tgt_alignment) {
			// => Both blocks more top than current tgt_order entry, prioritize. 
			/* Alter linkage */
			half_left->prev_free = NULL; 
			half_right->next_free = _buddy_free_areas[tgt_order]; 
			_buddy_free_areas[tgt_order]->prev_free = half_right; 
			_buddy_free_areas[tgt_order] = half_left; 

		} else if (_buddy_free_areas[tgt_order] + tgt_alignment <= half_left) {
			// => Both blocks more bottom than current tgt_order entry, prioritize too 
            // since this is faster
			/* Alter linkage */
			half_left->prev_free = NULL; 
			half_right->next_free = _buddy_free_areas[tgt_order]; 
			_buddy_free_areas[tgt_order]->prev_free = half_right; 
			_buddy_free_areas[tgt_order] = half_left; 

		} else {
			// => Segmentation fault
			mm_log.messagef(
				LogLevel::FATAL, 
				"%s Block segmentation fault. Crashed!", 
                _FN_IDENT
			); 
			mm_log.messagef(
				LogLevel::DEBUG, 
				"%s Attempted to split {pgd@0x%lx, order: %d} to "
				"({pgd@0x%lx, order: %d}, {pgd@0x%lx, order: %d}) and insert wrt "
				"({pgd@0x%lx, order: %d} -> {pgd@0x%lx, order: %d}), but got segmentation fault.", 
                _FN_IDENT, 
				block_ptr, 
				source_order, 
				half_left, 
				tgt_order, 
				half_right, 
				tgt_alignment, 
				_buddy_free_areas[tgt_order], 
				tgt_order, 
				_buddy_free_areas[tgt_order]->next_free, 
				(_buddy_free_areas[tgt_order]->next_free == NULL) ? -1 : tgt_order
			); 
			assert(false); 
		}
		return half_left; 
	}

    /**
	 * Takes a block in the given source order, and merges it (and its buddy) into the next order.
     * 
     * @attention
     * This function is expected to work with the buddy subset of this allocator. 
     * 
	 * @param block_pointer A pointer to a pointer containing a block in the pair to merge.
	 * @param source_order The order in which the pair of blocks live.
	 * @return Returns the new slot that points to the merged block, or NULL if merge failed. 
	 */
	PageDescriptor** buddy_merge_block(PageDescriptor** block_pointer, int source_order)
	{
        const char* const _FN_IDENT = "[chimera(buddy)::buddy_merge_block]"; 
		assert(0 <= source_order && source_order < BUDDY_MAX_ORDER); 

		PageDescriptor* block_ptr = *block_pointer; 
		PageDescriptor* src_l_block; 
		PageDescriptor* src_r_block; 
		PageDescriptor* buddy_ptr = buddy_of(block_ptr, source_order); 
		{
			block_ptr->type = PageDescriptorType::AVAILABLE; 
			if (buddy_ptr < block_ptr) {
				src_l_block = buddy_ptr; 
				src_r_block = block_ptr; 
			} else {
				src_l_block = block_ptr; 
				src_r_block = buddy_ptr; 
			}
		}

		/* Check if mergeable */
		if (buddy_ptr->type != PageDescriptorType::AVAILABLE) {
			// => L, R not mergeable -- buddy not free.
			mm_log.messagef(
				LogLevel::ERROR, 
				"%s Cannot merge ({pgd@0x%lx, order: %d}, {pgd@0x%lx, order: %d}) "
				"because one of the buddy blocks is not marked as AVAILABLE.", 
                _FN_IDENT, 
				src_l_block, source_order, 
				src_r_block, source_order
			); 
			return NULL; 
		}
		// => L, R mergeable -- buddy free.

		/* Assert correct internal linkage */
		assert(src_l_block->next_free == src_r_block && src_r_block->prev_free == src_l_block); 

		/* Alter & delete external linkage */
		if (_buddy_free_areas[source_order] != src_l_block) {
			src_l_block->prev_free->next_free = src_r_block->next_free; 
		}
		if (src_r_block->next_free != NULL) {
			src_r_block->next_free->prev_free = src_l_block->prev_free; 
		}
		_buddy_free_areas[source_order] = src_r_block->next_free; 
		src_l_block->prev_free = NULL; 
		src_l_block->next_free = NULL; 
		src_r_block->next_free = NULL; 
		src_r_block->prev_free = NULL; 

		/* Merge and alter external linkage */
		PageDescriptor* tgt_block (src_l_block); 
		size_t tgt_order = source_order + 1; 
		size_t alignment = TWO_POW(tgt_order); 
		
		/* Much like cases in split_block... */
		if (_buddy_free_areas[tgt_order] == NULL) {
			_buddy_free_areas[tgt_order] = tgt_block; 

		} else if (_buddy_free_areas[tgt_order] + alignment <= tgt_block) {
			tgt_block->next_free = _buddy_free_areas[tgt_order]; 
			_buddy_free_areas[tgt_order]->prev_free = tgt_block; 
			_buddy_free_areas[tgt_order] = tgt_block; 

		} else if (tgt_block + alignment <= _buddy_free_areas[tgt_order]) {
			tgt_block->next_free = _buddy_free_areas[tgt_order]; 
			_buddy_free_areas[tgt_order]->prev_free = tgt_block; 
			_buddy_free_areas[tgt_order] = tgt_block; 			

		} else {
			// => Segmentation fault
			mm_log.messagef(
				LogLevel::FATAL, 
				"%s Block segmentation fault. Crashed!", 
                _FN_IDENT
			); 
			mm_log.messagef(
				LogLevel::DEBUG, 
				"%s Attempted to merge ({pgd@0x%lx, order: %d}, {pgd@0x%lx, order: %d}) to "
				"{pgd@0x%lx, order: %d} and insert wrt "
				"({pgd@0x%lx, order: %d} -> {pgd@0x%lx, order: %d}), but got segmentation fault.", 
                _FN_IDENT, 
				src_l_block, source_order, 
				src_r_block, source_order, 
				tgt_block, tgt_order, 
				_buddy_free_areas[tgt_order], tgt_order, 
				_buddy_free_areas[tgt_order]->next_free, 
				(_buddy_free_areas[tgt_order] == NULL) ? -1 : tgt_order
			); 
			assert(false); 
		}

		mm_log.messagef(
			LogLevel::DEBUG, 
			"%s Merged ({pgd@0x%lx, order: %d}, {pgd@0x%lx, order: %d}) to "
			"{pgd@0x%lx, order: %d}.", 
            _FN_IDENT, 
			src_l_block, source_order, 
			src_r_block, source_order, 
			tgt_block, tgt_order
		); 
		return &_buddy_free_areas[tgt_order]; 
	}

    /**
	 * Allocates 2^order number of contiguous pages
     * 
     * @attention
     * This function only allocates memory from the subset managed by buddy allocator. 
     * 
	 * @param order The power of two, of the number of contiguous pages to allocate.
	 * @return Returns a pointer to the first page descriptor for the newly allocated page range, or 
	 * NULL if allocation failed.
	 */
	PageDescriptor* buddy_allocate_pages(int order)
	{
        const char* const _FN_IDENT = "[chimera(buddy)::allocate_pages]"; 
		mm_log.messagef(
			LogLevel::DEBUG, 
			"%s Trying to allocate %d-order block...", 
			_FN_IDENT, order
		); 
		if (_buddy_free_areas[order] != NULL) {
			// => Exists subdivision in _free_areas
			auto allocated = _buddy_free_areas[order];

			// Costly, but required by kernel -- otherwise won't pass assertion
			for (PageDescriptor* p = allocated; p < allocated + buddy_helper::two_pow(order); p++) {
				// [Spec?] Check if pg_from_allocated is of a allocable type? 
				p->type = PageDescriptorType::AVAILABLE; 
			}

			/* Alter _free_areas state */
			assert(allocated->prev_free == NULL); 
			_buddy_free_areas[order] = allocated->next_free; 
			if (_buddy_free_areas[order] != NULL) _buddy_free_areas[order]->prev_free = NULL; 

			/* Keep bookmark on intended buddy 
			 * I don't like this way of implementing things at all -- one thing for more than one 
			 * well-defined purpose. But otherwise it would be very difficult to know which 
			 * allocation's sized to what -- difficult for freeing it up. 
			 * 
			 * Ideally the `PageDescriptor` type should be expanded with 1 uint8_t field to store 
			 * order, but for potrability's sake this is used as a bandaid solution. 
			 */
			auto allocated_buddy = buddy_of(allocated, order); 
			assert(buddy_helper::in_ptr_bound(allocated_buddy, _buddy_pgds_base, _buddy_pgds_len)); 
			assert(allocated_buddy != allocated); 
			if (allocated_buddy < allocated) {
				allocated->prev_free = allocated_buddy; 
				allocated->next_free = NULL; 
			} else {
				allocated->prev_free = NULL;
				allocated->next_free = allocated_buddy; 
			}
			// DO NOT alter those of allocated_buddy as we don't know whether they are available 
			// (i.e., in _free_areas) or not!

			mm_log.messagef(
				LogLevel::INFO, 
				"%s Allocated block {[pgd@0x%lx (%lx), pgd@0x%lx (%lx)), order: %d}.", 
                _FN_IDENT, 
				allocated, sys.mm().pgalloc().pgd_to_pfn(allocated), 
				allocated + buddy_helper::two_pow(order), 
                sys.mm().pgalloc().pgd_to_pfn(allocated + buddy_helper::two_pow(order)), 
				order
			); 
			return allocated; 
		}
		// => Otherwise 2 branches: 
		size_t from_order = -1; 
		for (size_t o = order + 1; o <= BUDDY_MAX_ORDER; o++) {
			if (_buddy_free_areas[o] != NULL) {
				from_order = o; 
				break; 
			}
		}
		// 1. No larger subdivisions exist => not enough contiguous memory, return NULL. 
		if (from_order == -1) {
			mm_log.messagef(
				LogLevel::ERROR, 
				"%s Cannot allocate contiguous memory of order %d -- 0x%lx pages", 
                _FN_IDENT, 
                order, TWO_POW(order)
			); 
			return NULL; 
		}

		// 2. Try to split larger subdivisions until exists correct order, then return 
		mm_log.messagef(
			LogLevel::DEBUG, 
			"%s Splitting {[pgd@0x%lx (%lx), pgd@0x%lx (%lx)), order: %d}...", 
            _FN_IDENT, 
			_buddy_free_areas[from_order], 
			sys.mm().pgalloc().pgd_to_pfn(_buddy_free_areas[from_order]), 
			_buddy_free_areas[from_order] + buddy_helper::two_pow(from_order), 
			sys.mm().pgalloc().pgd_to_pfn(
                _buddy_free_areas[from_order] + buddy_helper::two_pow(from_order)
            ), 
			from_order
		); 
		buddy_split_block(&_buddy_free_areas[from_order], from_order); 
		return allocate_pages(order); 
	}

    /**
	 * Frees 2^order contiguous pages.
     * 
     * @attention
     * This function only frees memory from the subset managed by buddy allocator. 
     * 
	 * @param pgd A pointer to an array of page descriptors to be freed.
	 * @param order The power of two number of contiguous pages to free.
     * @returns True if freed up to given `order`. 
     * @returns False if otherwise. 
	 */
    bool buddy_free_pages(PageDescriptor* pgd, int order) 
    {
        const char* const _FN_IDENT = "[chimera(buddy)::buddy_free_pages]"; 

		PageDescriptor* buddy_pgd = (pgd->next_free == NULL) ? pgd->prev_free : pgd->next_free; 
		assert(buddy_pgd != NULL); 
		assert(buddy_pgd != pgd); 

		size_t pgd_alignment = (buddy_pgd > pgd) ? buddy_pgd - pgd : pgd - buddy_pgd; 
		size_t pgd_order = __log2ceil(pgd_alignment); 

		/* Free this block */
		// or... at least to pass merge_block assertions
		pgd->type = PageDescriptorType::AVAILABLE; 

		if (_buddy_free_areas[pgd_order] == NULL) {
			pgd->prev_free = NULL; 
			pgd->next_free = NULL; 
			_buddy_free_areas[pgd_order] = pgd; 
		} else {
			pgd->prev_free = NULL;
			pgd->next_free = _buddy_free_areas[pgd_order]; 
			_buddy_free_areas[pgd_order]->prev_free = pgd; 
			_buddy_free_areas[pgd_order] = pgd; 
		}

		/* Coalesce to larger order, if needed */
		for (; pgd_order < order; pgd_order++) {
			if (buddy_merge_block(&pgd, pgd_order) == NULL) break; 
			// merge_block maintains _free_areas by itself, so no worries
		}

		if (pgd_order != order) {
			// => unable to merge to given order, exists non-available buddy
			mm_log.messagef(
				LogLevel::ERROR, 
				"%s Freed up until {[pgd@0x%lx (%lx), pgd@0x%lx (%lx)), order: %d} "
				"instead of order %d -- encountered unavailable buddy block.", 
                _FN_IDENT, 
				pgd, sys.mm().pgalloc().pgd_to_pfn(pgd), 
				pgd + buddy_helper::two_pow(pgd_order), 
                sys.mm().pgalloc().pgd_to_pfn(pgd + buddy_helper::two_pow(pgd_order)), 
				pgd_order, order
			); 
            return false; 
		} else {
			// => merged to given order
			mm_log.messagef(
				LogLevel::INFO, 
				"%s Freed up block {[pgd@0x%lx (%lx), pgd@0x%lx (%lx)), order: %d}.", 
                _FN_IDENT, 
				pgd, sys.mm().pgalloc().pgd_to_pfn(pgd), 
				pgd + buddy_helper::two_pow(pgd_order), 
                sys.mm().pgalloc().pgd_to_pfn(pgd + buddy_helper::two_pow(pgd_order)), 
				pgd_order
			); 
            return true; 
		}
    }

    /**
     * Marks a range of pages as available for allocation.
     * 
     * @attention
     * This function assumes page range to be inserted are all within the subset managed by 
     * buddy allocator. 
     * 
     * @param start A pointer to the first page descriptors to be made available.
     * @param count The number of page descriptors to make available.
     */
    void insert_buddy_page_range(PageDescriptor *start, uint64_t count)
    {
        const char* const _FN_IDENT = "[chimera(buddy)::insert_buddy_page_range]"; 

		PageDescriptor* bound_base = start; 
		PageDescriptor* bound_lim = start + count;
		mm_log.messagef(
			LogLevel::INFO, 
			"%s Clearing [pgd@0x%x (pfn: 0x%x), pgd@0x%x (pfn: 0x%x)).", 
            _FN_IDENT, 
			bound_base, sys.mm().pgalloc().pgd_to_pfn(bound_base), 
			bound_lim, sys.mm().pgalloc().pgd_to_pfn(bound_lim)
		); 

		while (bound_base != bound_lim) {
			assert(bound_base < bound_lim);
			for (size_t order = BUDDY_MAX_ORDER; order >= 0; order--) {
				/* Check if bound_base aligned by order */
				if (!buddy_helper::aligned_by_order(bound_base, _buddy_pgds_base, order)) continue; 

				/* Check if bound_base at this order is in range */
				PageDescriptor* block_base = bound_base; 
				PageDescriptor* block_lim = bound_base + buddy_helper::two_pow(order); 
				if (block_lim > bound_lim) continue; 

				/* Init block of given order */
				block_base->type = PageDescriptorType::AVAILABLE; 
				if (_buddy_free_areas[order] == NULL) {
					// => First such order block
					block_base->prev_free = NULL;
					block_base->next_free = NULL; 
				} else {
					// => Prepend new order block
					block_base->prev_free = NULL; 
					block_base->next_free = _buddy_free_areas[order]; 
					_buddy_free_areas[order]->prev_free = block_base; 
				}
				_buddy_free_areas[order] = block_base; 

				bound_base = block_lim; 
				mm_log.messagef(
					LogLevel::DEBUG, 
					"%s At order %d, retrieved [pgd@0x%lx (%lx), pgd@0x%lx (%lx)). "
					"%lx pages remaining...", 
                    _FN_IDENT, 
					order, 
					block_base, sys.mm().pgalloc().pgd_to_pfn(block_base), 
					block_lim, sys.mm().pgalloc().pgd_to_pfn(block_lim), 
					bound_lim - bound_base
				);
				break; 
			}
		}
		mm_log.messagef(
			LogLevel::INFO, 
			"%s Finished clearance! Dumping state...", 
            _FN_IDENT
		);
		dump_state(); 
    }

    /**
     * Marks a range of pages as unavailable for allocation.
     * @param start A pointer to the first page descriptors to be made unavailable.
     * @param count The number of page descriptors to make unavailable.
     */
    void remove_buddy_page_range(PageDescriptor *start, uint64_t count)
    {
        const char* const _FN_IDENT = "[chimera(buddy)::remove_buddy_page_range]"; 

        PageDescriptor* bound_base = start; 
		PageDescriptor* bound_lim = start + count; 
		mm_log.messagef(
			LogLevel::INFO, 
			"%s Reserving [{pgd@0x%lx (%x)}, {pgd@0x%lx} (%x)).", 
            _FN_IDENT, 
			bound_base, sys.mm().pgalloc().pgd_to_pfn(bound_base), 
			bound_lim, sys.mm().pgalloc().pgd_to_pfn(bound_lim)
		); 

		/* Subprocedure for reserving all blocks for [bound_base, bound_lim) */
		find_block_for_bound:  
		for (size_t order = BUDDY_MAX_ORDER; order >= 0; order--) {
			PageDescriptor* block_base = _buddy_free_areas[order]; 

			while (block_base != NULL) {
				PageDescriptor* block_lim = block_base + buddy_helper::two_pow(order); 

				if (block_base == bound_base && block_lim == bound_lim) {
					// => [bound_base, bound_lim) aligned by buddy-ness, can fill into single block
					buddy_reserve_block(block_base, order);
					mm_log.messagef(
						LogLevel::INFO, 
						"%s Finished reservation! Dumping state...", 
                        _FN_IDENT
					);
					dump_state(); 
					return; 

				} else if (block_base == bound_base && block_lim < bound_lim) {
					// => [bound_base, bound_lim) left aligned by buddy-ness only, larger
					buddy_reserve_block(block_base, order); 
					bound_base = block_lim; 
					goto find_block_for_bound; 

				} else if (bound_base < block_base && block_lim == bound_lim) {
					// => [bound_base, bound_lim) right aligned by buddy-ness only, larger
					buddy_reserve_block(block_base, order); 
					bound_lim = block_base; 
					goto find_block_for_bound; 

				} else if (block_base <= bound_base && bound_lim <= block_lim) {
					// => Related block has order too large, continue splitting
					buddy_split_block(&block_base, order); // [UNSAFE] mut-immut conflict on _free_areas
					goto find_block_for_bound;             // Therefore sacrifice performance through rerun

				} else {
					// => Unrelated block
					block_base = block_base->next_free; 
				}
			}
		}
		__unreachable();
    } 
#pragma endregion buddy_subset
	
#pragma region fib_subset
	/* 
	 * Base pointer for the page descriptor table for the fib subset. 
	 */
	PageDescriptor* _fib_pgds_base = _pgds_base + _buddy_pgds_len; 

	/* 
	 * Limit pointer for the page descriptor table for the fib subset. 
	 */
	uintptr_t _fib_pgds_lim = _pgds_lim; 

    /* 
     * Allocated memory for caching the beginning of first pgd for contiguous fibonacci block 
     * allocations. 
     * 
     * Used by the fibonacci buddy allocator. 
     */
    PageDescriptor* _fib_free_areas_base = NULL; 

    /* 
     * Limit pointer for the contiguous fibonacci block allocation cache. 
     */
    uintptr_t _fib_free_areas_lim = 0; 

    /* 
     * Array for indexing into the contiguous array of fibonacci blocks. 
     */
    PageDescriptor** const _fib_free_areas = &_fib_free_areas_base; 

    /* 
     * Length of contiguous memory allocated for _fib_free_areas in number of elements. 
     */
    size_t _fib_free_areas_len; 

    /* 
     * Maximum number of pages needed for _fib_free_areas to be allocated.
	 *
	 * Determined at init time since I don't wanna do dynamic allocation more than once... 
	 * So long as index never exceeds _fib_max_block_size (which is determined dynamically at 
	 * insert time) fib subset should work fine. 
     */
    size_t _fib_free_areas_pgcount; 

    /*
     * Maximum size of block allocable under fibonacci allocator. 
     */
    size_t _fib_max_block_size; 

    /** 
     * Marks a range of pages as available for allocation. 
     * 
     * @attention
     * This function assumes page range to be inserted are all within the subset managed by 
     * fibonacci allocator. 
    */
    void fib_insert_page_range(PageDescriptor* start, uint64_t count) {
        const char* const _FN_IDENT = "[chimera(fib)::fib_insert_page_range]"; 
	
        assert(_fib_free_areas != NULL); 

        PageDescriptor* bound_base = start; 
        PageDescriptor* bound_lim = start + count; 
        mm_log.messagef(
			LogLevel::INFO, 
			"%s Clearing [pgd@0x%x (pfn: 0x%x), pgd@0x%x (pfn: 0x%x)).", 
            _FN_IDENT, 
			bound_base, sys.mm().pgalloc().pgd_to_pfn(bound_base), 
			bound_lim, sys.mm().pgalloc().pgd_to_pfn(bound_lim)
		); 

        while (bound_base != bound_lim) {
            assert(bound_base < bound_lim); 
            PageDescriptor* block_base = bound_base; 
            size_t block_count = __min(fib_helper::count_to_fib_ceil(count), _fib_max_block_size); 
            PageDescriptor* block_lim = block_base + block_count; 

            /* Init block of given order */
            block_base->type = PageDescriptorType::AVAILABLE; 
            size_t idx_of_block_size = fib_helper::fib_to_idx(block_count); 
            if (_fib_free_areas[idx_of_block_size] == NULL) {
                // => First such fib-sized block
                block_base->prev_free = NULL; 
                block_base->next_free = NULL; 
            } else {
                // => Prepend new order block
                block_base->prev_free = NULL; 
                block_base->next_free = _fib_free_areas[idx_of_block_size]; 
                _fib_free_areas[idx_of_block_size]->prev_free = block_base; 
            }
            _fib_free_areas[idx_of_block_size] = block_base; 

            bound_base = block_lim; 
            mm_log.messagef(
                LogLevel::DEBUG, 
                "%s At fib %d, retrieved [pgd@0x%lx (%lx), pgd@0x%lx (%lx)). "
                "%lx pages remaining...", 
                _FN_IDENT, 
                block_count, 
                block_base, sys.mm().pgalloc().pgd_to_pfn(block_base), 
                block_lim, sys.mm().pgalloc().pgd_to_pfn(block_lim), 
                bound_lim - bound_base
            );
        }

        mm_log.messagef(
			LogLevel::INFO, 
			"%s Finished clearance! Dumping state...", 
            _FN_IDENT
		);
		dump_state(); 
    }

    /* Assume fibonacci allocator will not be responsible for block removal */

	fib_helper::PgdPtrPair fib_split_block(PageDescriptor* block_ptr, size_t fib_idx) {
		assert(fib_idx >= 1); 
		assert(block_ptr->type == PageDescriptorType::AVAILABLE); 
		assert(block_ptr->next_free != NULL); 
		
		/* Maintain pgd & _fib_free_areas state */
		if (_fib_free_areas[fib_idx] == block_ptr) {
			_fib_free_areas[fib_idx] = block_ptr->next_free; 
		}
		if (block_ptr->prev_free != NULL) {
			block_ptr->prev_free->next_free = block_ptr->next_free; 
		}
		if (block_ptr->next_free != NULL) {
			block_ptr->next_free->prev_free = block_ptr->prev_free; 
		}
		block_ptr->prev_free = NULL; 
		block_ptr->next_free = NULL; 

		/* Split in lower orders, update new state */
		const size_t   fib_idx_prev_lo = (fib_idx - 2 > fib_idx) ? 0 : fib_idx - 2; 
		const size_t   fib_idx_prev_hi = (fib_idx - 1 > fib_idx) ? 0 : fib_idx - 1; 
		const uint32_t fib_prev_lo     = fib_helper::idx_to_fib(fib_idx_prev_lo); 
		const uint32_t fib_prev_hi     = fib_helper::idx_to_fib(fib_idx_prev_hi); 
		// By default, left is prev_lo block, right is prev_hi block. 
		PageDescriptor* fib_lo = block_ptr; 
		PageDescriptor* fib_hi = block_ptr + fib_prev_lo; 
		// Ensure segmentation
		assert(fib_hi + fib_prev_hi == block_ptr + fib_helper::idx_to_fib(fib_idx)); 

		if (_fib_free_areas[fib_idx_prev_lo] == NULL) {
			// First such sized block
			fib_lo->prev_free = NULL; 
			fib_lo->next_free = NULL; 
		} else {
			// Won't check for segmentation anymore...
			fib_lo->prev_free = NULL; 
			fib_lo->next_free = _fib_free_areas[fib_idx_prev_lo]; 
			_fib_free_areas[fib_idx_prev_lo]->prev_free = fib_lo; 
		}
		_fib_free_areas[fib_idx_prev_lo] = fib_lo; 

		if (_fib_free_areas[fib_idx_prev_hi] == NULL) {
			// First such sized block
			fib_hi->prev_free = NULL; 
			fib_hi->next_free = NULL; 
		} else {
			// Won't check for segmentation anymore...
			fib_hi->prev_free = NULL; 
			fib_hi->next_free = _fib_free_areas[fib_idx_prev_hi]; 
			_fib_free_areas[fib_idx_prev_hi]->prev_free = fib_hi; 
		}
		_fib_free_areas[fib_idx_prev_hi] = fib_hi; 

		return (fib_helper::PgdPtrPair){fib_lo, fib_hi}; 
	}

	PageDescriptor* fib_allocate_pages(int order) {
		const char* const _FN_IDENT = "[chimera(fib)::fib_allocate_pages]"; 

		const size_t pg_count = fib_helper::order_to_fib_ceil(order);
		if (pg_count > _fib_max_block_size) {
			// => Requested block size larger than fib_floor()
			return NULL; 
		}

		const size_t fib_idx  = fib_helper::fib_to_idx(pg_count);  
		find_block: 
		for (size_t i = fib_idx; i <= _fib_max_block_size; i++) {
			if (i == fib_idx && _fib_free_areas[fib_idx] != NULL) {
				// => Exists exact subdivision in _fib_free_areas
				auto allocated = _fib_free_areas[fib_idx]; 

				// Required by kernel
				for (PageDescriptor* p = allocated; p < allocated + pg_count; p++) {
					p->type = PageDescriptorType::AVAILABLE; 
				}

				/* Alter _free_areas state */
				assert(allocated->prev_free == NULL); 
				_fib_free_areas[fib_idx] = allocated->next_free; 
				if (_fib_free_areas[fib_idx] != NULL) _fib_free_areas[pg_count]->prev_free = NULL; 

				/* Keep bookmark of its size--fib, using next_free as limit ptr */ 
				allocated->next_free = allocated + pg_count; 
				
				mm_log.messagef(
					LogLevel::INFO, 
					"%s Allocated block {[pgd@0x%lx (%lx), pgd@0x%lx (%lx)), fib: %d}.", 
					_FN_IDENT, 
					allocated, sys.mm().pgalloc().pgd_to_pfn(allocated), 
					allocated->next_free, sys.mm().pgalloc().pgd_to_pfn(allocated->next_free), 
					pg_count
				); 
				return allocated; 
			} else if (i != fib_idx && _fib_free_areas[fib_idx] != NULL) {
				// => Exists larger subdivision in _fib_free_areas
				fib_split_block(_fib_free_areas[fib_idx], fib_idx);
				goto find_block; 
			}
			// Otherwise continue. 
		}

		// => Cannot allocate given order
		mm_log.messagef(
			LogLevel::ERROR, 
			"%s Cannot allocate contiguous block of size %d.", 
			_FN_IDENT, 
			pg_count
		); 
		return NULL; 
	}

	// [TODO] free_pages

#pragma endregion buddy_subset

public: 
#pragma region api_impl
	// [TODO] lots... 
	// [FIXME] _fib_max_block_size wrt. _fib_free_areas_len alteration

    /**
	 * Returns the friendly name of the allocation algorithm, for debugging and selection purposes.
	 */
    const char* name() const override { return "adv"; }

    /**
	 * Initialises the allocation algorithm.
	 * @return Returns TRUE if the algorithm was successfully initialised, FALSE otherwise.
	 */
    bool init(PageDescriptor* page_descriptors, uint64_t nr_page_descriptors) override {
        _pgds_base = page_descriptors; 
        _pgds_len = nr_page_descriptors; 
		_pgds_lim = (uintptr_t)(_pgds_base + _pgds_len); 

        if (sys.mm().pgalloc().pgd_to_pfn(_pgds_base) != 0) {
			// => _pgds_base somehow has non-0 pfn, which should not happen
			mm_log.messagef(
				LogLevel::FATAL, 
				"[buddy::init] Failed to initialize page descriptor table: "
				"PFN should begin at 0x0, got 0x%lx instead.", 
				sys.mm().pgalloc().pgd_to_pfn(_pgds_base)
			); 
			return false; 
		}

        /* Initialize _fib_free_areas_len */
        uint32_t first = 1; 
        uint32_t second = 1; 
        _fib_free_areas_len = 1; 
        while (second < (_pgds_len / 2)) {
            auto temp = first; 
            first = second; 
            second = temp + second; 
            _fib_free_areas_len++; 
        }
        _fib_max_block_size = first; 
        auto fib_free_areas_bytes = sizeof(PageDescriptor*) * _fib_free_areas_len;
        _fib_free_areas_pgcount = 
            (fib_free_areas_bytes >> __page_bits << __page_bits == fib_free_areas_bytes)
            ? fib_free_areas_bytes >> __page_bits
            : (fib_free_areas_bytes >> __page_bits) + 1; 

        mm_log.messagef(
			LogLevel::INFO, 
			"[buddy::init] Initialized buddy descriptor with %lx pages. Dumping state...", 
			_pgds_len
		); 
		return true; 
    }

    /**
     * Marks a range of pages as unavailable for allocation.
     * @param start A pointer to the first page descriptors to be made unavailable.
     * @param count The number of page descriptors to make unavailable.
     */
    virtual void remove_page_range(PageDescriptor *start, uint64_t count) override {
        // Knowing that kernel only removes once, 
        // Assume that kernel only marks lower memory as unavailable 
        // (which is the case for current InfOS, apparently)
        assert((uintptr_t)(start + count) < _buddy_pgds_lim); 
        remove_buddy_page_range(start, count); 
    }

    /**
     * Marks a range of pages as available for allocation.
     * @param start A pointer to the first page descriptors to be made available.
     * @param count The number of page descriptors to make available.
     */
    virtual void insert_page_range(PageDescriptor *start, uint64_t count) override {
        const char* const _FN_IDENT = "[chimera::insert_page_range]"; 

        if ((uintptr_t)(start + count) < _buddy_pgds_lim) {
            // => [start, start + count) fully in lower memory
            insert_buddy_page_range(start, count); 

            /* Try allocate, which may fail to NULL */
            if (_fib_free_areas == NULL) {
                _fib_free_areas_base = buddy_allocate_pages(__log2ceil(_fib_free_areas_pgcount)); 
            }
            
        } else if ((uintptr_t)start >= _buddy_pgds_lim) {
            // => [start, start + count) fully in upper memory
            /* 
             * Steal a part of upper memory into lower memory to initialize fibonacci allocator, 
             * which may fail to NULL
             */
            size_t steal_count = __min(_fib_free_areas_len, count); 
            if (_fib_free_areas == NULL) {
                PageDescriptor* steal_start = start; 
                insert_buddy_page_range(steal_start, steal_count); 
                _buddy_pgds_len += steal_count; 
				_buddy_pgds_lim = sizeof(PageDescriptor*) * _buddy_pgds_len; 

                _fib_free_areas_base = buddy_allocate_pages(__log2ceil(_fib_free_areas_pgcount)); 
            }

            if (_fib_free_areas != NULL) {
                // => Stolen upper memory sufficient for allocating _fib_free_areas
                start += steal_count; 
                count -= steal_count; 
                fib_insert_page_range(start, count); 
            }
            // => Otherwise __min(_fib_free_areas_len, count) == count, 
            //    hence all are cleared to buddy. 

        } else {
            // => Split [start, start + count) into 
            //    lower: [start, _buddy_pgds_len), 
            //    upper: [_buddy_pgds_len, start + count)
            PageDescriptor* half = start + _buddy_pgds_len; 
            size_t count_until_half = half - start; 
            size_t count_after_half = count - count_until_half; 
            insert_buddy_page_range(start, count_until_half); 

            /* Try allocate in lower memory */
            if (_fib_free_areas == NULL) {
                _fib_free_areas_base = buddy_allocate_pages(__log2ceil(_fib_free_areas_pgcount)); 
            }

            if (_fib_free_areas != NULL) {
				// => Allocation successful! Insert rest for fib allocator
                fib_insert_page_range(half, count_after_half); 
            } else {
                // => Fallback to try steal upper memory -- reentry
                insert_page_range(half, count_after_half); 
            }

        }

        if (_fib_free_areas == NULL) {
            mm_log.messagef(
                LogLevel::ERROR, 
                "%s Cannot allocate _fib_free_areas in given kernel clearance: "
                "need %lx pages but cleared only %lx pages.", 
                _FN_IDENT, 
                _fib_free_areas_pgcount, count
            ); 
        } else {
			// _fib_max_block_size = 
            _fib_free_areas_lim = (uintptr_t)(_fib_free_areas_base + _fib_free_areas_len); 
            mm_log.messagef(
                LogLevel::INFO, 
                "%s Initialized fibonacci buddy suballocator.", 
                _FN_IDENT
            ); 
        }
    }
#pragma endregion api_impl
}; 

// RegisterPageAllocator(ChimeraPageAllocator); 
