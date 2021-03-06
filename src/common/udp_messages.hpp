#pragma once

#include "config.hpp"
#include "hashing.hpp"
#include <array>
#include <cstddef>
#include <glm/glm.hpp>
#include <ostream>

enum class UdpMsgType : uint8_t {
	/** A GeomUpdatePacket, which modifies a model's vertices or indices */
	GEOM_UPDATE = 0x01,
	/** A PointLightUpdatePacket, which modifies a light's position and/or color and/or attenuation */
	POINT_LIGHT_UPDATE = 0x02,
	/** A TransformUpdatePacket, which modifies a model's transform */
	TRANSFORM_UPDATE = 0x03,
	/** An ACK to some UDP message. Typically sent by the client. */
	ACK = 0x20,
	UNKNOWN
};

inline std::ostream& operator<<(std::ostream& s, UdpMsgType msg)
{
	switch (msg) {
		using M = UdpMsgType;
	case M::GEOM_UPDATE:
		s << "GEOM_UPDATE";
		break;
	case M::POINT_LIGHT_UPDATE:
		s << "POINT_LIGHT_UPDATE";
		break;
	case M::TRANSFORM_UPDATE:
		s << "TRANSFORM_UPDATE";
		break;
	case M::ACK:
		s << "ACK";
		break;
	default:
		s << "UNKNOWN";
		break;
	}
	return s;
}

constexpr UdpMsgType byte2udpmsg(uint8_t byte)
{
	return byte == 0 || byte > static_cast<uint8_t>(UdpMsgType::UNKNOWN) ? UdpMsgType::UNKNOWN
									     : static_cast<UdpMsgType>(byte);
}

constexpr uint8_t udpmsg2byte(UdpMsgType type)
{
	return type == UdpMsgType::UNKNOWN ? 0 : static_cast<uint8_t>(type);
}

enum class GeomDataType : uint8_t {
	VERTEX = 0,
	INDEX = 1,
	INVALID = 2,
};

#pragma pack(push, 1)

struct UdpHeader {
	/** Sequential packet "generation" id. Used to discard old packets. */
	uint32_t packetGen;

	/** How many bytes of the payload are actual data (as there may be garbage at the end).
	 *  Must be equal to the sum of all the chunks' size (type + header + payload).
	 */
	uint32_t size;
};

/** A single UDP packet. The format is the following:
 *  [udp header] (containing packet generation and total payload size)
 *  [chunk0 type] (containing the type of the next chunk header + payload)
 *  [chunk0 header] (the metadata about its payload)
 *  [chunk0 payload] (the actual data)
 *  [chunk1 type]
 *  ...
 *  Note that a UdpPacket can contain different types of chunks.
 *  A chunk has a payload only if it's of variable size.
 */
struct UdpPacket {
	UdpHeader header;
	/** Payload contains chunks (each consisting of ChunkHeader + chunk payload) */
	std::array<uint8_t, cfg::PACKET_SIZE_BYTES - sizeof(UdpHeader)> payload;
};

/** Update vertex/index buffers of existing model */
struct GeomUpdateHeader {
	/** Unique ID of the packet, needed to ACK it */
	uint32_t serialId;

	StringId modelId;

	/** Whether vertices or indices follow */
	GeomDataType dataType;

	/** Starting vertex/index to modify */
	uint32_t start;
	/** Amount of vertices/indices to modify */
	uint32_t len;
};

/** Update color/attenuation of existing point light
 *  Note: this chunk is header-only.
 */
struct PointLightUpdateHeader {
	StringId lightId;
	glm::vec3 color;
	float attenuation;
};

/** Update transform of an object (currently, only a model).
 *  Note: this chunk is header-only.
 */
struct TransformUpdateHeader {
	StringId objectId;
	// TODO: we may compress this information, as some elements of the matrix are always 0.
	glm::mat4 transform;
};

/** A client-to-server ACK packet. It's a standalone struct, not part of UdpPacket. */
struct AckPacket {
	/** Must be UdpMsgType::ACK */
	UdpMsgType msgType;
	uint32_t nAcks;
	std::array<uint32_t, (cfg::PACKET_SIZE_BYTES - sizeof(UdpMsgType) - sizeof(uint32_t)) / sizeof(uint32_t)> acks;
};

#pragma pack(pop)

static_assert(sizeof(UdpPacket) == cfg::PACKET_SIZE_BYTES, "sizeof(UdpPacket) != PACKET_SIZE_BYTES!");
static_assert(sizeof(AckPacket) <= cfg::PACKET_SIZE_BYTES, "sizeof(AckPacket) > PACKET_SIZE_BYTES!");
