#include "mesh_frame.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t source[600];
    for (size_t index = 0; index < sizeof(source); index++) source[index] = (uint8_t)index;
    uint8_t packet[MESH_RADIO_MTU];
    mesh_frame_state_t state = {0};
    size_t offset = 0;
    while (offset < sizeof(source)) {
        size_t encoded = mesh_frame_encode(source, sizeof(source), 42, offset, packet, sizeof(packet));
        assert(encoded > MESH_FRAGMENT_HEADER_SIZE && encoded <= MESH_RADIO_MTU);
        int result = mesh_frame_feed(&state, packet, encoded);
        offset += encoded - MESH_FRAGMENT_HEADER_SIZE;
        assert(result == (offset == sizeof(source) ? 1 : 0));
    }
    assert(state.total == sizeof(source));
    assert(memcmp(source, state.data, sizeof(source)) == 0);
    assert(mesh_frame_feed(&state, packet, 3) == -1);
    assert(mesh_frame_encode(source, MESH_FRAME_MAX + 1, 1, 0, packet, sizeof(packet)) == 0);
    size_t encoded = mesh_frame_encode(source, sizeof(source), 43, 234, packet, sizeof(packet));
    assert(mesh_frame_feed(&state, packet, encoded) == -1);
    puts("PASS: bounded mesh fragmentation, reconstruction, and malformed/out-of-order rejection");
    return 0;
}