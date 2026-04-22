#include "buddy.h"
#include <stddef.h>

#define PAGE_SIZE 4096
#define MAXRANK_CONST 16
#define MAX_PAGES (1 << 15)

static void *base_ptr = NULL;
static int total_pages = 0;
static int max_rank = 1;

static unsigned char free_bitmap[MAXRANK_CONST + 1][MAX_PAGES];
static int free_counts[MAXRANK_CONST + 1];
static unsigned char allocated_rank[MAX_PAGES];

static inline int valid_rank(int r) { return r >= 1 && r <= max_rank; }
static inline int blocks_count(int r) { return (total_pages >> (r - 1)); }

int init_page(void *p, int pgcount){
    base_ptr = p;
    total_pages = pgcount;

    // Compute max_rank based on total_pages (assumes power of two, capped at 16)
    max_rank = 1;
    int t = total_pages;
    while (t > 1 && max_rank < MAXRANK_CONST) { t >>= 1; max_rank++; }

    // Clear state
    for (int r = 1; r <= MAXRANK_CONST; ++r) {
        free_counts[r] = 0;
        int cnt = (r <= max_rank) ? blocks_count(r) : 0;
        for (int i = 0; i < (cnt > 0 ? cnt : 0); ++i) free_bitmap[r][i] = 0;
    }
    for (int i = 0; i < total_pages && i < MAX_PAGES; ++i) allocated_rank[i] = 0;

    // Initially one big free block at max_rank
    if (max_rank >= 1) {
        free_bitmap[max_rank][0] = 1;
        free_counts[max_rank] = 1;
    }
    return OK;
}

void *alloc_pages(int rank){
    if (!valid_rank(rank)) return ERR_PTR(-EINVAL);

    // Find smallest k >= rank with a free block (choose lowest index for determinism)
    int k_found = 0, idx = -1;
    for (int k = rank; k <= max_rank; ++k) {
        int cnt = blocks_count(k);
        for (int i = 0; i < cnt; ++i) {
            if (free_bitmap[k][i]) { k_found = k; idx = i; break; }
        }
        if (k_found) break;
    }
    if (!k_found) return ERR_PTR(-ENOSPC);

    // Remove chosen block from its free list
    free_bitmap[k_found][idx] = 0;
    free_counts[k_found]--;

    // Split down to desired rank, always taking the left child; right child stays free
    int left_idx = idx;
    for (int j = k_found - 1; j >= rank; --j) {
        int right_idx = left_idx * 2 + 1;
        free_bitmap[j][right_idx] = 1;
        free_counts[j]++;
        left_idx = left_idx * 2; // go left
    }

    // Allocate the final block at rank
    int base_idx = left_idx << (rank - 1);
    allocated_rank[base_idx] = (unsigned char)rank;
    char *base = (char *)base_ptr;
    return (void *)(base + (size_t)base_idx * PAGE_SIZE);
}

int return_pages(void *p){
    if (p == NULL || base_ptr == NULL) return -EINVAL;
    char *base = (char *)base_ptr;
    char *cp = (char *)p;
    if (cp < base) return -EINVAL;
    size_t diff = (size_t)(cp - base);
    if (diff % PAGE_SIZE != 0) return -EINVAL;
    int base_idx = (int)(diff / PAGE_SIZE);
    if (base_idx < 0 || base_idx >= total_pages) return -EINVAL;

    int r = allocated_rank[base_idx];
    if (!valid_rank(r)) return -EINVAL;

    // Clear allocation mark
    allocated_rank[base_idx] = 0;

    // Compute index at rank r
    int idx_r = base_idx >> (r - 1);
    // Coalesce upwards while buddy at current level is free
    while (r < max_rank) {
        int buddy_idx = idx_r ^ 1;
        if (buddy_idx >= blocks_count(r)) break;
        if (free_bitmap[r][buddy_idx]) {
            // Remove buddy from free list
            free_bitmap[r][buddy_idx] = 0;
            free_counts[r]--;
            // Move up
            idx_r >>= 1;
            r++;
        } else {
            break;
        }
    }
    // Insert the final block into free list
    free_bitmap[r][idx_r] = 1;
    free_counts[r]++;
    return OK;
}

int query_ranks(void *p){
    if (p == NULL || base_ptr == NULL) return -EINVAL;
    char *base = (char *)base_ptr;
    char *cp = (char *)p;
    if (cp < base) return -EINVAL;
    size_t diff = (size_t)(cp - base);
    if (diff % PAGE_SIZE != 0) return -EINVAL;
    int base_idx = (int)(diff / PAGE_SIZE);
    if (base_idx < 0 || base_idx >= total_pages) return -EINVAL;

    int ar = allocated_rank[base_idx];
    if (valid_rank(ar)) return ar;

    // For free pages, return the maximum rank containing this page
    for (int r = max_rank; r >= 1; --r) {
        int idx_r = base_idx >> (r - 1);
        if (idx_r < blocks_count(r) && free_bitmap[r][idx_r]) return r;
    }
    // If not found, treat as invalid (likely interior of allocated block)
    return -EINVAL;
}

int query_page_counts(int rank){
    if (!valid_rank(rank)) return -EINVAL;
    return free_counts[rank];
}
