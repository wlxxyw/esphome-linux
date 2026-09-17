/**
 * @file sensor_proto.c
 * @brief Protobuf encoding/decoding implementation for Sensor messages
 */

#include "include/sensor_proto.h"
#include <string.h>
#include <stdio.h>

/**
 * Encode ListEntitiesSensorResponse (message ID 16)
 *
 * Fields:
 * 1: object_id (string)
 * 2: key (fixed32)
 * 3: name (string)
 * 5: icon (string)
 * 6: unit_of_measurement (string)
 * 7: accuracy_decimals (int32)
 * 8: force_update (bool)
 * 9: device_class (string)
 * 10: state_class (enum)
 * 12: disabled_by_default (bool)
 * 13: entity_category (enum)
 */
size_t sensor_encode_list_entities_response(uint8_t *buf, size_t size,
                                            const list_entities_sensor_response_t *msg) {
    pb_buffer_t pb;
    pb_buffer_init_write(&pb, buf, size);

    // Field 1: object_id (string)
    if (msg->object_id[0]) {
        if (!pb_encode_string(&pb, 1, msg->object_id)) {
            return 0;
        }
    }

    // Field 2: key (fixed32)
    uint8_t tag2 = PB_FIELD_TAG(2, PB_WIRE_TYPE_32BIT);
    if (pb.pos + 1 + 4 <= pb.size) {
        pb.data[pb.pos++] = tag2;
        *(uint32_t*)(pb.data + pb.pos) = msg->key;
        pb.pos += 4;
    } else {
        pb.error = true;
        return 0;
    }

    // Field 3: name (string)
    if (!pb_encode_string(&pb, 3, msg->name)) {
        return 0;
    }

    // Field 5: icon (string) - optional
    if (msg->icon[0]) {
        if (!pb_encode_string(&pb, 5, msg->icon)) {
            return 0;
        }
    }

    // Field 6: unit_of_measurement (string)
    if (msg->unit_of_measurement[0]) {
        if (!pb_encode_string(&pb, 6, msg->unit_of_measurement)) {
            return 0;
        }
    }

    // Field 7: accuracy_decimals (int32)
    if (msg->accuracy_decimals != 0) {
        if (!pb_encode_sint32(&pb, 7, msg->accuracy_decimals)) {
            return 0;
        }
    }

    // Field 8: force_update (bool)
    if (msg->force_update) {
        if (!pb_encode_bool(&pb, 8, msg->force_update)) {
            return 0;
        }
    }

    // Field 9: device_class (string)
    if (msg->device_class[0]) {
        if (!pb_encode_string(&pb, 9, msg->device_class)) {
            return 0;
        }
    }

    // Field 10: state_class (enum)
    if (msg->state_class != SENSOR_STATE_CLASS_NONE) {
        if (!pb_encode_uint32(&pb, 10, (uint32_t)msg->state_class)) {
            return 0;
        }
    }

    // Field 12: disabled_by_default (bool)
    if (msg->disabled_by_default) {
        if (!pb_encode_bool(&pb, 12, msg->disabled_by_default)) {
            return 0;
        }
    }

    // Field 13: entity_category (enum)
    if (msg->entity_category != ENTITY_CATEGORY_NONE) {
        if (!pb_encode_uint32(&pb, 13, (uint32_t)msg->entity_category)) {
            return 0;
        }
    }

    return pb.error ? 0 : pb.pos;
}

/**
 * Encode SensorStateResponse (message ID 25)
 *
 * Fields:
 * 1: key (fixed32)
 * 2: state (float)
 * 3: missing_state (bool)
 */
size_t sensor_encode_state_response(uint8_t *buf, size_t size,
                                    const sensor_state_response_t *msg) {
    pb_buffer_t pb;
    pb_buffer_init_write(&pb, buf, size);

    // Field 1: key (fixed32)
    uint8_t tag1 = PB_FIELD_TAG(1, PB_WIRE_TYPE_32BIT);
    if (pb.pos + 1 + 4 <= pb.size) {
        pb.data[pb.pos++] = tag1;
        *(uint32_t*)(pb.data + pb.pos) = msg->key;
        pb.pos += 4;
    } else {
        pb.error = true;
        return 0;
    }

    // Field 2: state (float - 32-bit IEEE 754)
    if (pb.pos + 1 + 4 <= pb.size) {
        pb.data[pb.pos++] = PB_FIELD_TAG(2, PB_WIRE_TYPE_32BIT);
        *(float*)(pb.data + pb.pos) = msg->state;
        pb.pos += 4;
    } else {
        pb.error = true;
        return 0;
    }

    // Field 3: missing_state (bool)
    if (msg->missing_state) {
        if (!pb_encode_bool(&pb, 3, msg->missing_state)) {
            return 0;
        }
    }

    return pb.error ? 0 : pb.pos;
}
