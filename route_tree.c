#include "route_tree.h"
#include <arpa/inet.h>

static RouteTreeNodeV4 **v4_nodes_pool;
static size_t v4_nodes_pool_total;
static size_t v4_nodes_pool_front;
static size_t v4_nodes_pool_rear;

static RouteTreeNodeV6 **v6_nodes_pool;
static size_t v6_nodes_pool_total;
static size_t v6_nodes_pool_front;
static size_t v6_nodes_pool_rear;

enum RouteTreeReturnStatue {
    ROUTE_TREE_FAILED,
    ROUTE_TREE_SUCCESS,
    ROUTE_TREE_FAILED_CONTINUE,
    ROUTE_TREE_SUCCESS_CONTINUE,
};


/*
 * Formula: total_nodes = 2 * max_routes - n_vrf
 * Ignore the number of VRF, so just minus 1.
 */
#define N_ROUTES_TO_N_NODES(n_routes) (2 * (n_routes) - 1)

#define GET_BIT_U32(u32, bit) ((u32) >> (bit) & 0x1)
#define GET_BIT_U64_PTR(u64_ptr, bit) \
                ((((bit) >= 64) ? (((uint64_t*)(u64_ptr))[1]) >> ((bit) - 64): (((uint64_t*)(u64_ptr))[0]) >> (bit)) & 0x1)

#define GET_KEY_32(u32, bit_offset, bit_len) \
                (0 == bit_len ? \
                0 : ((u32) >> (32-((bit_offset)+(bit_len)))) << (32-(bit_len)))
#define GET_KEY_64(u64, bit_offset, bit_len) \
                (0 == bit_len ? \
                0 : ((u64) >> (64-((bit_offset)+(bit_len)))) << (64-(bit_len)))

#define U8_PTR_TO_CPU_IPV6(ipv6, be_u8_ptr) \
                do { \
                    ipv6.u8[15] = be_u8_ptr[0]; ipv6.u8[14] = be_u8_ptr[1]; ipv6.u8[13] = be_u8_ptr[2]; ipv6.u8[12] = be_u8_ptr[3]; \
                    ipv6.u8[11] = be_u8_ptr[4]; ipv6.u8[10] = be_u8_ptr[5]; ipv6.u8[9] = be_u8_ptr[6]; ipv6.u8[8] = be_u8_ptr[7]; \
                    ipv6.u8[7] = be_u8_ptr[8]; ipv6.u8[6] = be_u8_ptr[9]; ipv6.u8[5] = be_u8_ptr[10]; ipv6.u8[4] = be_u8_ptr[11]; \
                    ipv6.u8[3] = be_u8_ptr[12]; ipv6.u8[2] = be_u8_ptr[13]; ipv6.u8[1] = be_u8_ptr[14]; ipv6.u8[0] = be_u8_ptr[15]; \
                } while (0);

#define GET_PARENT_TARGET(node, head_node) \
                ((node)->parent ? \
                    ((node)->parent->next_bit_0 == (node) ? \
                        (void*)(&(node)->parent->next_bit_0) : (void*)(&(node)->parent->next_bit_1)) \
                    : \
                    ((head_node)->first_bit_0 == (node) ? \
                        (void*)(&(head_node)->first_bit_0) : (void*)(&(head_node)->first_bit_1)))


static void get_key_ipv6(const RouteTreeIPV6 *ipv6, uint8_t bit_offset, uint8_t bit_len, RouteTreeIPV6 *ipv6_key)
{
    if (0 == bit_len) {
        ipv6_key->u64[1] = 0;
        ipv6_key->u64[0] = 0;
    }
    else {
        if (bit_offset >= 64) {
            ipv6_key->u64[1] = GET_KEY_64(ipv6->u64[0], bit_offset - 64, bit_len);
            ipv6_key->u64[0] = 0;
        }
        else {
            if (bit_len > 64) {
                const uint8_t u64_1_bit_len = 64 - bit_offset;
                const uint8_t u64_0_bit_len = bit_len - u64_1_bit_len;
                const uint64_t split_key = u64_1_bit_len < 64 ? (GET_KEY_64(ipv6->u64[0], 0, u64_0_bit_len) >> u64_1_bit_len) : 0;
                ipv6_key->u64[1] = GET_KEY_64(ipv6->u64[1], bit_offset, u64_1_bit_len)
                                        | split_key;
                ipv6_key->u64[0] = GET_KEY_64(ipv6->u64[0], bit_offset, bit_len - 64);
            }
            else {
                if (bit_offset + bit_len > 64) {
                    const uint8_t u64_1_bit_len = 64 - bit_offset;
                    const uint8_t u64_0_bit_len = bit_len - u64_1_bit_len;
                    const uint64_t split_key = u64_1_bit_len < 64 ? (GET_KEY_64(ipv6->u64[0], 0, u64_0_bit_len) >> u64_1_bit_len) : 0;
                    ipv6_key->u64[1] = GET_KEY_64(ipv6->u64[1], bit_offset, u64_1_bit_len)
                                            | split_key;
                    ipv6_key->u64[0] = 0;
                }
                else {
                    ipv6_key->u64[1] = GET_KEY_64(ipv6->u64[1], bit_offset, bit_len);
                    ipv6_key->u64[0] = 0;
                }
            }
        }
    }
}

#define PTR_ADD(ptr, x) ((void*)((uintptr_t)(ptr) + (x)))
#define MOVE_FRONT_REAR(v, total) (((v) + 1) % (total))



static inline int _free_node_v4(const RouteTreeNodeV4 *free_node)
{
    if (MOVE_FRONT_REAR(v4_nodes_pool_rear, v4_nodes_pool_total) == v4_nodes_pool_front) {
        // full, should not happen
        printf("total=%zu front=%zu rear=%zu\n", v4_nodes_pool_total, v4_nodes_pool_front, v4_nodes_pool_rear);
        abort();
    }

    v4_nodes_pool[v4_nodes_pool_rear] = free_node;
    v4_nodes_pool_rear = MOVE_FRONT_REAR(v4_nodes_pool_rear, v4_nodes_pool_total);

    return 0;
}

