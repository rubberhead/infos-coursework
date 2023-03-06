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
 * Checks if `elem_ptr` is in an contiguous "array" -- delineated by `base_ptr` and `len`. 
 */
template<typename T>
inline const bool __in_ptr_bound(
	const T* elem_ptr, 
	const T* base_ptr, 
	const uint64_t len
) {
	const size_t alignment = sizeof(T); 
	return base_ptr <= elem_ptr && elem_ptr < base_ptr + len * alignment; 
}

/**
 * Checks if `block_left_ptr` is aligned to given order wrt `base_ptr[len]`. 
 * 
 * @attention
 * Does not guarantee that `block_left_ptr` is in between `base_ptr` and `base_ptr + len`. 
 */
template<typename T>
inline const bool __is_aligned_by_order(
	const T* block_left_ptr, 
	const T* base_ptr, 
	const uint64_t len, 
	const uint64_t order
) {
	// assert(__in_ptr_bound(block_left_ptr, base_ptr, len)); 
	const size_t pgds_in_block = TWO_POW(order); 
	const size_t alignment = sizeof(T) * pgds_in_block;
	return (block_left_ptr - base_ptr) % alignment == 0; 
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
	const size_t alignment = sizeof(T) << order; 
	T* result_ptr (block_ptr - alignment); 
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
	const size_t alignment = sizeof(T) << order; 
	T* result_ptr (block_ptr + alignment); 
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
	PageDescriptor *_free_areas[MAX_ORDER+1];

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
		PageDescriptor* aligned_pgd; 
		if (pfn_of_pgd % TWO_POW(order)) {
			aligned_pgd = pgd; 
		} else {
			aligned_pgd = (PageDescriptor*)((uintptr_t)pgd >> order << order); 
			mm_log.messagef(
				LogLevel::ERROR, 
				"[buddy::buddy_of] Misaligned pgd @0x%x for given order %d: "
				"fixed to closest aligned page @0x%x.", 
				pgd, 
				order, 
				aligned_pgd
			); 
		}

		const size_t pfn_of_aligned_pgd = sys.mm().pgalloc().pgd_to_pfn(aligned_pgd); 
		if (pfn_of_aligned_pgd / TWO_POW(order) % 2) {
			// => odd "block idx", return previous block
			return __prev_block_ptr(aligned_pgd, _pgds_base, _pgds_len, order); 
		} else {
			// => even "block idx", return next block
			return __next_block_ptr(aligned_pgd, _pgds_base, _pgds_len, order); 
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
        // TODO: Implement me!
		PageDescriptor* block_ptr = *block_pointer; 
		assert(block_ptr->type != PageDescriptorType::INVALID); 
		assert(
			block_ptr->next_free == NULL || 
			block_ptr->next_free - block_ptr >= sizeof(PageDescriptor) << source_order
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
		size_t tgt_order = source_order - 1; 
		PageDescriptor* half_left = block_ptr; 
		PageDescriptor* half_right = buddy_of(half_left, tgt_order); 
		assert(__min((uintptr_t)half_left, (uintptr_t)half_right) == (uintptr_t)half_left); 

		assert(half_left->type != PageDescriptorType::INVALID); 
		half_left->type = PageDescriptorType::AVAILABLE; 

		assert(half_right->type != PageDescriptorType::INVALID); 
		half_right->type = PageDescriptorType::AVAILABLE; 

		half_left->next_free = half_right; 
		half_right->prev_free = half_left; 

		size_t tgt_alignment = sizeof(PageDescriptor) << tgt_order;
		if (_free_areas[tgt_order] == NULL) {
			// => First time splitting into tgt_order
			half_left->prev_free = NULL; 
			half_right->next_free = NULL; 
			_free_areas[tgt_order] = half_left; 

		} else if (_free_areas[tgt_order] >= half_right + tgt_alignment) {
			// => Both blocks more top than current tgt_order entry, prioritize. 
			/* Ensure no block segmentation faults & invariant */ 
			assert(_free_areas[tgt_order]->prev_free == NULL); 

			/* Alter states */
			half_left->prev_free = NULL; 
			half_right->next_free = _free_areas[tgt_order]; 
			_free_areas[tgt_order]->prev_free = half_right; 
			_free_areas[tgt_order] = half_left; 

		} else if (_free_areas[tgt_order] + tgt_alignment <= half_left) {
			// => Both blocks more bottom than current tgt_order entry, insert-on-right. 
			/* [NOTE]
			 * We could either iterate over the linked pgds to find the correct placement for top 
			 * memory priority or insert it as we wish (e.g., FIFO). 
			 * 
			 * The former effectively makes the allocator O(n), but the latter causes excessive 
			 * external fragmentation. 
			 * 
			 * Think this could either be solved using O(1) heaps or "de-fragmentation" routines?
			 * For now, just append at back.  
			 */

			/* Ensure no block segmentation faults */
			

		}

		
		return NULL; 
	}

	/**
	 * Takes a block in the given source order, and merges it (and its buddy) into the next order.
	 * @param block_pointer A pointer to a pointer containing a block in the pair to merge.
	 * @param source_order The order in which the pair of blocks live.
	 * @return Returns the new slot that points to the merged block.
	 */
	PageDescriptor **merge_block(PageDescriptor **block_pointer, int source_order)
	{
        // TODO: Implement me!
		return NULL; 
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
        // TODO: Implement me!
		if (_free_areas[order] != NULL) {
			// => Exists subdivision in _free_areas
			auto allocated = _free_areas[order]; 
			assert(allocated->prev_free == NULL); 
			allocated->type = PageDescriptorType::ALLOCATED; 
			_free_areas[order] = allocated->next_free; 

			allocated->next_free = NULL; 
			_free_areas[order]->prev_free = NULL; 

			return allocated; 
		}
		// => Otherwise 2 branches: 
		// 1. Try to split larger subdivisions until exists correct order, then return
		// 2. No larger subdivisions exist => not enough memory, return NULL. 


		return NULL; 
	}

    /**
	 * Frees 2^order contiguous pages.
	 * @param pgd A pointer to an array of page descriptors to be freed.
	 * @param order The power of two number of contiguous pages to free.
	 */
    void free_pages(PageDescriptor *pgd, int order) override
    {
        // TODO: Implement me!
    }

    /**
     * Marks a range of pages as available for allocation.
     * @param start A pointer to the first page descriptors to be made available.
     * @param count The number of page descriptors to make available.
     */
    virtual void insert_page_range(PageDescriptor *start, uint64_t count) override
    {
        // TODO: Implement me!
    }

    /**
     * Marks a range of pages as unavailable for allocation.
     * @param start A pointer to the first page descriptors to be made unavailable.
     * @param count The number of page descriptors to make unavailable.
     */
    virtual void remove_page_range(PageDescriptor *start, uint64_t count) override
    {
        // TODO: Implement me!
    }

	/**
	 * Initialises the allocation algorithm.
	 * @return Returns TRUE if the algorithm was successfully initialised, FALSE otherwise.
	 */
	bool init(PageDescriptor *page_descriptors, uint64_t nr_page_descriptors) override
	{
		/* 1. Initialize class fields */
        PageDescriptor* _pgds_base (page_descriptors); 
		uint64_t _pgds_len (nr_page_descriptors); 
		uintptr_t _pgds_lim ((uintptr_t)_pgds_base + sizeof(PageDescriptor) * _pgds_len); 
		PageDescriptor* _free_areas[MAX_ORDER + 1] { NULL }; 

		if (sys.mm().pgalloc().pgd_to_pfn(_pgds_base) != 0) {
			// => _pgds_base somehow has non-0 pfn, which should not happen
			mm_log.messagef(
				LogLevel::FATAL, 
				"[buddy::init] Failed to initialize page descriptor table: "
				"PFN should begin at 0, got %d instead.", 
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
				curr_block_start->prev_free = __prev_block_ptr(curr_block_start, curr_base, curr_len, order); 
				curr_block_start->next_free = __next_block_ptr(curr_block_start, curr_base, curr_len, order); 
				curr_block_start->type = PageDescriptorType::AVAILABLE; 
				if (_free_areas[order] == NULL) {
					_free_areas[order] = curr_block_start; 
					mm_log.messagef(
						LogLevel::DEBUG, 
						"[buddy::init] Initialized _free_area[%d] to pgd@0x%x", 
						order, 
						_free_areas[order]
					); 
				}

				// [UNSAFE]
				curr_block_start = (PageDescriptor*)next_block_start_raw; 
			}
		}

		mm_log.messagef(
			LogLevel::INFO, 
			"[buddy::init] Initialized buddy descriptor with %d pages. Dumping current state...", 
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