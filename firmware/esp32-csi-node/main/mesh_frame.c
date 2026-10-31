#include "mesh_frame.h"
#include <string.h>

#define MESH_FRAGMENT_MAGIC 0x52564d46u

static void put_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static uint16_t get_u16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static void put_u32(uint8_t *bytes, uint32_t value)
{
    for (size_t index = 0; index < 4; index++) bytes[index] = (uint8_t)(value >> (index * 8));
}

static uint32_t get_u32(const uint8_t *bytes)
{
    uint32_t value = 0;
    for (size_t index = 0; index < 4; index++) value |= (uint32_t)bytes[index] << (index * 8);
    return value;
}

size_t mesh_frame_encode(const uint8_t *frame, size_t total, uint32_t sequence,
                         size_t offset, uint8_t *packet, size_t capacity)
{
    if (frame == NULL || packet == NULL || total < 4 || total > MESH_FRAME_MAX
        || offset >= total || capacity <= MESH_FRAGMENT_HEADER_SIZE) return 0;
    size_t payload = total - offset;
    if (payload > MESH_RADIO_MTU - MESH_FRAGMENT_HEADER_SIZE) payload = MESH_RADIO_MTU - MESH_FRAGMENT_HEADER_SIZE;
    if (payload + MESH_FRAGMENT_HEADER_SIZE > capacity) return 0;
    put_u32(packet, MESH_FRAGMENT_MAGIC);
    put_u32(packet + 4, sequence);
    put_u16(packet + 8, (uint16_t)total);
    put_u16(packet + 10, (uint16_t)offset);
    put_u16(packet + 12, (uint16_t)payload);
    packet[14] = 1;
    packet[15] = 0;
    memcpy(packet + MESH_FRAGMENT_HEADER_SIZE, frame + offset, payload);
    return MESH_FRAGMENT_HEADER_SIZE + payload;
}

int mesh_frame_feed(mesh_frame_state_t *state, const uint8_t *packet, size_t length)
{
    if (state == NULL || packet == NULL || length <= MESH_FRAGMENT_HEADER_SIZE
        || length > MESH_RADIO_MTU || get_u32(packet) != MESH_FRAGMENT_MAGIC
        || packet[14] != 1 || packet[15] != 0) return -1;
    uint32_t sequence = get_u32(packet + 4);
    uint16_t total = get_u16(packet + 8);
    uint16_t offset = get_u16(packet + 10);
    uint16_t payload = get_u16(packet + 12);
    if (total < 4 || total > MESH_FRAME_MAX || payload == 0
        || length != MESH_FRAGMENT_HEADER_SIZE + payload
        || offset >= total || (size_t)offset + payload > total) return -1;
    if (offset == 0) {
        state->sequence = sequence;
        state->total = total;
        state->received = 0;
    }
    if (state->sequence != sequence || state->total != total || state->received != offset) return -1;
    memcpy(state->data + offset, packet + MESH_FRAGMENT_HEADER_SIZE, payload);
    state->received = (uint16_t)(offset + payload);
    return state->received == state->total ? 1 : 0;
}