static inline int _free_node_v6(const RouteTreeNodeV6 *free_node)
{
    if (MOVE_FRONT_REAR(v6_nodes_pool_rear, v6_nodes_pool_total) == v6_nodes_pool_front) {
        // full, should not happen
        printf("total=%zu front=%zu rear=%zu\n", v6_nodes_pool_total, v6_nodes_pool_front, v6_nodes_pool_rear);
        abort();
    }

    v6_nodes_pool[v6_nodes_pool_rear] = free_node;
    v6_nodes_pool_rear = MOVE_FRONT_REAR(v6_nodes_pool_rear, v6_nodes_pool_total);

    return 0;
}

static inline int free_node_v4(RouteTreeHeadNode *head_node_v4, const RouteTreeNodeV4 *free_node)
{
    _free_node_v4(free_node);

    head_node_v4->total_nodes--;

    return 0;
}

static inline int free_node_v6(RouteTreeHeadNode *head_node_v6, const RouteTreeNodeV6 *free_node)
{
    _free_node_v6(free_node);

    head_node_v6->total_nodes--;

    return 0;
}

static inline int alloc_node_bulk_v4(RouteTreeHeadNode *head_node_v4,
                                RouteTreeNodeV4 **new_node,
                                size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        if (v4_nodes_pool_front == v4_nodes_pool_rear) {
            // empty
            size_t has_alloc;
            for (has_alloc = 0; has_alloc < i; ++has_alloc) {
                _free_node_v4(new_node[has_alloc]);
                new_node[has_alloc] = NULL;
            }
            return -1;
        }

        new_node[i] = v4_nodes_pool[v4_nodes_pool_front];
        v4_nodes_pool_front = MOVE_FRONT_REAR(v4_nodes_pool_front, v4_nodes_pool_total);
    }

    head_node_v4->total_nodes += count;

    return 0;
}

static inline int alloc_node_bulk_v6(RouteTreeHeadNode *head_node_v6,
                                RouteTreeNodeV6 **new_node,
                                size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        if (v6_nodes_pool_front == v6_nodes_pool_rear) {
            // empty
            size_t has_alloc;
            for (has_alloc = 0; has_alloc < i; ++has_alloc) {
                _free_node_v6(new_node[has_alloc]);
                new_node[has_alloc] = NULL;
            }
            return -1;
        }

        new_node[i] = v6_nodes_pool[v6_nodes_pool_front];
        v6_nodes_pool_front = MOVE_FRONT_REAR(v6_nodes_pool_front, v6_nodes_pool_total);
    }

    head_node_v6->total_nodes += count;

    return 0;
}

static inline enum RouteTreeReturnStatue lookup_subtree_v4(RouteTreeNodeV4 *node_v4,
                                                RouteTreeNodeV4 **next_node_v4,
                                                RouteTreeNodeV4 **parent_node_v4,
                                                RouteTreeNodeV4 ***target_node_v4,
                                                uint32_t ipv4,
                                                uint8_t depth_len,
                                                uint8_t *bit_offset,
                                                uint32_t *next_hop)
{
    enum RouteTreeReturnStatue ret = ROUTE_TREE_FAILED;

    if (node_v4->key_bit_len > (depth_len - *bit_offset)) {
        // new key is shorter, do not match
        return ret;
    }

    const uint32_t key = GET_KEY_32(ipv4, *bit_offset, node_v4->key_bit_len);
    if (key != node_v4->key) {
        return ret;
    }

    // match success
    if (node_v4->next_hop >= 0) {
        *next_hop = node_v4->next_hop;
        ret = ROUTE_TREE_SUCCESS;
    }

    *bit_offset += node_v4->key_bit_len;
    if (*bit_offset == depth_len) {
        // match done
        return ret;
    }

    if (parent_node_v4) {
        *parent_node_v4 = node_v4;
    }
    if (GET_BIT_U32(ipv4, 31-(*bit_offset))) {
        if (target_node_v4) {
            *target_node_v4 = &node_v4->next_bit_1;
        }
        *next_node_v4 = node_v4->next_bit_1;
    }
    else {
        if (target_node_v4) {
            *target_node_v4 = &node_v4->next_bit_0;
        }
        *next_node_v4 = node_v4->next_bit_0;
    }
    if (NULL == *next_node_v4) {
        return ret;
    }

    if (ROUTE_TREE_SUCCESS == ret) {
        ret = ROUTE_TREE_SUCCESS_CONTINUE;
    }
    else {
        ret = ROUTE_TREE_FAILED_CONTINUE;
    }
    return ret;
}

static inline enum RouteTreeReturnStatue lookup_subtree_v6(RouteTreeNodeV6 *node_v6,
                                                RouteTreeNodeV6 **next_node_v6,
                                                RouteTreeNodeV6 **parent_node_v6,
                                                RouteTreeNodeV6 ***target_node_v6,
                                                const RouteTreeIPV6 *ipv6,
                                                uint8_t depth_len,
                                                uint8_t *bit_offset,
                                                uint32_t *next_hop)
{
    enum RouteTreeReturnStatue ret = ROUTE_TREE_FAILED;

    if (node_v6->key_bit_len > (depth_len - *bit_offset)) {
        // new key is shorter, do not match
        return ret;
    }

    RouteTreeIPV6 key;
    get_key_ipv6(ipv6, *bit_offset, node_v6->key_bit_len, &key);
    if (memcmp(node_v6->key.u8, key.u8, sizeof(node_v6->key.u8))) {
        return ret;
    }

    // match success
    if (node_v6->next_hop >= 0) {
        *next_hop = node_v6->next_hop;
        ret = ROUTE_TREE_SUCCESS;
    }

    *bit_offset += node_v6->key_bit_len;
    if (*bit_offset == depth_len) {
        // match done
        return ret;
    }

    if (parent_node_v6) {
        *parent_node_v6 = node_v6;
    }
    if (GET_BIT_U64_PTR(ipv6->u64, 127-(*bit_offset))) {
        if (target_node_v6) {
            *target_node_v6 = &node_v6->next_bit_1;
        }
        *next_node_v6 = node_v6->next_bit_1;
    }
    else {
        if (target_node_v6) {
            *target_node_v6 = &node_v6->next_bit_0;
        }
        *next_node_v6 = node_v6->next_bit_0;
    }
    if (NULL == *next_node_v6) {
        return ret;
    }

    if (ROUTE_TREE_SUCCESS == ret) {
        ret = ROUTE_TREE_SUCCESS_CONTINUE;
    }
    else {
        ret = ROUTE_TREE_FAILED_CONTINUE;
    }
    return ret;
}

