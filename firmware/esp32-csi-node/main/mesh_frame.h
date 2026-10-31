#pragma once
#include <stddef.h>
#include <stdint.h>

#define MESH_RADIO_MTU 250u
#define MESH_FRAGMENT_HEADER_SIZE 16u
#define MESH_FRAME_MAX 2068u

typedef struct {
    uint32_t sequence;
    uint16_t total;
    uint16_t received;
    uint8_t data[MESH_FRAME_MAX];
} mesh_frame_state_t;

size_t mesh_frame_encode(const uint8_t *frame, size_t total, uint32_t sequence,
                         size_t offset, uint8_t *packet, size_t capacity);
int mesh_frame_feed(mesh_frame_state_t *state, const uint8_t *packet, size_t length);