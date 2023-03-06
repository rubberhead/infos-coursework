/*
 * The Buddy Page Allocator
 * SKELETON IMPLEMENTATION TO BE FILLED IN FOR TASK 2
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

#define MAX_ORDER	18
#define TWO_POW(order) 1ull << order // Overflow? Never heard of it. 

/**
 * Preprocessor defines doesn't bide well with pointer arithmetics. 
 * This is a wrapper for TWO_POW.
 */
inline constexpr uint64_t __two_pow(uint32_t order) {
	return TWO_POW(order); 
}

/**
 * Checks if `elem_ptr` is in an contiguous "array" -- delineated by `base_ptr` and `len`. 
 */
template<typename T>
inline const bool __in_ptr_bound(
	const T* elem_ptr, 
	const T* base_ptr, 
	const uint64_t len
) {
	return base_ptr <= elem_ptr && elem_ptr < base_ptr + len; 
}

/**
 * Finds the index of `*elem_ptr` in the contiguous "array" between `base_ptr[0]` and `base_ptr[len]`. 
 * 
 * @attention
 * Assumes that `elem_ptr` is in the contiguous "array" delineated by `base_ptr` and `len`. 
 */
template<typename T>
inline const size_t __idx_of(
	const T* elem_ptr, 
	const T* base_ptr, 
	const uint64_t len
) {
	const size_t alignment = sizeof(T); 
	assert(__in_ptr_bound(elem_ptr, base_ptr, len)); 
	return ((uintptr_t)elem_ptr - (uintptr_t)base_ptr) / alignment; 
}

/**
 * Finds the pointer to previous 2^order-block given `block_ptr` wrt `base_ptr[len]`. 
 * 
 * @returns block_ptr - alignment, or NULL if out of bounds
 */