static inline uint32_t get_diff_bit_v4(const RouteTreeNodeV4 *node_v4,
                                uint32_t ipv4,
                                uint8_t bit_offset,
                                uint8_t match_len)
{
    uint32_t match_bit;

    for (match_bit = 0; match_bit < match_len; ++match_bit) {
        if (GET_BIT_U32(node_v4->key, 31-match_bit)
                    != GET_BIT_U32(ipv4, 31-(bit_offset + match_bit))) {
            break;
        }
    }

    return match_bit;
}

static inline uint32_t get_diff_bit_v6(const RouteTreeNodeV6 *node_v6,
                                const RouteTreeIPV6 *ipv6,
                                uint8_t bit_offset,
                                uint8_t match_len)
{
    uint32_t match_bit;

    for (match_bit = 0; match_bit < match_len; ++match_bit) {
        if (GET_BIT_U64_PTR(node_v6->key.u64, 127-match_bit)
                    != GET_BIT_U64_PTR(ipv6->u64, 127-(bit_offset + match_bit))) {
            break;
        }
    }

    return match_bit;
}

static inline void fill_node_v4(RouteTreeNodeV4 *node_v4,
                            uint8_t key_bit_len,
                            uint32_t key,
                            int32_t next_hop,
                            RouteTreeNodeV4 *parent,
                            RouteTreeNodeV4 *next_bit_0,
                            RouteTreeNodeV4 *next_bit_1)
{
    node_v4->key_bit_len = key_bit_len;
    node_v4->key = key;
    node_v4->next_hop = next_hop;
    node_v4->parent = parent;
    node_v4->next_bit_0 = next_bit_0;
    node_v4->next_bit_1 = next_bit_1;

    if (next_bit_0) {
        next_bit_0->parent = node_v4;
    }
    if (next_bit_1) {
        next_bit_1->parent = node_v4;
    }
}

static inline void fill_node_v6(RouteTreeNodeV6 *node_v6,
                            uint8_t key_bit_len,
                            const RouteTreeIPV6 *key,
                            int32_t next_hop,
                            RouteTreeNodeV6 *parent,
                            RouteTreeNodeV6 *next_bit_0,
                            RouteTreeNodeV6 *next_bit_1)
{
    node_v6->key_bit_len = key_bit_len;
    memcpy(node_v6->key.u8, key->u8, sizeof(node_v6->key));
    node_v6->next_hop = next_hop;
    node_v6->parent = parent;
    node_v6->next_bit_0 = next_bit_0;
    node_v6->next_bit_1 = next_bit_1;

    if (next_bit_0) {
        next_bit_0->parent = node_v6;
    }
    if (next_bit_1) {
        next_bit_1->parent = node_v6;
    }
}

static inline int handle_mismatch_node_v4(RouteTreeHeadNode *head_node_v4,
                                RouteTreeNodeV4 *node_v4,
                                uint32_t ipv4,
                                uint8_t depth_len,
                                uint8_t bit_offset,
                                uint32_t next_hop,
                                RouteTreeNodeV4 **target_node_v4)
{
    RouteTreeNodeV4 *new_node[3];
    if (alloc_node_bulk_v4(head_node_v4, new_node, 3)) {
        return -1;
    }

    uint32_t match_bit = get_diff_bit_v4(node_v4, ipv4, bit_offset, node_v4->key_bit_len);

    fill_node_v4(new_node[0], match_bit, GET_KEY_32(node_v4->key, 0, match_bit),
            -1, node_v4->parent, new_node[1], new_node[2]);

    RouteTreeNodeV4 *ori_node_p2;
    RouteTreeNodeV4 *new_route_node;
    if (GET_BIT_U32(node_v4->key, 31-match_bit)) {
        ori_node_p2 = new_node[2];
        new_route_node = new_node[1];
    }
    else {
        ori_node_p2 = new_node[1];
        new_route_node = new_node[2];
    }

    fill_node_v4(ori_node_p2,
            node_v4->key_bit_len - match_bit,
            GET_KEY_32(node_v4->key, match_bit, node_v4->key_bit_len - match_bit),
            node_v4->next_hop, new_node[0], node_v4->next_bit_0, node_v4->next_bit_1);

    fill_node_v4(new_route_node,
            depth_len - (bit_offset + match_bit),
            GET_KEY_32(ipv4, (bit_offset + match_bit), depth_len - (bit_offset + match_bit)),
            next_hop, new_node[0], NULL, NULL);

    *target_node_v4 = new_node[0];
    free_node_v4(head_node_v4, node_v4);

    return 0;
}

static inline int handle_mismatch_node_v6(RouteTreeHeadNode *head_node_v6,
                                RouteTreeNodeV6 *node_v6,
                                const RouteTreeIPV6 *ipv6,
                                uint8_t depth_len,
                                uint8_t bit_offset,
                                uint32_t next_hop,
                                RouteTreeNodeV6 **target_node_v6)
{
    RouteTreeNodeV6 *new_node[3];
    if (alloc_node_bulk_v6(head_node_v6, new_node, 3)) {
        return -1;
    }

    uint32_t match_bit = get_diff_bit_v6(node_v6, ipv6, bit_offset, node_v6->key_bit_len);

    RouteTreeIPV6 ipv6_key;
    get_key_ipv6(&node_v6->key, 0, match_bit, &ipv6_key);
    fill_node_v6(new_node[0], match_bit, &ipv6_key,
            -1, node_v6->parent, new_node[1], new_node[2]);

    RouteTreeNodeV6 *ori_node_p2;
    RouteTreeNodeV6 *new_route_node;
    if (GET_BIT_U64_PTR(node_v6->key.u64, 127-match_bit)) {
        ori_node_p2 = new_node[2];
        new_route_node = new_node[1];
    }
    else {
        ori_node_p2 = new_node[1];
        new_route_node = new_node[2];
    }

    get_key_ipv6(&node_v6->key, match_bit, node_v6->key_bit_len - match_bit, &ipv6_key);
    fill_node_v6(ori_node_p2,
            node_v6->key_bit_len - match_bit,
            &ipv6_key,
            node_v6->next_hop, new_node[0], node_v6->next_bit_0, node_v6->next_bit_1);

    get_key_ipv6(ipv6, (bit_offset + match_bit), depth_len - (bit_offset + match_bit), &ipv6_key);
    fill_node_v6(new_route_node,
            depth_len - (bit_offset + match_bit),
            &ipv6_key,
            next_hop, new_node[0], NULL, NULL);

    *target_node_v6 = new_node[0];
    free_node_v6(head_node_v6, node_v6);

    return 0;
}

