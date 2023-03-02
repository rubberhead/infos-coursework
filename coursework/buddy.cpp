/*
 * The Buddy Page Allocator
 * SKELETON IMPLEMENTATION TO BE FILLED IN FOR TASK 2
 * ============================================================================
 * Ongoing...
 * 
 * B171926
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
#define TWO_POW(order) 1ull << order // Overflow? Never heard of it. Looks all 0 to me :?

/**
 * Checks if `elem_ptr` is in an contiguous "array" -- delineated by `base_ptr` and `len`. 
 * 
 * @tparam T Any "sized" type -- not sure if this concept of Rust applies here...
 * @param elem_ptr Pointer to element to check if in array.
 * @param base_ptr Pointer to element which is base of array.
 * @param len      Size of that contiguous memory block from `base_ptr`. 
 * @return true  是
 * @return false 否
 */
template<typename T>
inline const bool __in_ptr_bound(
	const T* elem_ptr, 
	const T* base_ptr, 
	const uint64_t len
) {
	const size_t alignment = sizeof(*base_ptr); 
	return base_ptr <= elem_ptr && elem_ptr < base_ptr + len * alignment; 
}

/**
 * Finds the index of `*elem_ptr` in the contiguous "array" between `base_ptr[0]` and `base_ptr[len]`. 
 * 
 * @attention
 * Assumes that `elem_ptr` is in the contiguous "array" delineated by `base_ptr` and `len`. 
 * 
 * @tparam T Any "sized" type -- not sure if this concept of Rust applies here...
 * @param elem_ptr Pointer to element to check if in array.
 * @param base_ptr Pointer to element which is base of array.
 * @param len      Size of that contiguous memory block from `base_ptr`. 
 * @return const size_t Index. 
 */
template<typename T>
inline const size_t __idx_of(
	const T* elem_ptr, 
	const T* base_ptr, 
	const uint64_t len
) {
	const size_t alignment = sizeof(*base_ptr); 
	assert(__in_ptr_bound(elem_ptr, base_ptr, len)); 
	return ((uintptr_t)elem_ptr - (uintptr_t)base_ptr) / alignment; 
}

/**
 * A buddy page allocation algorithm.
 */
class BuddyPageAllocator : public PageAllocatorAlgorithm
{
private:
	// [TODO]: Alter _pgds to proper array. 
	/* Base pointer for the page descriptor table. */
	PageDescriptor* const _pgds; 

	/* Size of the page descriptor table. */
	const uint64_t _pgds_len; 

	/* 
	 * Limit pointer for the page descriptor table. Points to hypothetical (last + 1) element. 
	 *
	 * [DANGER] NEVER dereference -- segfault coredump otherwise. 
	 */
	const uintptr_t _pgds_arr_lim; 

	/*
	 * Array of page descriptor pointers for caching the beginning of first pgd for contiguous 
	 * 2^order page cluster allocations. 
	 */
	PageDescriptor* _free_areas[MAX_ORDER+1];