template<typename T>
inline T* __prev_block_ptr(
	T* const block_ptr, 
	T* const base_ptr, 
	const uint64_t len, 
	const uint64_t order
) {
	// assert(__is_aligned_by_order(block_ptr, base_ptr, len, order)); 
	const size_t alignment = TWO_POW(order); 
	T* result_ptr = block_ptr - alignment; 
	if (!__in_ptr_bound(result_ptr, base_ptr, len)) {
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
inline T* __next_block_ptr(
	T* const block_ptr, 
	T* const base_ptr, 
	const uint64_t len, 
	const uint64_t order
) {
	// assert(__is_aligned_by_order(block_ptr, base_ptr, len, order)); 
	const size_t alignment = TWO_POW(order); 
	T* result_ptr = block_ptr + alignment; 
	if (!__in_ptr_bound(result_ptr, base_ptr, len)) {
		return NULL; 
	} else {
		return result_ptr; 
	}
}

/**
 * A buddy page allocation algorithm.
 */
class BuddyPageAllocator : public PageAllocatorAlgorithm
{
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
	 * [DANGER] NEVER dereference -- segfault coredump otherwise. 
	 */
	uintptr_t _pgds_lim; 

	/*
	 * Array of page descriptor pointers for caching the beginning of first pgd for contiguous 
	 * 2^order page cluster allocations. 
	 */
	PageDescriptor *_free_areas[MAX_ORDER+1] { NULL };


	/** Given a page descriptor, and an order, returns the buddy PGD.  The buddy could either be
	 * to the left or the right of PGD, in the given order.
	 * @param pgd The page descriptor to find the buddy for.
	 * @param order The order in which the page descriptor lives.
	 * @return Returns the buddy of the given page descriptor, in the given order, or NULL if no 
	 * buddy exists (e.g., odd-divided pgd table). 
	 */
	PageDescriptor *buddy_of(PageDescriptor *pgd, int order)
	{
		const size_t pfn_of_pgd = sys.mm().pgalloc().pgd_to_pfn(pgd); 
		mm_log.messagef(LogLevel::DEBUG, "Checking pfn %x -- block %d, order %d", pfn_of_pgd, pfn_of_pgd >> order, order);
		if ((pfn_of_pgd >> order) % 2) {
			// => odd "block idx", return previous block
			return __prev_block_ptr(pgd, _pgds_base, _pgds_len, order); 
		} else {
			// => even "block idx", return next block
			return __next_block_ptr(pgd, _pgds_base, _pgds_len, order); 
		}
	}

	/**
	 * Given a pointer to a block of free memory in the order "source_order", this function will
	 * split the block in half, and insert it into the order below.
	 * @param block_pointer A pointer to a pointer containing the beginning of a block of free memory.
	 * @param source_order The order in which the block of free memory exists.  Naturally,
	 * the split will insert the two new blocks into the order below.
	 * @return Returns the left-hand-side of the new block.
	 */
	PageDescriptor *split_block(PageDescriptor **block_pointer, int source_order)
	{
		PageDescriptor* block_ptr = *block_pointer; 
		assert(
			block_ptr->next_free == NULL || 
			block_ptr->next_free - block_ptr >= TWO_POW(source_order)
		);

		/* Maintain pgd & _free_areas state */
		if (_free_areas[source_order] == block_ptr) {
			_free_areas[source_order] = block_ptr->next_free; 
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
			"[buddy::split_block] Halves: L@0x%lx, R@0x%lx. Order: %d->%d", 
			half_left, half_right,
			source_order, tgt_order
		); 
		assert(__min((uintptr_t)half_left, (uintptr_t)half_right) == (uintptr_t)half_left); 

		assert(half_left->type != PageDescriptorType::INVALID); 
		half_left->type = PageDescriptorType::AVAILABLE; 

		assert(half_right->type != PageDescriptorType::INVALID); 
		half_right->type = PageDescriptorType::AVAILABLE; 

		half_left->next_free = half_right; 
		half_right->prev_free = half_left; 

		size_t tgt_alignment = TWO_POW(tgt_order);
		if (_free_areas[tgt_order] == NULL) {
			// => First time splitting into tgt_order
			half_left->prev_free = NULL; 
			half_right->next_free = NULL; 
			_free_areas[tgt_order] = half_left; 

		} else if (_free_areas[tgt_order] >= half_right + tgt_alignment) {
			// => Both blocks more top than current tgt_order entry, prioritize. 
			/* Ensure no block segmentation faults & invariant */ 
			assert(_free_areas[tgt_order]->prev_free == NULL); 

			/* Alter linkage */
			half_left->prev_free = NULL; 
			half_right->next_free = _free_areas[tgt_order]; 
			_free_areas[tgt_order]->prev_free = half_right; 
			_free_areas[tgt_order] = half_left; 

		} else if (_free_areas[tgt_order] + tgt_alignment <= half_left) {
			// => Both blocks more bottom than current tgt_order entry, insert-on-right
			// At least this is the intended behavior...

			/* [NOTE]
			 * We could either iterate over the linked pgds to find the correct placement for top 
			 * memory priority or insert it as we wish (e.g., FILO). 
			 * 
			 * The former effectively makes the allocator O(#pfnlg(s)) -> O(lg(#pfn)*lg(s)) where 
			 * s is size of allocation, #pfn is number of pages...
			 * But the latter causes excessive external fragmentation! 
			 * 
			 * Think this could either be solved using O(1) heaps or "de-fragmentation" routines? 
			 * Memory use for O(1) heap would be O(nlg(n)) methinks. 
			 * 
			 * There are also techniques for improving linked list sorted insertion to O(lg(n)) time, 
			 * but for now, do the same thing as above. 
			 */

			/* Ensure no block segmentation faults & invariant */
			assert(
				_free_areas[tgt_order]->next_free == NULL || 
				half_right + tgt_alignment <= _free_areas[tgt_order]->next_free 
			); 

			/* Alter linkage */
			half_left->prev_free = NULL; 
			half_right->next_free = _free_areas[tgt_order]; 
			_free_areas[tgt_order]->prev_free = half_right; 
			_free_areas[tgt_order] = half_left; 

		} else {
			// => Segmentation fault
			mm_log.message(
				LogLevel::FATAL, 
				"[buddy::split_block] Block segmentation fault. Crashed!"
			); 
			mm_log.messagef(
				LogLevel::DEBUG, 
				"[buddy::split_block] Attempted to split {pgd@0x%lx, order: %d} to "
				"({pgd@0x%lx, order: %d}, {pgd@0x%lx, order: %d}) and insert wrt "
				"({pgd@0x%lx, order: %d} -> {pgd@0x%lx, order: %d}), but got segmentation fault.", 
				block_ptr, 
				source_order, 
				half_left, 
				tgt_order, 
				half_right, 
				tgt_alignment, 
				_free_areas[tgt_order], 
				tgt_order, 
				_free_areas[tgt_order]->next_free, 
				(_free_areas[tgt_order]->next_free == NULL) ? -1 : tgt_order
			); 
			assert(false); 
		}

		mm_log.messagef(
			LogLevel::DEBUG, 
			"[buddy::split_block] Split {pgd@0x%lx, order: %d} to "
			"({pgd@0x%lx, order: %d}, {pgd@0x%lx, order: %d}).", 
			block_ptr, source_order, 
			half_left, tgt_order, 
			half_right, tgt_order
		); 
		return half_left; 
	}

	/**
	 * Takes a block in the given source order, and merges it (and its buddy) into the next order.
	 * @param block_pointer A pointer to a pointer containing a block in the pair to merge.
	 * @param source_order The order in which the pair of blocks live.
	 * @return Returns the new slot that points to the merged block, or NULL if merge failed. 
	 */
	PageDescriptor **merge_block(PageDescriptor **block_pointer, int source_order)
	{
		assert(0 <= source_order && source_order < MAX_ORDER); 

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
				"[buddy::merge_block] Cannot merge ({pgd@0x%lx, order: %d}, {pgd@0x%lx, order: %d}) "
				"because one of the buddy blocks is not marked as AVAILABLE.", 
				src_l_block, source_order, 
				src_r_block, source_order
			); 
			return NULL; 
		}
		// => L, R mergeable -- buddy free.

		/* Assert correct internal linkage */
		assert(src_l_block->next_free == src_r_block && src_r_block->prev_free == src_l_block); 

		/* Alter & delete external linkage */
		if (_free_areas[source_order] != src_l_block) {
			src_l_block->prev_free->next_free = src_r_block->next_free; 
		}
		if (src_r_block->next_free != NULL) {
			src_r_block->next_free->prev_free = src_l_block->prev_free; 
		}
		_free_areas[source_order] = src_r_block->next_free; 
		src_l_block->prev_free = NULL; 
		src_l_block->next_free = NULL; 
		src_r_block->next_free = NULL; 
		src_r_block->prev_free = NULL; 

		/* Merge and alter external linkage */
		PageDescriptor* tgt_block (src_l_block); 
		size_t tgt_order = source_order + 1; 
		size_t alignment = TWO_POW(tgt_order); 
		
		/* Much like cases in split_block... */
		if (_free_areas[tgt_order] == NULL) {
			_free_areas[tgt_order] = tgt_block; 
		} else if (_free_areas[tgt_order] + alignment <= tgt_block) {
			assert(
				_free_areas[tgt_order]->next_free == NULL ||
				tgt_block + alignment <= _free_areas[tgt_order]->next_free
			); 

			tgt_block->next_free = _free_areas[tgt_order]; 
			_free_areas[tgt_order]->prev_free = tgt_block; 
			_free_areas[tgt_order] = tgt_block; 
		} else if (tgt_block + alignment <= _free_areas[tgt_order]) {
			assert(_free_areas[tgt_order]->prev_free == NULL); 

			tgt_block->next_free = _free_areas[tgt_order]; 
			_free_areas[tgt_order]->prev_free = tgt_block; 
			_free_areas[tgt_order] = tgt_block; 			
		} else {
			// => Segmentation fault
			mm_log.message(
				LogLevel::FATAL, 
				"[buddy::merge_block] Block segmentation fault. Crashed!"
			); 
			mm_log.messagef(
				LogLevel::DEBUG, 
				"[buddy::merge_block] Attempted to merge ({pgd@0x%lx, order: %d}, {pgd@0x%lx, order: %d}) to "
				"{pgd@0x%lx, order: %d} and insert wrt "
				"({pgd@0x%lx, order: %d} -> {pgd@0x%lx, order: %d}), but got segmentation fault.", 
				src_l_block, source_order, 
				src_r_block, source_order, 
				tgt_block, tgt_order, 
				_free_areas[tgt_order], tgt_order, 
				_free_areas[tgt_order]->next_free, 
				(_free_areas[tgt_order] == NULL) ? -1 : tgt_order
			); 
			assert(false); 
		}

		mm_log.messagef(
			LogLevel::DEBUG, 
			"[buddy::split_block] Merged ({pgd@0x%lx, order: %d}, {pgd@0x%lx, order: %d}) to "
			"{pgd@0x%lx, order: %d}.", 
			src_l_block, source_order, 
			src_r_block, source_order, 
			tgt_block, tgt_order
		); 
		return &_free_areas[tgt_order]; 
	}

public:
	/**
	 * Allocates 2^order number of contiguous pages
	 * @param order The power of two, of the number of contiguous pages to allocate.
	 * @return Returns a pointer to the first page descriptor for the newly allocated page range, or 
	 * NULL if allocation failed.
	 */
	PageDescriptor *allocate_pages(int order) override
	{
		if (_free_areas[order] != NULL) {
			// => Exists subdivision in _free_areas
			auto allocated = _free_areas[order];

			// Costly, but required by kernel -- otherwise won't pass assertion
			PageDescriptor* pg_from_allocated (allocated);
			for (uintptr_t _ = 0; _ < TWO_POW(order); _++) {
				// [Spec?] Check if pg_from_allocated is of a allocable type? 
				pg_from_allocated->type = PageDescriptorType::AVAILABLE; 
				pg_from_allocated++; 
			}

			assert(allocated->prev_free == NULL); 
			_free_areas[order] = allocated->next_free; 

			// allocated->next_free = NULL; 
			_free_areas[order]->prev_free = NULL; 

			mm_log.messagef(
				LogLevel::INFO, 
				"[buddy::allocate_pages] Found allocable block {pgd@0x%lx, order: %d}.", 
				allocated, 
				order
			); 
			dump_state(); 
			return allocated; 
		}
		// => Otherwise 2 branches: 
		size_t from_order = -1; 
		for (size_t o = order + 1; o <= MAX_ORDER; o++) {
			if (_free_areas[o] != NULL) {
				from_order = o; 
				break; 
			}
		}
		// 1. No larger subdivisions exist => not enough contiguous memory, return NULL. 
		if (from_order == -1) {
			mm_log.messagef(
				LogLevel::ERROR, 
				"[buddy::allocate_pages] Cannot allocate contiguous memory of order %d -- 0x%lx pages", 
				order, 
				TWO_POW(order)
			); 
			return NULL; 
		}

		// 2. Try to split larger subdivisions until exists correct order, then return 
		mm_log.messagef(
			LogLevel::DEBUG, 
			"[buddy::allocate_pages] Splitting {pgd@0x%lx, order: %d}...", 
			_free_areas[from_order], 
			from_order
		); 
		split_block(&_free_areas[from_order], from_order); 
		return allocate_pages(order); 
	}

    /**
	 * Frees 2^order contiguous pages.
	 * @param pgd A pointer to an array of page descriptors to be freed.
	 * @param order The power of two number of contiguous pages to free.
	 */
    void free_pages(PageDescriptor *pgd, int order) override
    {
		uintptr_t pgd_next = (pgd->next_free == NULL) ? _pgds_lim : (uintptr_t)pgd->next_free;
		size_t pgd_order = -1; 
		for (size_t o = 0; o <= MAX_ORDER; o++) {
			if ((uintptr_t)pgd + TWO_POW(o) * sizeof(PageDescriptor) == pgd_next) {
				pgd_order = o; 
				break; 
			}
		}

		if (pgd_order == order) {
			PageDescriptor* freed = pgd; 

			PageDescriptor* pg_from_freed (freed);
			for (uintptr_t _ = 0; _ < TWO_POW(order); _++) {
				pg_from_freed->type = PageDescriptorType::AVAILABLE; 
				pg_from_freed++; 
			}

			freed->prev_free = NULL; 
			if (_free_areas[order] != NULL) _free_areas[order]->prev_free = freed; 
			freed->next_free = _free_areas[order]; 

			mm_log.messagef(
				LogLevel::INFO, 
				"[buddy::free_pages] Freed block {pgd@0x%lx, order: %d}.", 
				freed, 
				order
			); 
			dump_state(); 
		}
    }

    /**
     * Marks a range of pages as available for allocation.
     * @param start A pointer to the first page descriptors to be made available.
     * @param count The number of page descriptors to make available.
     */
    virtual void insert_page_range(PageDescriptor *start, uint64_t count) override
    {
        // TODO: Implement me!
		mm_log.messagef(
			LogLevel::INFO, 
			"[buddy::insert_page_range] Clearing [pgd@0x%x (pfn: 0x%x), pgd@0x%x (pfn: 0x%x)).", 
			start, sys.mm().pgalloc().pgd_to_pfn(start), 
			start + count, sys.mm().pgalloc().pgd_to_pfn(start + count)
		);
		for (size_t _ = 0; _ < count; _++) {
			start->type = PageDescriptorType::AVAILABLE; 
			start++;
		}
		
    }

	/*
	 * Helper function.
	 * 
	 * Alters a block of given order to RESERVED status and deletes it from _free_areas[order] LL. 
	 * This function performs no check for memory safety whatsoever. Use with caution.
	 */
	void _unsafe_reserve_block(PageDescriptor* block_base, size_t order) {
		PageDescriptor* block_lim = block_base + __two_pow(order); 

		if (block_base->prev_free == NULL) {
			// => should be _free_areas[order]
			assert(_free_areas[order] == block_base); 
			_free_areas[order] = block_base->next_free; 
			if (block_base->next_free != NULL) block_base->next_free->prev_free = NULL; 
			block_base->next_free = NULL; 
		} else {
			// => otherwise...
			block_base->prev_free->next_free = block_base->next_free; 
			if (block_base->next_free != NULL) {
				block_base->next_free->prev_free = block_base->prev_free; 
			}
			block_base->prev_free = NULL;
			block_base->next_free = NULL; 
		}

		for (; block_base < block_lim; block_base++) {
			block_base->type = PageDescriptorType::RESERVED; 
		}
	}

    /**
     * Marks a range of pages as unavailable for allocation.
     * @param start A pointer to the first page descriptors to be made unavailable.
     * @param count The number of page descriptors to make unavailable.
     */
    virtual void remove_page_range(PageDescriptor *start, uint64_t count) override
    {
		assert(((PageDescriptor*)((uintptr_t)0xffffffff83145000))->next_free == NULL); // WTH?!

        PageDescriptor* bound_base = start; 
		PageDescriptor* bound_lim = start + count; 
		mm_log.messagef(
			LogLevel::INFO, 
			"[buddy::remove_page_range] Reserving [{pgd@0x%lx (%x)}, {pgd@0x%lx} (%x)).", 
			bound_base, sys.mm().pgalloc().pgd_to_pfn(bound_base), 
			bound_lim, sys.mm().pgalloc().pgd_to_pfn(bound_lim)
		); 

		find_block_for_bound: 
		dump_state();
		for (size_t order = MAX_ORDER; order >= 0; order--) {
			PageDescriptor* block_base = _free_areas[order]; 

			while (block_base != NULL) {
				PageDescriptor* block_lim = block_base + __two_pow(order); 
				mm_log.messagef(
					LogLevel::INFO, 
					"[buddy::remove_page_range] Checking [{pgd@0x%lx (%x)}, {pgd@0x%lx} (%x)) "
					"against [{pgd@0x%lx (%x)}, {pgd@0x%lx} (%x))", 
					block_base, sys.mm().pgalloc().pgd_to_pfn(block_base), 
					block_lim, sys.mm().pgalloc().pgd_to_pfn(block_lim), 
					bound_base, sys.mm().pgalloc().pgd_to_pfn(bound_base), 
					bound_lim, sys.mm().pgalloc().pgd_to_pfn(bound_lim)
				);

				if (block_base == bound_base && block_lim == bound_lim) {
					// => [bound_base, bound_lim) aligned by buddy-ness, can fill into single block
					mm_log.messagef(
						LogLevel::INFO, 
						"[buddy::remove_page_range] Block@0x%lx fits.", 
						block_base
					); 
					_unsafe_reserve_block(block_base, order);
					return; 

				} else if (block_base == bound_base && block_lim < bound_lim) {
					// => [bound_base, bound_lim) left aligned by buddy-ness only, larger
					//mm_log.messagef(LogLevel::INFO, "[buddy::remove_page_range] Block@0x%lx takes left.", block_base); 
					_unsafe_reserve_block(block_base, order); 
					bound_base = block_lim; 
					goto find_block_for_bound; 

				} else if (bound_base < block_base && block_lim == bound_lim) {
					// => [bound_base, bound_lim) right aligned by buddy-ness only, larger
					//mm_log.messagef(LogLevel::INFO, "[buddy::remove_page_range] Block@0x%lx takes right.", block_base); 
					_unsafe_reserve_block(block_base, order); 
					bound_lim = block_base; 
					goto find_block_for_bound; 

				} else if (block_base <= bound_base && bound_lim <= block_lim) {
					// => Related block has order too large, continue splitting
					//mm_log.messagef(LogLevel::INFO, "[buddy::remove_page_range] Block@0x%lx too large.", block_base); 
					split_block(&block_base, order); // [UNSAFE] mut-immut conflict on _free_areas
					goto find_block_for_bound; // Therefore sacrifice performance through reentrant code...

				} else {
					// => Unrelated block
					//mm_log.messagef(LogLevel::INFO, "[buddy::remove_page_range] Block@0x%lx unrelated.", block_base); 
					block_base = block_base->next_free; 
					// if (block_base == NULL) break; 
				}
			}
		}

		unreachable: 
		__unreachable();
    }

	/**
	 * Initialises the allocation algorithm.
	 * @return Returns TRUE if the algorithm was successfully initialised, FALSE otherwise.
	 */
	bool init(PageDescriptor *page_descriptors, uint64_t nr_page_descriptors) override
	{
		/* 1. Initialize class fields */
        _pgds_base = page_descriptors; 
		_pgds_len = nr_page_descriptors;  
		_pgds_lim = (uintptr_t)_pgds_base + sizeof(PageDescriptor) * _pgds_len; 

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

		/* 2. Initialize pgd table by largest blocks. */
		size_t order = MAX_ORDER; 
		PageDescriptor* curr_block_start = _pgds_base; 
		PageDescriptor* curr_base = _pgds_base; 
		size_t curr_len = _pgds_len; 
		while ((uintptr_t)curr_block_start != _pgds_lim) {
			/* Suppose we have infinite memory. This is the next block boundary for this order */
			size_t alignment = sizeof(PageDescriptor) << order; 
			uintptr_t next_block_start_raw = (uintptr_t)curr_block_start + alignment; 

			if (next_block_start_raw > _pgds_lim) {
				// => Maybe can still initialize allocation with some reduced order...
				// At order == 0, next_block_start_raw should just be ptr + 1, which matches. 
				order--; 
				curr_len -= (curr_block_start - curr_base) / sizeof(PageDescriptor); 
				curr_base = curr_block_start; 
			} else {
				// => Otherwise initialize block at current order. 
				curr_block_start->prev_free = 
					__prev_block_ptr(curr_block_start, curr_base, curr_len, order); 
				curr_block_start->next_free = 
					__next_block_ptr(curr_block_start, curr_base, curr_len, order); 
				assert(
					curr_block_start->next_free == NULL || 
					(uintptr_t)curr_block_start->next_free == next_block_start_raw
				); 

				curr_block_start->type = PageDescriptorType::AVAILABLE; 
				if (_free_areas[order] == NULL) {
					_free_areas[order] = curr_block_start; 
					mm_log.messagef(
						LogLevel::DEBUG, 
						"[buddy::init] Initialized _free_area[%d] to pgd@0x%lx", 
						order, 
						_free_areas[order]
					); 
				}
				mm_log.messagef(
					LogLevel::DEBUG, 
					"[buddy::init] Initialized {pgd@0x%lx (%lx), order: %d, prev@0x%lx (%lx), next@0x%lx (%lx)}.", 
					curr_block_start, sys.mm().pgalloc().pgd_to_pfn(curr_block_start), 
					order, 
					curr_block_start->prev_free, sys.mm().pgalloc().pgd_to_pfn(curr_block_start->prev_free),
					curr_block_start->next_free, sys.mm().pgalloc().pgd_to_pfn(curr_block_start->next_free)
				); 

				// [UNSAFE]
				curr_block_start = (PageDescriptor*)next_block_start_raw; 
			}
		}

		mm_log.messagef(
			LogLevel::INFO, 
			"[buddy::init] Initialized buddy descriptor with %lx pages. Dumping current state...", 
			_pgds_len
		); 
		dump_state(); 
		return true; 
	}

	/**
	 * Returns the friendly name of the allocation algorithm, for debugging and selection purposes.
	 */
	const char* name() const override { return "buddy"; }

	/**
	 * Dumps out the current state of the buddy system
	 */
	void dump_state() const override
	{
		// Print out a header, so we can find the output in the logs.
		mm_log.messagef(LogLevel::DEBUG, "BUDDY STATE:");

		// Iterate over each free area.
		for (unsigned int i = 0; i < ARRAY_SIZE(_free_areas); i++) {
			char buffer[256];
			snprintf(buffer, sizeof(buffer), "[%d] ", i);

			// Iterate over each block in the free area.
			PageDescriptor *pg = _free_areas[i];
			while (pg) {
				// Append the PFN of the free block to the output buffer.
				snprintf(buffer, sizeof(buffer), "%s%lx ", buffer, sys.mm().pgalloc().pgd_to_pfn(pg));
				pg = pg->next_free;
			}

			mm_log.messagef(LogLevel::DEBUG, "%s", buffer);
		}
	}
};

/* --- DO NOT CHANGE ANYTHING BELOW THIS LINE --- */

/*
 * Allocation algorithm registration framework
 */
RegisterPageAllocator(BuddyPageAllocator);