static inline int handle_merge_node_v4(RouteTreeHeadNode *head_node_v4,
                        RouteTreeNodeV4 *parent_node_v4,
                        RouteTreeNodeV4 *child_node_v4,
                        RouteTreeNodeV4 **target_node_v4)
{
    RouteTreeNodeV4 *new_node;
    if (alloc_node_bulk_v4(head_node_v4, &new_node, 1)) {
        return -1;
    }

    const uint32_t key = parent_node_v4->key | (child_node_v4->key >> parent_node_v4->key_bit_len);

    fill_node_v4(new_node,
                parent_node_v4->key_bit_len + child_node_v4->key_bit_len,
                key,
                child_node_v4->next_hop,
                parent_node_v4->parent,
                child_node_v4->next_bit_0,
                child_node_v4->next_bit_1);

    *target_node_v4 = new_node;

    free_node_v4(head_node_v4, parent_node_v4);
    free_node_v4(head_node_v4, child_node_v4);

    return 0;
}

static inline void _merge_ipv6_key(uint8_t dst_bit_len,
                                RouteTreeIPV6 *dst,
                                const RouteTreeIPV6 *src)
{
    if (0 == dst_bit_len) {
        *dst = *src;
    }
    else if (dst_bit_len >= 128) {
        return;
    }
    else if (dst_bit_len >= 64) {
        dst->u64[0] |= src->u64[1] >> (dst_bit_len - 64);
    }
    else {
        dst->u64[1] |= src->u64[1] >> dst_bit_len;
        dst->u64[0] |= src->u64[1] << (64 - dst_bit_len);
        dst->u64[0] |= src->u64[0] >> dst_bit_len;
    }
}

static inline int handle_merge_node_v6(RouteTreeHeadNode *head_node_v6,
                        RouteTreeNodeV6 *parent_node_v6,
                        RouteTreeNodeV6 *child_node_v6,
                        RouteTreeNodeV6 **target_node_v6)
{
    RouteTreeNodeV6 *new_node;
    if (alloc_node_bulk_v6(head_node_v6, &new_node, 1)) {
        return -1;
    }

    RouteTreeIPV6 key = parent_node_v6->key;
    _merge_ipv6_key(parent_node_v6->key_bit_len, &key, &child_node_v6->key);

    fill_node_v6(new_node,
                parent_node_v6->key_bit_len + child_node_v6->key_bit_len,
                &key,
                child_node_v6->next_hop,
                parent_node_v6->parent,
                child_node_v6->next_bit_0,
                child_node_v6->next_bit_1);

    *target_node_v6 = new_node;

    free_node_v6(head_node_v6, parent_node_v6);
    free_node_v6(head_node_v6, child_node_v6);

    return 0;
}

static char _tree_iterate_str[128];

static void _compressed_route_tree_iterate_v4(RouteTreeHeadNode *head_node_v4,
                                        const RouteTreeNodeV4 *node_v4,
                                        uint32_t ipv4,
                                        uint8_t bit_offset,
                                        uint32_t print_prefix,
                                        bool print_tree,
                                        bool reset)
{
    ipv4 |= (node_v4->key >> bit_offset);
    bit_offset += node_v4->key_bit_len;

    const uint8_t *p8 = (uint8_t *)&ipv4;
    print_prefix += snprintf(_tree_iterate_str, sizeof(_tree_iterate_str),
                            "%u.%u.%u.%u/%u next_hop=%d",
                            p8[3], p8[2], p8[1], p8[0], bit_offset,
                            node_v4->next_hop);
    if (print_tree) {
        printf("%s", _tree_iterate_str);
    }
    if (node_v4->next_hop < 0
            && (NULL == node_v4->next_bit_0 || NULL == node_v4->next_bit_1)) {
        printf("%s is invalid node", _tree_iterate_str);
        if (!print_tree) {
            printf("\n");
        }
    }

    const RouteTreeNodeV4 *next_node_v4;

    next_node_v4 = node_v4->next_bit_0;
    if (next_node_v4) {
        if (print_tree) {
            if (next_node_v4->parent != node_v4) {
                printf(" -? ");
            }
            else {
                printf(" -- ");
            }
        }
        _compressed_route_tree_iterate_v4(head_node_v4, next_node_v4,
                ipv4, bit_offset, print_prefix + 4, print_tree, reset);
    }

    next_node_v4 = node_v4->next_bit_1;
    if (next_node_v4) {
        if (print_tree) {
            printf("\n");
            uint32_t i;
            for (i = 0; i < print_prefix; ++i) {
                printf(" ");
            }

            if (next_node_v4->parent != node_v4) {
                printf(" |? ");
            }
            else {
                printf(" |- ");
            }
        }
        _compressed_route_tree_iterate_v4(head_node_v4, next_node_v4,
                ipv4, bit_offset, print_prefix + 4, print_tree, reset);
    }

    if (reset) {
        free_node_v4(head_node_v4, node_v4);
    }
}

static void _compressed_route_tree_iterate_v6(RouteTreeHeadNode *head_node_v6,
                                        const RouteTreeNodeV6 *node_v6,
                                        RouteTreeIPV6 *_ipv6,
                                        uint8_t bit_offset,
                                        uint32_t print_prefix,
                                        bool print_tree,
                                        bool reset)
{
    RouteTreeIPV6 ipv6 = *_ipv6;
    _merge_ipv6_key(bit_offset, &ipv6, &node_v6->key);
    bit_offset += node_v6->key_bit_len;

    print_prefix += snprintf(_tree_iterate_str, sizeof(_tree_iterate_str),
                            "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x/%u next_hop=%d",
                            ipv6.u8[15], ipv6.u8[14], ipv6.u8[13], ipv6.u8[12],
                            ipv6.u8[11], ipv6.u8[10], ipv6.u8[9], ipv6.u8[8],
                            ipv6.u8[7], ipv6.u8[6], ipv6.u8[5], ipv6.u8[4],
                            ipv6.u8[3], ipv6.u8[2], ipv6.u8[1], ipv6.u8[0],
                            bit_offset,
                            node_v6->next_hop);
    if (print_tree) {
        printf("%s", _tree_iterate_str);
    }
    if (node_v6->next_hop < 0
            && (NULL == node_v6->next_bit_0 || NULL == node_v6->next_bit_1)) {
        printf("%s is invalid node", _tree_iterate_str);
        if (!print_tree) {
            printf("\n");
        }
    }

    const RouteTreeNodeV6 *next_node_v6;

    next_node_v6 = node_v6->next_bit_0;
    if (next_node_v6) {
        if (print_tree) {
            if (next_node_v6->parent != node_v6) {
                printf(" -? ");
            }
            else {
                printf(" -- ");
            }
        }
        _compressed_route_tree_iterate_v6(head_node_v6, next_node_v6,
                &ipv6, bit_offset, print_prefix + 4, print_tree, reset);
    }

    next_node_v6 = node_v6->next_bit_1;
    if (next_node_v6) {
        if (print_tree) {
            printf("\n");
            uint32_t i;
            for (i = 0; i < print_prefix; ++i) {
                printf(" ");
            }

            if (next_node_v6->parent != node_v6) {
                printf(" |? ");
            }
            else {
                printf(" |- ");
            }
        }
        _compressed_route_tree_iterate_v6(head_node_v6, next_node_v6,
                &ipv6, bit_offset, print_prefix + 4, print_tree, reset);
    }

    if (reset) {
        free_node_v6(head_node_v6, node_v6);
    }
}