	/** 
	 * Given a page descriptor, and an order, returns the buddy PGD.  The buddy could either be
	 * to the left or the right of PGD, in the given order.
	 * 
	 * @attention
	 * This function DOES NOT guarantee that the returned pointer could be safely dereferenced -- 
	 * it may point to uninitialized data in the array. 
	 * 
	 * @param pgd The page descriptor to find the buddy for.
	 * @param order The order in which the page descriptor lives.
	 * @return Returns the buddy of the given page descriptor, in the given order.
	 */
	PageDescriptor *buddy_of(PageDescriptor *pgd, int order)
	{
		assert(0 <= order && order <= MAX_ORDER); 
		assert(__in_ptr_bound(pgd, _pgds, _pgds_len)); 
 
		// [?] Necessity? Can any `pgd` be assumed to be start of 2^order page cluster? 
		// Align `pgd` to 2^order idx boundary, as shown in Fig.1. 
		const uintptr_t aligned_pgd = (uintptr_t)pgd / TWO_POW(order) * TWO_POW(order); 
		if (aligned_pgd != (uintptr_t)pgd) {
			assert(__in_ptr_bound((PageDescriptor*)aligned_pgd, _pgds, _pgds_len)); 
			mm_log.messagef(
				LogLevel::ERROR, 
				"[buddy::buddy_of] `pgd: 0x%x` not aligned properly to start of block: "
				"Order: %d -- delta: %d; fixed to `aligned_pgd: 0x%x`.", 
				pgd, 
				order, 
				TWO_POW(order), 
				aligned_pgd
			); 
		}

        const uintptr_t delta = sizeof(*pgd) * TWO_POW(order); 

		// Candidates
		const uintptr_t aligned_pgd_l = aligned_pgd - delta; 
		const uintptr_t aligned_pgd_r = aligned_pgd + delta; 

		// "pattern-matching"
		const bool pattern[2] = {
			__in_ptr_bound((PageDescriptor*)aligned_pgd_l, _pgds, _pgds_len), 
			__in_ptr_bound((PageDescriptor*)aligned_pgd_r, _pgds, _pgds_len)
		}; 
		if (pattern[0] && pattern[1]) {
			// => L and R both in "array"
			const size_t idx_aligned_pgd = __idx_of((PageDescriptor*)aligned_pgd, _pgds, _pgds_len); 
			if (idx_aligned_pgd % TWO_POW(order + 1)) {
				// => `aligned_pgd` right buddy 
				return (PageDescriptor*)aligned_pgd_l; 
			} else {
				// => `aligned_pgd` left buddy
				return (PageDescriptor*)aligned_pgd_r; 
			}
		} else if (pattern[1]) {
			// => Only R in "array"; `aligned_pgd` first
			assert(__idx_of((PageDescriptor*)aligned_pgd, _pgds, _pgds_len) == 0); 
			return (PageDescriptor*)aligned_pgd_r; 
		} else if (pattern[0]) {
			// => Only L in "array"; `aligned_pgd` last 
			assert(__idx_of((PageDescriptor*)aligned_pgd, _pgds, _pgds_len) 
				== _pgds_len - TWO_POW(order)); 
			return (PageDescriptor*)aligned_pgd_l; 
		} else {
			// unreachable!
			mm_log.message(
				LogLevel::FATAL, 
				"[buddy::buddy_of] Both left and right not in `_pgds`. Crashed!"
			); 
			assert(false); 
		}
	}

