#ifndef __ROUTE_TREE_H__
#define __ROUTE_TREE_H__

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>


typedef struct {
    union {
        uint8_t u8[16];
        uint16_t u16[8];
        uint32_t u32[4];
        uint64_t u64[2];
    };
} RouteTreeIPV6;

typedef struct route_tree_head_node_s {
    int32_t default_next_hop;
    void *first_bit_0;
    void *first_bit_1;

    // stats
    size_t total_nodes;

    size_t total_routes;
    size_t add_count;
    size_t del_count;
} RouteTreeHeadNode;

typedef struct route_tree_node_v4_s {
    uint8_t key_bit_len;
    int32_t next_hop;
    uint32_t key;
    struct route_tree_node_v4_s *parent;
    struct route_tree_node_v4_s *next_bit_0;
    struct route_tree_node_v4_s *next_bit_1;
} RouteTreeNodeV4;

typedef struct route_tree_node_v6_s {
    uint8_t key_bit_len;
    int32_t next_hop;
    RouteTreeIPV6 key;
    struct route_tree_node_v6_s *parent;
    struct route_tree_node_v6_s *next_bit_0;
    struct route_tree_node_v6_s *next_bit_1;
} RouteTreeNodeV6;



size_t compressed_route_tree_get_memory_footprint_v4(const size_t v4_max_routes);
size_t compressed_route_tree_get_memory_footprint_v6(const size_t v6_max_routes);

int compressed_route_tree_init_nodes(void * const v4_nodes_pool_ptr, const size_t v4_max_routes,
                                    void * const v6_nodes_pool_ptr, const size_t v6_max_routes);
void compressed_route_tree_reset_head(RouteTreeHeadNode *head_node);

size_t compressed_route_tree_pool_count_v4();
size_t compressed_route_tree_pool_free_count_v4();
size_t compressed_route_tree_pool_count_v6();
size_t compressed_route_tree_pool_free_count_v6();

int compressed_route_tree_lookup_v4(const RouteTreeHeadNode *head_node_v4, uint32_t be_ipv4, uint32_t *next_hop);
int compressed_route_tree_lookup_v6(const RouteTreeHeadNode *head_node_v6, const uint8_t *be_ipv6_u8ptr, uint32_t *next_hop);

int compressed_route_tree_add_v4(RouteTreeHeadNode *head_node_v4, uint32_t be_ipv4, uint8_t depth_len, uint32_t next_hop);
int compressed_route_tree_add_v6(RouteTreeHeadNode *head_node_v6, const uint8_t *be_ipv6_u8ptr, uint8_t depth_len, uint32_t next_hop);

int compressed_route_tree_del_v4(RouteTreeHeadNode *head_node_v4, uint32_t be_ipv4, uint8_t depth_len);
int compressed_route_tree_del_v6(RouteTreeHeadNode *head_node_v6, const uint8_t *be_ipv6_u8ptr, uint8_t depth_len);

int compressed_route_tree_iterate_v4(RouteTreeHeadNode *head_node_v4, bool print_tree, bool reset);
int compressed_route_tree_iterate_v6(RouteTreeHeadNode *head_node_v6, bool print_tree, bool reset);


#endif