// Public API:


size_t compressed_route_tree_get_memory_footprint_v4(const size_t v4_max_routes)
{
    // Circular queue need one extra space to distinguish queue empty/full.
    return sizeof(**v4_nodes_pool) * N_ROUTES_TO_N_NODES(v4_max_routes)
                + sizeof(*v4_nodes_pool) * (N_ROUTES_TO_N_NODES(v4_max_routes) + 1);
}

size_t compressed_route_tree_get_memory_footprint_v6(const size_t v6_max_routes)
{
    // Circular queue need one extra space to distinguish queue empty/full.
    return sizeof(**v6_nodes_pool) * N_ROUTES_TO_N_NODES(v6_max_routes)
                + sizeof(*v6_nodes_pool) * (N_ROUTES_TO_N_NODES(v6_max_routes) + 1);
}

int compressed_route_tree_init_nodes(void * const v4_nodes_pool_ptr,
                                    const size_t v4_max_routes,
                                    void * const v6_nodes_pool_ptr,
                                    const size_t v6_max_routes)
{
    v4_nodes_pool = (RouteTreeNodeV4 **)PTR_ADD(v4_nodes_pool_ptr, sizeof(**v4_nodes_pool) * N_ROUTES_TO_N_NODES(v4_max_routes));
    // Circular queue need one extra space to distinguish queue empty/full.
    v4_nodes_pool_total = N_ROUTES_TO_N_NODES(v4_max_routes) + 1;
    v4_nodes_pool_front = 0;
    v4_nodes_pool_rear  = 0;

    v6_nodes_pool = (RouteTreeNodeV6 **)PTR_ADD(v6_nodes_pool_ptr, sizeof(**v6_nodes_pool) * N_ROUTES_TO_N_NODES(v6_max_routes));
    // Circular queue need one extra space to distinguish queue empty/full.
    v6_nodes_pool_total = N_ROUTES_TO_N_NODES(v6_max_routes) + 1;
    v6_nodes_pool_front = 0;
    v6_nodes_pool_rear  = 0;

    size_t i;
    for (i = 0; i < N_ROUTES_TO_N_NODES(v4_max_routes); ++i) {
        RouteTreeNodeV4 *free_node = &((RouteTreeNodeV4 *)v4_nodes_pool_ptr)[i];
        free_node->next_hop = -1;
        if (_free_node_v4(free_node)) {
            return -1;
        }
    }
    for (i = 0; i < N_ROUTES_TO_N_NODES(v6_max_routes); ++i) {
        RouteTreeNodeV6 *free_node = &((RouteTreeNodeV6 *)v6_nodes_pool_ptr)[i];
        free_node->next_hop = -1;
        if (_free_node_v6(free_node)) {
            return -1;
        }
    }

    return 0;
}

void compressed_route_tree_reset_head(RouteTreeHeadNode *head_node)
{
    memset(head_node, 0, sizeof(*head_node));
    head_node->default_next_hop = -1;
}

size_t compressed_route_tree_pool_free_count_v4()
{
    return (v4_nodes_pool_rear - v4_nodes_pool_front + v4_nodes_pool_total) % v4_nodes_pool_total;
}

size_t compressed_route_tree_pool_count_v4()
{
    // Circular queue need one extra space to distinguish queue empty/full.
    return v4_nodes_pool_total - compressed_route_tree_pool_free_count_v4() - 1;
}

size_t compressed_route_tree_pool_free_count_v6()
{
    return (v6_nodes_pool_rear - v6_nodes_pool_front + v6_nodes_pool_total) % v6_nodes_pool_total;
}

size_t compressed_route_tree_pool_count_v6()
{
    // Circular queue need one extra space to distinguish queue empty/full.
    return v6_nodes_pool_total - compressed_route_tree_pool_free_count_v6() - 1;
}

int compressed_route_tree_lookup_v4(const RouteTreeHeadNode *head_node_v4,
                                uint32_t be_ipv4,
                                uint32_t *next_hop)
{
    int ret = -1;

    if (head_node_v4->default_next_hop >= 0) {
        *next_hop = head_node_v4->default_next_hop;
        ret = 0;
    }

    uint32_t ipv4 = ntohl(be_ipv4);

    RouteTreeNodeV4 *node_v4;
    if (GET_BIT_U32(ipv4, 31)) {
        node_v4 = (RouteTreeNodeV4 *)head_node_v4->first_bit_1;
    }
    else {
        node_v4 = (RouteTreeNodeV4 *)head_node_v4->first_bit_0;
    }
    if (NULL == node_v4) {
        goto ret;
    }

    uint8_t bit_offset = 0;
    enum RouteTreeReturnStatue status;
    do {
        status = lookup_subtree_v4(node_v4, &node_v4, NULL, NULL, ipv4, 32, &bit_offset, next_hop);
        if (ROUTE_TREE_SUCCESS == status || ROUTE_TREE_SUCCESS_CONTINUE == status) {
            ret = 0;
        }
    } while (ROUTE_TREE_FAILED_CONTINUE == status || ROUTE_TREE_SUCCESS_CONTINUE == status);

ret:
    return ret;
}

