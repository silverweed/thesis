#include "udp_serialize.hpp"
#include "server_resources.hpp"
#include "shared_resources.hpp"
#include "udp_messages.hpp"
#include <cassert>

using namespace logging;

std::size_t writeUdpHeader(uint8_t* buffer, std::size_t bufsize, uint64_t packetGen)
{
	assert(bufsize >= sizeof(UdpHeader));

	UdpHeader header;
	header.packetGen = packetGen;
	header.size = 0;

	// DEBUG
	// memset(buffer, 0xAA, bufsize);
	memcpy(buffer, reinterpret_cast<void*>(&header), sizeof(UdpHeader));

	return sizeof(UdpHeader);
}

std::size_t addGeomUpdate(uint8_t* buffer,
	std::size_t bufsize,
	std::size_t offset,
	const GeomUpdateHeader& geomUpdate,
	const ServerResources& resources)
{
	assert(offset < bufsize);
	assert(geomUpdate.modelId != SID_NONE);
	assert(geomUpdate.dataType < GeomDataType::INVALID);

	// Retreive data from the model
	const auto& model_it = resources.models.find(geomUpdate.modelId);
	assert(model_it != resources.models.end());

	void* dataPtr;
	std::size_t dataSize;
	switch (geomUpdate.dataType) {
	case GeomDataType::VERTEX:
		dataPtr = model_it->second.vertices;
		dataSize = sizeof(Vertex);
		break;
	case GeomDataType::INDEX:
		dataPtr = model_it->second.indices;
		dataSize = sizeof(Index);
		break;
	default:
		assert(false);
	};
	const auto payloadSize = dataSize * geomUpdate.len;
	verbose("start: ", geomUpdate.start, ", len: ", geomUpdate.len);
	verbose("offset: ", offset, ", payload size: ", payloadSize, ", bufsize: ", bufsize);
	// Prevent infinite loops
	assert(sizeof(UdpMsgType) + sizeof(GeomUpdateHeader) + payloadSize < bufsize);

	if (offset + sizeof(UdpMsgType) + sizeof(GeomUpdateHeader) + payloadSize > bufsize) {
		verbose("Not enough room!");
		return 0;
	}

	std::size_t written = 0;

	// Write chunk type
	static_assert(sizeof(UdpMsgType) == 1, "Need to change this code!");
	buffer[offset] = udpmsg2byte(UdpMsgType::GEOM_UPDATE);
	written += sizeof(UdpMsgType);

	// Write chunk header
	memcpy(buffer + offset + written, &geomUpdate, sizeof(GeomUpdateHeader));
	written += sizeof(GeomUpdateHeader);

	// Write chunk payload
	memcpy(buffer + offset + written,
		reinterpret_cast<uint8_t*>(dataPtr) + dataSize * geomUpdate.start,
		payloadSize);
	written += payloadSize;

	// Update size in header
	reinterpret_cast<UdpHeader*>(buffer)->size += written;
	verbose("Packet size is now ", reinterpret_cast<UdpHeader*>(buffer)->size);

	return written;
}

