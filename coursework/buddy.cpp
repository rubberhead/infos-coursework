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
	PageDescriptor* _pgds; 
	const uint64_t _pgds_len; 

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
 
		// [?] Necessity? Can any `pgd` be assumed to be start of ordered page cluster? 
		// Align `pgd` to 2^order idx boundary, as shown in Fig.1. 
		const uintptr_t aligned_pgd = (uintptr_t)pgd / TWO_POW(order) * TWO_POW(order); 
		if (aligned_pgd != (uintptr_t)pgd) {
			assert(__in_ptr_bound((PageDescriptor*)aligned_pgd, _pgds, _pgds_len)); 
			mm_log.messagef(
				LogLevel::ERROR, 
				"[buddy::buddy_of] `pgd: 0x%x` not aligned properly to start of page cluster: "
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
	 * @param block_pointer A pointer to a pointer containing the beginning of a block memory already marked free.
	 * @param source_order The order in which the block of free memory exists.  Naturally,
	 * the split will insert the two new blocks into the order below.
	 * @return Returns the left-hand-side of the new block.
	 */
	PageDescriptor *split_block(PageDescriptor **block_pointer, int source_order)
	{
        // TODO: Implement me!
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
        // TODO: Implement me!
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
		_pgds = page_descriptors; 
		uint64_t _pgds_len (nr_page_descriptors); 
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


private:
	PageDescriptor *_free_areas[MAX_ORDER+1];
};

/* --- DO NOT CHANGE ANYTHING BELOW THIS LINE --- */

/*
 * Allocation algorithm registration framework
 */
RegisterPageAllocator(BuddyPageAllocator);