int compressed_route_tree_lookup_v6(const RouteTreeHeadNode *head_node_v6,
                                const uint8_t *be_ipv6_u8ptr,
                                uint32_t *next_hop)
{
    int ret = -1;

    if (head_node_v6->default_next_hop >= 0) {
        *next_hop = head_node_v6->default_next_hop;
        ret = 0;
    }

    RouteTreeIPV6 ipv6;
    U8_PTR_TO_CPU_IPV6(ipv6, be_ipv6_u8ptr);

    RouteTreeNodeV6 *node_v6;
    if (GET_BIT_U64_PTR(ipv6.u64, 127)) {
        node_v6 = (RouteTreeNodeV6 *)head_node_v6->first_bit_1;
    }
    else {
        node_v6 = (RouteTreeNodeV6 *)head_node_v6->first_bit_0;
    }
    if (NULL == node_v6) {
        goto ret;
    }

    uint8_t bit_offset = 0;
    enum RouteTreeReturnStatue status;
    do {
        status = lookup_subtree_v6(node_v6, &node_v6, NULL, NULL, &ipv6, 128, &bit_offset, next_hop);
        if (ROUTE_TREE_SUCCESS == status || ROUTE_TREE_SUCCESS_CONTINUE == status) {
            ret = 0;
        }
    } while (ROUTE_TREE_FAILED_CONTINUE == status || ROUTE_TREE_SUCCESS_CONTINUE == status);

ret:
    return ret;
}

int compressed_route_tree_add_v4(RouteTreeHeadNode *head_node_v4,
                            uint32_t be_ipv4,
                            uint8_t depth_len,
                            uint32_t next_hop)
{
    if (depth_len > 32) {
        return -1;
    }

    if (0 == depth_len) {
        head_node_v4->default_next_hop = next_hop;
        return 0;
    }

    uint32_t ipv4 = ntohl(be_ipv4);
    ipv4 = GET_KEY_32(ipv4, 0, depth_len);

    uint8_t bit_offset = 0;
    RouteTreeNodeV4 **target_node_v4;
    RouteTreeNodeV4 *node_v4;
    RouteTreeNodeV4 *parent_node_v4 = NULL;
    if (GET_BIT_U32(ipv4, 31)) {
        target_node_v4 = (RouteTreeNodeV4 **)(&head_node_v4->first_bit_1);
        node_v4 = (RouteTreeNodeV4 *)head_node_v4->first_bit_1;
        
    }
    else {
        target_node_v4 = (RouteTreeNodeV4 **)(&head_node_v4->first_bit_0);
        node_v4 = (RouteTreeNodeV4 *)head_node_v4->first_bit_0;
    }
    if (NULL == node_v4) {
        goto add;
    }

    enum RouteTreeReturnStatue status;
    uint32_t next_hop_exist;
    do {
        status = lookup_subtree_v4(node_v4, &node_v4, &parent_node_v4,
                                &target_node_v4, ipv4, depth_len,
                                &bit_offset, &next_hop_exist);
    } while (ROUTE_TREE_FAILED_CONTINUE == status || ROUTE_TREE_SUCCESS_CONTINUE == status);

    if (bit_offset == depth_len) {
        // match done
        if (node_v4->next_hop < 0) {
            head_node_v4->total_routes++;
            head_node_v4->add_count++;
        }
        node_v4->next_hop = next_hop;
        return 0;
    }

add:
    if (NULL == node_v4) {
        // no node
        RouteTreeNodeV4 *new_node;
        if (alloc_node_bulk_v4(head_node_v4, &new_node, 1)) {
            return -1;
        }

        fill_node_v4(new_node, (depth_len - bit_offset), GET_KEY_32(ipv4, bit_offset, (depth_len - bit_offset)),
                    next_hop, parent_node_v4, NULL, NULL);

        *target_node_v4 = new_node;
    }
    else {
        // mismatch or new key is shorter
        if (node_v4->key_bit_len > (depth_len - bit_offset)) {
            // key is shorter
            uint32_t match_bit = get_diff_bit_v4(node_v4, ipv4, bit_offset, (depth_len - bit_offset));

            if ((depth_len - bit_offset) != match_bit) {
                // mismatch
                if (handle_mismatch_node_v4(head_node_v4, node_v4, ipv4, depth_len, bit_offset, next_hop, target_node_v4)) {
                    return -1;
                }
            }
            else {
                // shorter consistent
                RouteTreeNodeV4 *new_node[2];
                if (alloc_node_bulk_v4(head_node_v4, new_node, 2)) {
                    return -1;
                }

                if (GET_BIT_U32(node_v4->key, 31-(depth_len - bit_offset))) {
                    fill_node_v4(new_node[0], (depth_len - bit_offset), GET_KEY_32(node_v4->key, 0, (depth_len - bit_offset)),
                            next_hop, parent_node_v4, NULL, new_node[1]);
                }
                else {
                    fill_node_v4(new_node[0], (depth_len - bit_offset), GET_KEY_32(node_v4->key, 0, (depth_len - bit_offset)),
                            next_hop, parent_node_v4, new_node[1], NULL);
                }

                fill_node_v4(new_node[1],
                        node_v4->key_bit_len - (depth_len - bit_offset),
                        GET_KEY_32(node_v4->key, (depth_len - bit_offset), node_v4->key_bit_len - (depth_len - bit_offset)),
                        node_v4->next_hop, new_node[0], node_v4->next_bit_0, node_v4->next_bit_1);

                *target_node_v4 = new_node[0];
                free_node_v4(head_node_v4, node_v4);
            }
        }
        else {
            // mismatch
            if (handle_mismatch_node_v4(head_node_v4, node_v4, ipv4, depth_len, bit_offset, next_hop, target_node_v4)) {
                return -1;
            }
        }
    }

    head_node_v4->total_routes++;
    head_node_v4->add_count++;

    return 0;
}