	/**
	 * Given a pointer to a block of free memory in the order "source_order", this function will
	 * split the block in half, and insert it into the order below.
	 * 
	 * @attention
	 * This function assumes that block_pointer points to the beginning of a 2^source_order-aligned 
	 * block. 
	 * 
	 * @attention
	 * This function passes assertion iff 0 < source_order <= MAX_ORDER. 
 	 * 
	 * @param block_pointer A pointer to a pointer containing the beginning of a block memory already marked free.
	 * @param source_order The order in which the block of free memory exists.  Naturally,
	 * the split will insert the two new blocks into the order below.
	 * @return Returns the left-hand-side of the new block.
	 */
	PageDescriptor *split_block(PageDescriptor **block_pointer, int source_order)
	{
		assert(0 < source_order && source_order <= MAX_ORDER); 

		const size_t src_page_count = TWO_POW(source_order); 
		const size_t tgt_order = source_order - 1; 
		const size_t tgt_page_count = TWO_POW(tgt_order); 

		/* Assert block_pointer is available for allocation <1> */
		assert((*block_pointer)->type == PageDescriptorType::AVAILABLE); 

		/* 
		 * Alter _free_area cache ptr for 2^src_order block. Defaults to top memory unless nullptr. 
		 * 
		 * src_left_neighbour describes prev. free block @ order `src_order` -- must be NULL?    
		 * src_right_neighbour describes next free block @ order `src_order` -- not necessarily buddies. 
		 */
		PageDescriptor* src_left_neighbour = (*block_pointer)->prev_free;  // Must be NULL? 
		assert(src_left_neighbour == NULL); 
		PageDescriptor* src_right_neighbour = (*block_pointer)->next_free; // May be NULL
		// By invariant, next top memory has to be:  
		_free_areas[source_order] = src_right_neighbour; // next or NULL 

		/* 
		 * Make hypothetical next order boundary. 
		 * 
		 * tgt_left_buddy describes left buddy block @ order `tgt_order` just freed. 
		 * tgt_right_buddy describes right buddy block @ order `tgt_order` just freed. 
		 */
		PageDescriptor* tgt_left_buddy; 
		PageDescriptor* tgt_right_buddy; 
		{ // Clean up function scope
			PageDescriptor* tgt_buddy = buddy_of(*block_pointer, tgt_order); 
			tgt_left_buddy = (*block_pointer <= tgt_buddy) ? *block_pointer : tgt_buddy; 
			tgt_right_buddy = (*block_pointer > tgt_buddy) ? *block_pointer : tgt_buddy; 
		}
		assert(tgt_left_buddy == *block_pointer); // [Defn?]

		/* Alter left buddy state */
		assert(tgt_left_buddy->type == PageDescriptorType::AVAILABLE); // Ref. <1>

		/* Alter right buddy state */
		assert(tgt_right_buddy->type != PageDescriptorType::INVALID); // [Spec?]
		tgt_right_buddy->type = PageDescriptorType::AVAILABLE; 
		
		/* Alter _free_area cache AND prev_free, next_free */
		if (_free_areas[tgt_order] == NULL) {
			// => First time allocation, tgt_left_buddy guaranteed top

			// Before tgt_left_buddy link...
			tgt_left_buddy->prev_free = NULL; 

			// Between buddies link...
			tgt_left_buddy->next_free = tgt_right_buddy; 
			tgt_right_buddy->prev_free = tgt_left_buddy; 

			// After tgt_right_buddy link...
			tgt_right_buddy->next_free = NULL;

			// Maintain _free_areas invariant
			_free_areas[tgt_order] = tgt_left_buddy;  

		} else if (_free_areas[tgt_order] + sizeof(PageDescriptor) * tgt_page_count <= tgt_left_buddy) {
			// => _free_areas[tgt_order] strictly on top of tgt_left_buddy

			/* [NO FAIL]
			 * Ensure current cached tgt_order block's prev_free is NULL i.e., is topmost. 
			 * This algorithm guarantees that _free_areas always hold the topmost block allocatable (or will it?)
			 */
			assert(_free_areas[tgt_order]->prev_free == NULL); 

			/* [NO FAIL]
			 * Ensure no overlap btwn
			 * ------------------------   --------------------------------   -----------------------------------
			 * |_free_areas[tgt_order]|...|tgt_left_buddy|tgt_right_buddy|...|_free_areas[tgt_order]->next_free|
			 * ------------------------   --------------------------------   -----------------------------------
			 */
			assert(
				_free_areas[tgt_order]->next_free == NULL || 
				_free_areas[tgt_order]->next_free >= 
				tgt_right_buddy + sizeof(PageDescriptor) * tgt_page_count
			); 

			// Before tgt_left_buddy link...
			tgt_left_buddy->prev_free = _free_areas[tgt_order]; 
			auto tmp_next_free = _free_areas[tgt_order]->next_free; 
			_free_areas[tgt_order]->next_free = tgt_left_buddy; 

			// Between buddies link...
			tgt_left_buddy->next_free = tgt_right_buddy; 
			tgt_right_buddy->prev_free = tgt_left_buddy; 

			// After tgt_right_buddy link...
			tgt_right_buddy->next_free = tmp_next_free; 
			if (tmp_next_free != NULL) tmp_next_free->prev_free = tgt_right_buddy; 

			// Maintain _free_areas invariant
			// Already maintained since tgt_left_buddy is not the topmost block. 

		} else if (tgt_right_buddy + sizeof(PageDescriptor) * tgt_page_count <= _free_areas[tgt_order]) {
			// => _free_areas[tgt_order] strictly on bottom of tgt_right_buddy

			/* [NO FAIL]
			 * Ensure current cached tgt_order block's prev_free is NULL i.e., is topmost. 
			 * This algorithm guarantees that _free_areas always hold the topmost block allocatable (or will it?)
			 * 
			 * This also ensures block boundary validation, like above. 
			 */
			assert(_free_areas[tgt_order]->prev_free == NULL); 

			// Before tgt_left_buddy link...
			tgt_left_buddy->prev_free = NULL; 
			
			// Between buddies link...
			tgt_left_buddy->next_free = tgt_right_buddy; 
			tgt_right_buddy->prev_free = tgt_left_buddy; 

			// After tgt_right_buddy link...
			tgt_right_buddy->next_free = _free_areas[tgt_order];
			_free_areas[tgt_order]->prev_free = tgt_right_buddy; 

			// Maintain _free_areas invariant
			_free_areas[tgt_order] = tgt_left_buddy; 

		} else {
			// unreachable! 
			mm_log.messagef(
				LogLevel::FATAL, 
				"[buddy::split_block] Block segmentation fault when trying to free pgd@0x%x -- pfn:0x%x. "
				"Crashed!", 
				tgt_left_buddy, 
				sys.mm().pgalloc().pgd_to_pfn(tgt_left_buddy)
			); 
			dump_state(); 
			assert(false); 
		}

		return tgt_left_buddy; 
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
		assert(0 <= source_order && source_order < MAX_ORDER); 

		const size_t src_page_count = TWO_POW(source_order); 
		const size_t tgt_order = source_order + 1; 
		const size_t tgt_page_count = TWO_POW(tgt_order); 

		/* Assert block_pointer is available for allocation  */
		assert((*block_pointer)->type == PageDescriptorType::AVAILABLE); 

		/* Alter _free_area cache for 2^src_order block. Defaults to top memory unless nullptr. */
		PageDescriptor* src_left_neighbour = (*block_pointer)->prev_free; 
		assert(src_left_neighbour == NULL); 
		PageDescriptor* src_right_neighbour = (*block_pointer)->next_free; 
		if (src_right_neighbour != buddy_of(*block_pointer, source_order)) { // Must be buddy
			mm_log.messagef(
				LogLevel::FATAL, 
				"[buddy::merge_block] Block alignment fault when trying to merge buddies (@0x%x, @0x%x). "
				"Crashed!", 
				*block_pointer, 
				src_right_neighbour
			); 
			dump_state(); 
			assert(false); 
		} 
		// Likewise by invariant: 
		_free_areas[source_order] = src_right_neighbour->next_free; // next or NULL

		/* Make hypothetical next order boundary. */
		PageDescriptor* tgt_block = (*block_pointer); 

		/* Alter right src block state */
		assert(src_right_neighbour->type != PageDescriptorType::INVALID); 
		src_right_neighbour->type = PageDescriptorType::ALLOCATED; 

		/* Alter _free_area cache AND prev_free, next_free */
		if (_free_areas[tgt_order] == NULL) {
			// => First time allocation, tgt_block guaranteed top

			// Before tgt_block link...
			tgt_block->prev_free = NULL; 

			// After tgt_block link...
			tgt_block->next_free = NULL; 

			// Maintain _free_areas invariant
			_free_areas[tgt_order] = tgt_block; 
		} else if (_free_areas[tgt_order] + sizeof(PageDescriptor) * tgt_page_count <= tgt_block) {
			// => _free_areas[tgt_order] strictly before tgt_block, do not update _free_areas

			/* [NO FAIL]
			 * Ensure no overlap btwn
			 * ------------------------   -----------   -----------------------------------
			 * |_free_areas[tgt_order]|...|tgt_block|...|_free_areas[tgt_order]->next_free|
			 * ------------------------   -----------   -----------------------------------
			 */
			assert(
				_free_areas[tgt_order]->next_free == NULL || 
				tgt_block + sizeof(PageDescriptor) * tgt_page_count <= _free_areas[tgt_order]->next_free
			); 

			// Before tgt_block link...
			auto tmp_block = _free_areas[tgt_order]->next_free; 
			tgt_block->prev_free = _free_areas[tgt_order];  
			_free_areas[tgt_order]->next_free = tgt_block; 

			// After tgt_block link...
			tgt_block->next_free = tmp_block; 
			tmp_block->prev_free = tgt_block; 

		} else if (tgt_block + sizeof(PageDescriptor) * tgt_page_count <= _free_areas[tgt_order]) {
			// => _free_areas[tgt_order] strictly after tgt_block

			/* [NO FAIL]
			 * Ensure _free_areas[tgt_order] has no prev_free i.e., is NULL
			 */
			assert(_free_areas[tgt_order]->prev_free == NULL); 

			// Before tgt_block link...
			tgt_block->prev_free = NULL; 

			// After tgt_block link...
			tgt_block->next_free = _free_areas[tgt_order]; 
			_free_areas[tgt_order]->prev_free = tgt_block; 

			// Maintain invariant in _free_areas
			_free_areas[tgt_order] = tgt_block; 
		} else {
			// unreachable! 
			mm_log.messagef(
				LogLevel::FATAL, 
				"[buddy::merge_block] Block segmentation fault when trying to merge "
				"(pgd@0x%x) btwn (pgd@0x%x, pgd@0x%x, pgd@0x%x). Crashed!", 
				tgt_block, 
				_free_areas[tgt_order]->prev_free, 
				_free_areas[tgt_order], 
				_free_areas[tgt_order]->next_free
			); 
			dump_state(); 
			assert(false); 
		}

		return &(_free_areas[tgt_order]); // [Spec?]
	}

public:
	/**
	 * Allocates 2^order number of contiguous pages
	 * @param order The power of two, of the number of contiguous pages to allocate.
	 * @return Returns a pointer to the first page descriptor for the newly allocated page range, or NULL if
	 * allocation failed.
	 */
	PageDescriptor *allocate_pages(int order) override
	{
		if (_free_areas[order] != NULL) {
			// => Exists free block of this order
			auto tmp = _free_areas[order]; 
			tmp->type = PageDescriptorType::ALLOCATED; 
			_free_areas[order] = _free_areas[order]->next_free; 
			return tmp; 
		}

		// Since all free pages are buckled up to its largest order, 
		// We can allocate a page as long as there exists free pages of larger order. 
		size_t from_order = -1; 
		for (size_t o = order + 1; o <= MAX_ORDER; o++) {
			if (_free_areas[o] != NULL) {
				from_order = o;
				break; 
			}
		}
		if (from_order == -1) return NULL; // No free page found. Not enough memory. 

		split_block(&_free_areas[from_order], from_order); 
		return allocate_pages(order); // Recurse w/ _free_areas[from_order] split to lower order.
	}