std::size_t addPointLightUpdate(uint8_t* buffer,
	std::size_t bufsize,
	std::size_t offset,
	const shared::PointLight& pointLight)
{
	assert(offset < bufsize);
	assert(pointLight.dynMask != 0);

	const std::size_t payloadSize = sizeof(glm::vec3) * !shared::isLightPositionFixed(pointLight.dynMask) +
					sizeof(glm::vec3) * !shared::isLightColorFixed(pointLight.dynMask) +
					sizeof(float) * !shared::isLightIntensityFixed(pointLight.dynMask);

	// Prevent infinite loops
	assert(sizeof(UdpMsgType) + sizeof(PointLightUpdateHeader) + payloadSize < bufsize);

	if (offset + sizeof(UdpMsgType) + sizeof(PointLightUpdateHeader) + payloadSize > bufsize) {
		verbose("Not enough room!");
		return 0;
	}

	std::size_t written = 0;

	// Write chunk type
	static_assert(sizeof(UdpMsgType) == 1, "Need to change this code!");
	buffer[offset] = udpmsg2byte(UdpMsgType::POINT_LIGHT_UPDATE);
	written += sizeof(UdpMsgType);

	// Write header
	PointLightUpdateHeader header;
	header.lightId = pointLight.name;
	header.updateMask = pointLight.dynMask;

	memcpy(buffer + offset + written, &header, sizeof(PointLightUpdateHeader));
	written += sizeof(PointLightUpdateHeader);

	// Write payload
	if (!shared::isLightPositionFixed(pointLight.dynMask)) {
		*reinterpret_cast<glm::vec3*>(buffer + offset + written) = pointLight.position;
		written += sizeof(glm::vec3);
	}
	if (!shared::isLightColorFixed(pointLight.dynMask)) {
		*reinterpret_cast<glm::vec3*>(buffer + offset + written) = pointLight.color;
		written += sizeof(glm::vec3);
	}
	if (!shared::isLightIntensityFixed(pointLight.dynMask)) {
		*reinterpret_cast<float*>(buffer + offset + written) = pointLight.intensity;
		written += sizeof(float);
	}

	// Update size in header
	reinterpret_cast<UdpHeader*>(buffer)->size += written;
	verbose("Packet size is now ", reinterpret_cast<UdpHeader*>(buffer)->size);

	dumpFullPacket(buffer, bufsize, LOGLV_DEBUG);

	return written;
}

void dumpFullPacket(const uint8_t* buffer, std::size_t bufsize, LogLevel loglv)
{
	const auto header = reinterpret_cast<const UdpHeader*>(buffer);
	log(loglv, true, "header.packetGen:");
	dumpBytes(&header->packetGen, sizeof(uint64_t), 50, loglv);
	log(loglv, true, "header.size:");
	dumpBytes(&header->size, sizeof(uint32_t), 50, loglv);

	const auto type = byte2udpmsg(buffer[sizeof(UdpHeader)]);
	log(loglv, true, "chunk type: 0x", std::hex, int(buffer[sizeof(UdpHeader)]), std::dec, "  (", type, ")");

	switch (type) {
	case UdpMsgType::GEOM_UPDATE: {
		const auto chunkHead =
			reinterpret_cast<const GeomUpdateHeader*>(buffer + sizeof(UdpHeader) + sizeof(UdpMsgType));
		log(loglv, true, "chunkHead.modelId:");
		dumpBytes(&chunkHead->modelId, sizeof(uint32_t), 50, loglv);
		log(loglv, true, "chunkHead.dataType:");
		dumpBytes(&chunkHead->dataType, sizeof(GeomDataType), 50, loglv);
		log(loglv, true, "chunkHead.start:");
		dumpBytes(&chunkHead->start, sizeof(uint32_t), 50, loglv);
		log(loglv, true, "chunkHead.len:");
		dumpBytes(&chunkHead->len, sizeof(uint32_t), 50, loglv);
		log(loglv, true, "payload:");
		dumpBytes(buffer + sizeof(UdpHeader) + sizeof(GeomUpdateHeader), bufsize, 100, loglv);
	} break;
	case UdpMsgType::POINT_LIGHT_UPDATE: {
		const auto chunkHead = reinterpret_cast<const PointLightUpdateHeader*>(
			buffer + sizeof(UdpHeader) + sizeof(UdpMsgType));
		log(loglv, true, "chunkHead.lightId:");
		dumpBytes(&chunkHead->lightId, sizeof(uint32_t), 50, loglv);
		log(loglv, true, "chunkHead.updateMask:");
		dumpBytes(&chunkHead->updateMask, sizeof(uint8_t), 50, loglv);
		log(loglv, true, "payload:");
		dumpBytes(buffer + sizeof(UdpHeader) + sizeof(PointLightUpdateHeader), bufsize, 50, loglv);
	} break;
	default:
		break;
	}
}