int compressed_route_tree_add_v6(RouteTreeHeadNode *head_node_v6,
                            const uint8_t *be_ipv6_u8ptr,
                            uint8_t depth_len,
                            uint32_t next_hop)
{
    if (depth_len > 128) {
        return -1;
    }

    if (0 == depth_len) {
        head_node_v6->default_next_hop = next_hop;
        return 0;
    }

    RouteTreeIPV6 ipv6_ori;
    U8_PTR_TO_CPU_IPV6(ipv6_ori, be_ipv6_u8ptr);
    RouteTreeIPV6 ipv6;
    get_key_ipv6(&ipv6_ori, 0, depth_len, &ipv6);

    uint8_t bit_offset = 0;
    RouteTreeNodeV6 **target_node_v6;
    RouteTreeNodeV6 *node_v6;
    RouteTreeNodeV6 *parent_node_v6 = NULL;
    if (GET_BIT_U64_PTR(ipv6.u64, 127)) {
        target_node_v6 = (RouteTreeNodeV6 **)(&head_node_v6->first_bit_1);
        node_v6 = (RouteTreeNodeV6 *)head_node_v6->first_bit_1;
    }
    else {
        target_node_v6 = (RouteTreeNodeV6 **)(&head_node_v6->first_bit_0);
        node_v6 = (RouteTreeNodeV6 *)head_node_v6->first_bit_0;
    }
    if (NULL == node_v6) {
        goto add;
    }

    enum RouteTreeReturnStatue status;
    uint32_t next_hop_exist;
    do {
        status = lookup_subtree_v6(node_v6, &node_v6, &parent_node_v6,
                                &target_node_v6, &ipv6, depth_len,
                                &bit_offset, &next_hop_exist);
    } while (ROUTE_TREE_FAILED_CONTINUE == status || ROUTE_TREE_SUCCESS_CONTINUE == status);

    if (bit_offset == depth_len) {
        // match done
        if (node_v6->next_hop < 0) {
            head_node_v6->total_routes++;
            head_node_v6->add_count++;
        }
        node_v6->next_hop = next_hop;
        return 0;
    }

add:
    if (NULL == node_v6) {
        // no node
        RouteTreeNodeV6 *new_node;
        if (alloc_node_bulk_v6(head_node_v6, &new_node, 1)) {
            return -1;
        }

        RouteTreeIPV6 key;
        get_key_ipv6(&ipv6, bit_offset, (depth_len - bit_offset), &key);
        fill_node_v6(new_node, (depth_len - bit_offset), &key,
                    next_hop, parent_node_v6, NULL, NULL);

        *target_node_v6 = new_node;
    }
    else {
        // mismatch or new key is shorter
        if (node_v6->key_bit_len > (depth_len - bit_offset)) {
            // key is shorter
            uint32_t match_bit = get_diff_bit_v6(node_v6, &ipv6, bit_offset, (depth_len - bit_offset));

            if ((depth_len - bit_offset) != match_bit) {
                // mismatch
                if (handle_mismatch_node_v6(head_node_v6, node_v6, &ipv6, depth_len, bit_offset, next_hop, target_node_v6)) {
                    return -1;
                }
            }
            else {
                // shorter consistent
                RouteTreeNodeV6 *new_node[2];
                if (alloc_node_bulk_v6(head_node_v6, new_node, 2)) {
                    return -1;
                }

                RouteTreeIPV6 key;
                get_key_ipv6(&node_v6->key, 0, (depth_len - bit_offset), &key);
                if (GET_BIT_U64_PTR(node_v6->key.u64, 127-(depth_len - bit_offset))) {
                    fill_node_v6(new_node[0], (depth_len - bit_offset), &key,
                            next_hop, parent_node_v6, NULL, new_node[1]);
                }
                else {
                    fill_node_v6(new_node[0], (depth_len - bit_offset), &key,
                            next_hop, parent_node_v6, new_node[1], NULL);
                }

                get_key_ipv6(&node_v6->key, (depth_len - bit_offset), node_v6->key_bit_len - (depth_len - bit_offset), &key);
                fill_node_v6(new_node[1],
                        node_v6->key_bit_len - (depth_len - bit_offset),
                        &key,
                        node_v6->next_hop, new_node[0], node_v6->next_bit_0, node_v6->next_bit_1);

                *target_node_v6 = new_node[0];
                free_node_v6(head_node_v6, node_v6);
            }
        }
        else {
            // mismatch
            if (handle_mismatch_node_v6(head_node_v6, node_v6, &ipv6, depth_len, bit_offset, next_hop, target_node_v6)) {
                return -1;
            }
        }
    }

    head_node_v6->total_routes++;
    head_node_v6->add_count++;

    return 0;
}

int compressed_route_tree_del_v4(RouteTreeHeadNode *head_node_v4, uint32_t be_ipv4, uint8_t depth_len)
{
    if (depth_len > 32) {
        return -1;
    }

    if (0 == depth_len) {
        head_node_v4->default_next_hop = -1;
        return 0;
    }

    uint32_t ipv4 = ntohl(be_ipv4);
    ipv4 = GET_KEY_32(ipv4, 0, depth_len);

    uint8_t bit_offset = 0;
    RouteTreeNodeV4 **target_node_v4;
    RouteTreeNodeV4 *node_v4;
    RouteTreeNodeV4 *parent_node_v4 = NULL;
    if (GET_BIT_U32(ipv4, 31)) {
        target_node_v4 = (RouteTreeNodeV4 **)(&head_node_v4->first_bit_1);
        node_v4 = (RouteTreeNodeV4 *)head_node_v4->first_bit_1;
    }
    else {
        target_node_v4 = (RouteTreeNodeV4 **)(&head_node_v4->first_bit_0);
        node_v4 = (RouteTreeNodeV4 *)head_node_v4->first_bit_0;
    }
    if (NULL == node_v4) {
        return -1;
    }

    enum RouteTreeReturnStatue status;
    uint32_t next_hop_exist;
    do {
        status = lookup_subtree_v4(node_v4, &node_v4, &parent_node_v4, &target_node_v4, ipv4, depth_len, &bit_offset, &next_hop_exist);
    } while (ROUTE_TREE_FAILED_CONTINUE == status || ROUTE_TREE_SUCCESS_CONTINUE == status);

    if (bit_offset == depth_len) {
        // match done
        if (node_v4->next_bit_0 && node_v4->next_bit_1) {
            // two child: set next_hop invalid;
            node_v4->next_hop = -1;
        }
        else if (node_v4->next_bit_0 || node_v4->next_bit_1) {
            // one child: merge child; delete node;
            if (handle_merge_node_v4(head_node_v4,
                        node_v4,
                        node_v4->next_bit_0 ? node_v4->next_bit_0 : node_v4->next_bit_1,
                        target_node_v4)) {

                return -1;
            }
        }
        else {
            // no child: delete node;
            bool parent_has_two_branches = false;
            if (parent_node_v4 && parent_node_v4->next_bit_0 && parent_node_v4->next_bit_1) {
                parent_has_two_branches = true;
            }

            *target_node_v4 = NULL;
            free_node_v4(head_node_v4, node_v4);

            if (parent_has_two_branches && parent_node_v4->next_hop < 0) {
                // delete node and parent decrease branch; merge invalid next_hop parent and the other child;
                if (handle_merge_node_v4(head_node_v4,
                            parent_node_v4,
                            parent_node_v4->next_bit_0 ? parent_node_v4->next_bit_0 : parent_node_v4->next_bit_1,
                            (RouteTreeNodeV4 **)GET_PARENT_TARGET(parent_node_v4, head_node_v4))) {

                    return -1;
                }
            }
        }
    }
    else {
        return -1;
    }

    head_node_v4->total_routes--;
    head_node_v4->del_count++;

    return 0;
}