    /**
	 * Frees 2^order contiguous pages.
	 * @param pgd A pointer to an array of page descriptors to be freed.
	 * @param order The power of two number of contiguous pages to free.
	 */
    void free_pages(PageDescriptor *pgd, int order) override
    {
		// Recursive too? 




    }

    /**
     * Marks a range of pages as available for allocation.
     * @param start A pointer to the first page descriptors to be made available.
     * @param count The number of page descriptors to make available.
     */
    virtual void insert_page_range(PageDescriptor *start, uint64_t count) override
    { 
		// TODO! 
    }

    /**
     * Marks a range of pages as unavailable for allocation.
     * @param start A pointer to the first page descriptors to be made unavailable.
     * @param count The number of page descriptors to make unavailable.
     */
    virtual void remove_page_range(PageDescriptor *start, uint64_t count) override
    {
		// TODO!
    }

	/**
	 * Initialises the allocation algorithm.
	 * @return Returns TRUE if the algorithm was successfully initialised, FALSE otherwise.
	 */
	bool init(PageDescriptor *page_descriptors, uint64_t nr_page_descriptors) override
	{
		PageDescriptor* _pgds (page_descriptors); 
		_pgds->prev_free = NULL; 
		_pgds->next_free = NULL;
		_pgds->type = PageDescriptorType::AVAILABLE; 

		uint64_t _pgds_len (nr_page_descriptors); 
		uintptr_t _pgds_arr_lim ((uintptr_t)_pgds + _pgds_len * sizeof(PageDescriptor)); 

		// Initialize with 1x2^18 block in allocable mem
		PageDescriptor* _free_areas[MAX_ORDER + 1] { NULL }; 
		_free_areas[MAX_ORDER] = page_descriptors;
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