/**
 * @file sensor_proto.h
 * @brief Protobuf encoding/decoding for ESPHome Sensor messages
 *
 * These functions extend esphome_proto.h with Sensor-specific messages.
 */

#ifndef SENSOR_PROTO_H
#define SENSOR_PROTO_H

#include "esphome_proto.h"

/**
 * Sensor State Class enum (from api.proto)
 */
typedef enum {
    SENSOR_STATE_CLASS_NONE = 0,
    SENSOR_STATE_CLASS_MEASUREMENT = 1,
    SENSOR_STATE_CLASS_TOTAL_INCREASING = 2,
    SENSOR_STATE_CLASS_TOTAL = 3
} sensor_state_class_t;

/**
 * Entity Category enum (from api.proto)
 */
typedef enum {
    ENTITY_CATEGORY_NONE = 0,
    ENTITY_CATEGORY_CONFIG = 1,
    ENTITY_CATEGORY_DIAGNOSTIC = 2
} entity_category_t;

/**
 * List Entities Sensor Response structure (message ID 16)
 * From api.proto ListEntitiesSensorResponse
 */
typedef struct {
    char object_id[64];         // Field 1: Object ID
    uint32_t key;               // Field 2: Entity key
    char name[128];             // Field 3: Display name
    char icon[64];              // Field 5: Icon
    char unit_of_measurement[32]; // Field 6: Unit (%, °C, MB, etc.)
    int32_t accuracy_decimals;  // Field 7: Decimal places
    bool force_update;          // Field 8: Force update
    char device_class[64];      // Field 9: Device class
    sensor_state_class_t state_class; // Field 10: State class
    bool disabled_by_default;   // Field 12: Disabled by default
    entity_category_t entity_category; // Field 13: Entity category
} list_entities_sensor_response_t;

/**
 * Sensor State Response structure (message ID 25)
 * From api.proto SensorStateResponse
 */
typedef struct {
    uint32_t key;               // Field 1: Entity key
    float state;                // Field 2: Current value
    bool missing_state;         // Field 3: Whether state is valid
} sensor_state_response_t;

/**
 * Encode ListEntitiesSensorResponse to protobuf
 *
 * @param buf Output buffer
 * @param size Buffer size
 * @param msg Entity info to encode
 * @return Number of bytes written, or 0 on error
 */
size_t sensor_encode_list_entities_response(uint8_t *buf, size_t size,
                                            const list_entities_sensor_response_t *msg);

/**
 * Encode SensorStateResponse to protobuf
 *
 * @param buf Output buffer
 * @param size Buffer size
 * @param msg State to encode
 * @return Number of bytes written, or 0 on error
 */
size_t sensor_encode_state_response(uint8_t *buf, size_t size,
                                    const sensor_state_response_t *msg);

#endif /* SENSOR_PROTO_H */