int compressed_route_tree_del_v6(RouteTreeHeadNode *head_node_v6, const uint8_t *be_ipv6_u8ptr, uint8_t depth_len)
{
    if (depth_len > 128) {
        return -1;
    }

    if (0 == depth_len) {
        head_node_v6->default_next_hop = -1;
        return 0;
    }

    RouteTreeIPV6 ipv6_ori;
    U8_PTR_TO_CPU_IPV6(ipv6_ori, be_ipv6_u8ptr);
    RouteTreeIPV6 ipv6;
    get_key_ipv6(&ipv6_ori, 0, depth_len, &ipv6);

    uint8_t bit_offset = 0;
    RouteTreeNodeV6 **target_node_v6;
    RouteTreeNodeV6 *node_v6;
    RouteTreeNodeV6 *parent_node_v6 = NULL;
    if (GET_BIT_U64_PTR(ipv6.u64, 127)) {
        target_node_v6 = (RouteTreeNodeV6 **)(&head_node_v6->first_bit_1);
        node_v6 = (RouteTreeNodeV6 *)head_node_v6->first_bit_1;
    }
    else {
        target_node_v6 = (RouteTreeNodeV6 **)(&head_node_v6->first_bit_0);
        node_v6 = (RouteTreeNodeV6 *)head_node_v6->first_bit_0;
    }
    if (NULL == node_v6) {
        return -1;
    }

    enum RouteTreeReturnStatue status;
    uint32_t next_hop_exist;
    do {
        status = lookup_subtree_v6(node_v6, &node_v6, &parent_node_v6, &target_node_v6, &ipv6, depth_len, &bit_offset, &next_hop_exist);
    } while (ROUTE_TREE_FAILED_CONTINUE == status || ROUTE_TREE_SUCCESS_CONTINUE == status);

    if (bit_offset == depth_len) {
        // match done
        if (node_v6->next_bit_0 && node_v6->next_bit_1) {
            // two child: set next_hop invalid;
            node_v6->next_hop = -1;
        }
        else if (node_v6->next_bit_0 || node_v6->next_bit_1) {
            // one child: merge child; delete node;
            if (handle_merge_node_v6(head_node_v6,
                        node_v6,
                        node_v6->next_bit_0 ? node_v6->next_bit_0 : node_v6->next_bit_1,
                        target_node_v6)) {

                return -1;
            }
        }
        else {
            // no child: delete node;
            bool parent_has_two_branches = false;
            if (parent_node_v6 && parent_node_v6->next_bit_0 && parent_node_v6->next_bit_1) {
                parent_has_two_branches = true;
            }

            *target_node_v6 = NULL;
            free_node_v6(head_node_v6, node_v6);

            if (parent_has_two_branches && parent_node_v6->next_hop < 0) {
                // delete node and parent decrease branch; merge invalid next_hop parent and the other child;
                if (handle_merge_node_v6(head_node_v6,
                            parent_node_v6,
                            parent_node_v6->next_bit_0 ? parent_node_v6->next_bit_0 : parent_node_v6->next_bit_1,
                            (RouteTreeNodeV6 **)GET_PARENT_TARGET(parent_node_v6, head_node_v6))) {

                    return -1;
                }
            }
        }
    }
    else {
        return -1;
    }

    head_node_v6->total_routes--;
    head_node_v6->del_count++;

    return 0;
}

int compressed_route_tree_iterate_v4(RouteTreeHeadNode *head_node_v4, bool print_tree, bool reset)
{
    if (print_tree && head_node_v4->default_next_hop >= 0) {
        printf("default next_hop=%d\n", head_node_v4->default_next_hop);
    }

    const RouteTreeNodeV4 *node_v4;

    node_v4 = (RouteTreeNodeV4 *)head_node_v4->first_bit_0;
    if (node_v4) {
        uint32_t ipv4 = 0;
        _compressed_route_tree_iterate_v4(head_node_v4, node_v4, ipv4, 0, 0, print_tree, reset);
    }

    node_v4 = (RouteTreeNodeV4 *)head_node_v4->first_bit_1;
    if (node_v4) {
        if (print_tree) {
            printf("\n");
        }
        uint32_t ipv4 = 0;
        _compressed_route_tree_iterate_v4(head_node_v4, node_v4, ipv4, 0, 0, print_tree, reset);
    }

    if (print_tree) {
        printf("\n\n");
    }

    if (reset) {
        compressed_route_tree_reset_head(head_node_v4);
    }

    return 0;
}

int compressed_route_tree_iterate_v6(RouteTreeHeadNode *head_node_v6, bool print_tree, bool reset)
{
    if (print_tree && head_node_v6->default_next_hop >= 0) {
        printf("default next_hop=%d\n", head_node_v6->default_next_hop);
    }

    const RouteTreeNodeV6 *node_v6;

    node_v6 = (RouteTreeNodeV6 *)head_node_v6->first_bit_0;
    if (node_v6) {
        RouteTreeIPV6 ipv6 = {};
        _compressed_route_tree_iterate_v6(head_node_v6, node_v6, &ipv6, 0, 0, print_tree, reset);
    }

    node_v6 = (RouteTreeNodeV6 *)head_node_v6->first_bit_1;
    if (node_v6) {
        if (print_tree) {
            printf("\n");
        }
        RouteTreeIPV6 ipv6 = {};
        _compressed_route_tree_iterate_v6(head_node_v6, node_v6, &ipv6, 0, 0, print_tree, reset);
    }

    if (print_tree) {
        printf("\n\n");
    }

    if (reset) {
        compressed_route_tree_reset_head(head_node_v6);
    }

    return 0;
}
