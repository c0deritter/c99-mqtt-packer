#include <stdio.h>
#include <string.h>
#include "mqtt_packer.h"

int32_t unpackMqttChunk
(
    struct MqttPacket *packet, 
    uint32_t *currentSize, 
    uint32_t chunkSize,
    void (*onMqttPacketComplete) (struct MqttPacket *packet, void *extraData),
    void *onMqttPacketCompleteExtraData
)
{
    // Since one chunk might containt more than one MQTT packet, we need
    // to repeat parsing until all MQTT packets are processed.
    do
    {
        // If the size is zero, initialize everything
        if (packet->size == 0)
        {
            packet->fixedHeaderSize = 0;
            packet->type = 0;
            packet->remainingSize = 0;
        } 

        // Read the first of two bytes of the fixed header which is always present
        if (packet->size == 0 && (*currentSize) >= 1)
        {
            packet->fixedHeaderSize = 2;
            packet->type = (packet->bytes[0] & 0xF0) >> 4;
        }

        // Read the second of two bytes of the fixed header which is always present and
        // which contains the first byte of the remaining length
        if (packet->size <= 1 && (*currentSize) >= 2)
        {
            packet->remainingSize = packet->bytes[1] & 0x7F;

            if ((packet->bytes[1] & 0x80) == 128)
            {
                packet->fixedHeaderSize++;
            }
        }

        // Read the second byte of the remaining length if present
        if (packet->size <= 2 && (*currentSize) >= 3 && packet->fixedHeaderSize >= 3)
        {
            packet->remainingSize += (packet->bytes[2] & 0x7F) << 7;

            if ((packet->bytes[2] & 0x80) == 128)
            {
                packet->fixedHeaderSize++;
            }
        }

        // Read the third byte of the remaining length if present
        if (packet->size <= 3 && (*currentSize) >= 4 && packet->fixedHeaderSize >= 4)
        {
            packet->remainingSize += (packet->bytes[3] & 0x7F) << 14;
        
            if ((packet->bytes[3] & 0x80) == 128)
            {
                packet->fixedHeaderSize++;
            }
        }

        // Read the fourth and last byte of the remaining length if present
        if (packet->size <= 4 && (*currentSize) >= 5 && packet->fixedHeaderSize >= 5)
        {
            packet->remainingSize += (packet->bytes[4] & 0x7F) << 21;

            if ((packet->bytes[4] & 0x80) == 128)
            {
                // The continuation bit of the fourth length byte is set
                // which represents a malformed length byte. Ignore it.
            }
        }

        // If the current used size the byte array inside the MqttPacket struct is
        // equal or larger than the current MQTT packet we are finished parsing the
        // MQTT packet.
        if ((*currentSize) >= packet->fixedHeaderSize + packet->remainingSize)
        {
            // At first we update the size stored in the MqttPacket struct to its final size
            packet->size = packet->fixedHeaderSize + packet->remainingSize;

            // Then we call the callback
            if (onMqttPacketComplete != 0)
            {
                onMqttPacketComplete(packet, onMqttPacketCompleteExtraData);
            }

            // If the used size of the byte array is even larger than the just parsed
            // MQTT packet, we have at least an additional MQTT packet.
            if ((*currentSize) > packet->fixedHeaderSize + packet->remainingSize)
            {
                // In that case we need to move the bytes that belong to the next packet
                // to the beginning.
                memmove
                (
                    packet->bytes, 
                    &(packet->bytes[packet->fixedHeaderSize + packet->remainingSize]),
                    (*currentSize) - (packet->fixedHeaderSize + packet->remainingSize)
                );

                // Now we calculate a new current used size representing the amount of bytes that
                // were moved to the beginning.
                (*currentSize) = (*currentSize) - (packet->fixedHeaderSize + packet->remainingSize);
            }

            // If the used size of the array is not larger than the MQTT packet that was just
            // parsed we set the current used size to zero. Note that this change propagates back
            // to the function which owns this information and which will most likely be the
            // function which fills in the bytes of the TCP stream. That way we tell that
            // function to fill the newly arriving bytes at the beginning of the byte array.
            else
            {
                (*currentSize) = 0;
            }

            // Reset the MQTT packet
            packet->size = 0;
            packet->fixedHeaderSize = 0;
            packet->type = 0;
            packet->remainingSize = 0;
        }
        else
        {
            // By setting the size found in the MqttPacket struct to the size of the currently used
            // bytes of the array we will tell the while-loop that we are done parsing for this chunk.
            packet->size = (*currentSize);
        }
    }
    while (packet->size != (*currentSize));

    // Return the packet size which was determined until now
    return packet->fixedHeaderSize + packet->remainingSize;
}

void unpackMqttPacketIdentifier(struct MqttPacket *packet, uint16_t *packetIdentifier)
{
    if
    (
        packet->type == MQTT_PACKET_PUBACK ||
        packet->type == MQTT_PACKET_PUBREC ||
        packet->type == MQTT_PACKET_PUBCOMP ||
        packet->type == MQTT_PACKET_SUBACK ||
        packet->type == MQTT_PACKET_UNSUBACK
    )
    {
        (*packetIdentifier) = (packet->bytes[packet->fixedHeaderSize] << 8) + packet->bytes[packet->fixedHeaderSize + 1];
    }
}


void unpackMqttConnAck(struct MqttPacket *packet, struct MqttConnAckPacket *connAckPacket)
{    
    connAckPacket->sessionPresent = packet->bytes[2] & 0x01 ? 1 : 0;
    connAckPacket->returnCode = packet->bytes[3];
}

void unpackMqttPublish(struct MqttPacket *packet, struct MqttPublishPacket *publishPacket)
{
    publishPacket->dup = packet->bytes[0] & 0x08 ? 1 : 0;
    publishPacket->qos = (packet->bytes[0] & 0x06) >> 1;
    publishPacket->retain = packet->bytes[0] & 0x01 ? 1 : 0;

    publishPacket->topicName = &(packet->bytes[packet->fixedHeaderSize + 2]);
    publishPacket->topicNameSize = (packet->bytes[packet->fixedHeaderSize] << 8) + packet->bytes[packet->fixedHeaderSize + 1];

    // Fixed header size + topic name size value + topic name size
    uint16_t packetIdentifierPosition = packet->fixedHeaderSize + 2 + publishPacket->topicNameSize;
    publishPacket->packetIdentifier = (packet->bytes[packetIdentifierPosition] << 8) + packet->bytes[packetIdentifierPosition + 1];

    // Fixed header size + topic name size value + topic name size + packet identifier
    publishPacket->payload = &(packet->bytes[packet->fixedHeaderSize + 2 + publishPacket->topicNameSize + 2]);
    // Remaining size - topic name size value - topic name size - packet identifier
    publishPacket->payloadSize = packet->remainingSize - 2 - publishPacket->topicNameSize - 2;
}

void unpackMqttSubAck(struct MqttPacket *packet, struct MqttSubAckPacket *subAckPacket)
{
    subAckPacket->packetIdentifier = (packet->bytes[packet->fixedHeaderSize] << 8) + packet->bytes[packet->fixedHeaderSize + 1];
    subAckPacket->returnCode = packet->bytes[packet->fixedHeaderSize + 2];
}